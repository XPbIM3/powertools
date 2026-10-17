/*
 * cpubench.c - synthetic CPU throughput benchmark for PowerPC Macs
 *               (Mac OS X 10.4/10.5, G4/G5)
 *
 * Measures two honest, comparable numbers:
 *
 *   FP  - scalar double-precision floating point throughput:
 *         16 independent FMADD chains (a = a*x + 0.5), the native G4/G5
 *         fused multiply-add. One pass = 16 FMADD = 32 flop (mul+add).
 *         Reported as FMADD/s and GFLOP/s.
 *
 *   INT - integer ALU throughput: 4 xorshift32 chains (xor/shift only,
 *         1-cycle latency ops). One pass = 8 rounds x 4 lanes x 6 ops.
 *         Reported as ALU ops/s.
 *
 * Scheme (why there is no "random ops" inside the timed loop):
 *   - kernels are deterministic with an exact operation count;
 *   - there are NO clock reads inside the hot loop (gettimeofday can
 *     cost more than the measured work on these machines);
 *   - instead a short calibration run measures the pass rate, then the
 *     timed phase executes a precomputed pass count with wall-clock
 *     taken only around the whole phase;
 *   - a fold of the final state (checksum) is printed: proof the work
 *     really happened and was not folded away by the compiler.
 *
 * Usage: cpubench [-t SECONDS] [-h]
 *   -t S   seconds per phase (default 10: ~21 s total incl. calibration)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/sysctl.h>

typedef struct {
    double           fa[16];
    double           fx;
    unsigned int     ia[4];
    unsigned long long check;
    unsigned long long passes;
} bench_t;

/* ------------------------------------------------------------- util */

static double now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000000.0 + (double)tv.tv_usec;
}

/* ------------------------------------------------------------- FP */

/* 16 independent FMADD chains; gcc (4.0.1, -O2) folds a*x+.5 into fmadd */
#define FP_PASS(t) \
    ((t)->fa[0]  = (t)->fa[0]  * x + 0.5, (t)->fa[1]  = (t)->fa[1]  * x + 0.5, \
     (t)->fa[2]  = (t)->fa[2]  * x + 0.5, (t)->fa[3]  = (t)->fa[3]  * x + 0.5, \
     (t)->fa[4]  = (t)->fa[4]  * x + 0.5, (t)->fa[5]  = (t)->fa[5]  * x + 0.5, \
     (t)->fa[6]  = (t)->fa[6]  * x + 0.5, (t)->fa[7]  = (t)->fa[7]  * x + 0.5, \
     (t)->fa[8]  = (t)->fa[8]  * x + 0.5, (t)->fa[9]  = (t)->fa[9]  * x + 0.5, \
     (t)->fa[10] = (t)->fa[10] * x + 0.5, (t)->fa[11] = (t)->fa[11] * x + 0.5, \
     (t)->fa[12] = (t)->fa[12] * x + 0.5, (t)->fa[13] = (t)->fa[13] * x + 0.5, \
     (t)->fa[14] = (t)->fa[14] * x + 0.5, (t)->fa[15] = (t)->fa[15] * x + 0.5)

static void fp_run(bench_t *t, unsigned long long passes)
{
    double x = t->fx;
    unsigned long long p;

    for (p = 0; p < passes; p++) {
        FP_PASS(t);
        x += 0.03125;
        if (x > 16.0) {                    /* recycle: keep doubles finite */
            int k;
            x = 1.0;
            for (k = 0; k < 16; k++)
                t->fa[k] = (double)k;
        }
    }
    t->fx = x;
    t->passes += passes;
    t->check += (unsigned long long)(t->fa[0] + t->fa[1] + t->fa[2] + t->fa[3]
                                   + t->fa[4] + t->fa[5] + t->fa[6] + t->fa[7]
                                   + t->fa[8] + t->fa[9] + t->fa[10] + t->fa[11]
                                   + t->fa[12] + t->fa[13] + t->fa[14] + t->fa[15]);
}

/* ------------------------------------------------------------- INT */

/* 4 xorshift32 lanes; one pass = 8 rounds x 4 lanes x 6 ALU ops (192) */
static void int_run(bench_t *t, unsigned long long passes)
{
    unsigned long long p;
    int                k;

    for (p = 0; p < passes; p++)
        for (k = 0; k < 8; k++) {
            unsigned int a = t->ia[0], b = t->ia[1];
            unsigned int c = t->ia[2], d = t->ia[3];
            a ^= a << 13; a ^= a >> 17; a ^= a << 5;
            b ^= b << 11; b ^= b >> 21; b ^= b << 4;
            c ^= c << 17; c ^= c >> 15; c ^= c << 8;
            d ^= d << 7;  d ^= d >> 12; d ^= d << 23;
            t->ia[0] = a; t->ia[1] = b; t->ia[2] = c; t->ia[3] = d;
        }
    t->passes += passes;
    t->check ^= ((unsigned long long)t->ia[0] << 32) ^ t->ia[1]
              ^ ((unsigned long long)t->ia[2] << 16) ^ (t->ia[3] * 2246822519u);
}

/* ------------------------------------------------------------ driver */

typedef void (*kern_fn)(bench_t *, unsigned long long);

/* grow a fixed-size workload until one chunk takes >= MIN_S seconds,
   then return passes per second (no clocks inside the hot loop) */
#define MIN_S 0.15
static double find_rate(kern_fn run, bench_t *t)
{
    unsigned long long n = 512;
    double             el;

    for (;;) {
        double t0 = now_us();
        run(t, n);
        el = (now_us() - t0) / 1000000.0;
        if (el >= MIN_S)
            return (double)n / el;
        n *= 4;
    }
}

static void print_cpu(void)
{
    int              mib[2];
    char             model[64];
    size_t           len = sizeof model;
    unsigned long long mhz = 0;

    mib[0] = CTL_HW; mib[1] = HW_MODEL;
    if (sysctl(mib, 2, model, &len, NULL, 0) == 0) {
        model[sizeof model - 1] = 0;
        printf("machine: %s", model);
    }
    mib[1] = HW_CPU_FREQ;
    len = sizeof mhz;
    if (sysctl(mib, 2, &mhz, &len, NULL, 0) == 0 && mhz)
        printf(", %llu MHz", mhz / 1000000ull);
    printf("\n");
}

static void usage(void)
{
    printf("cpubench - synthetic CPU benchmark for PowerPC Mac (10.4/10.5)\n\n"
           "usage: cpubench [-t SECONDS] [-h]\n"
           "  -t S  seconds per phase, FP then INT (default 10)\n"
           "  -h    this help\n");
}

int main(int argc, char **argv)
{
    bench_t            t;
    double             phase = 10.0;
    double             rate, wall_fp, wall_int;
    unsigned long long n_fp, n_int;
    int                ch;

    while ((ch = getopt(argc, argv, "t:h")) != -1) {
        switch (ch) {
        case 't': phase = atof(optarg); break;
        case 'h': usage(); return 0;
        default:  usage(); return 1;
        }
    }
    if (phase < 0.1 || phase > 86400.0) {
        fprintf(stderr, "cpubench: -t must be 0.1..86400\n");
        return 1;
    }

    memset(&t, 0, sizeof t);
    t.fx = 1.0;
    t.ia[0] = 0x9E3779B9u;
    t.ia[1] = 0x85EBCA77u;
    t.ia[2] = 0xC2B2AE35u;
    t.ia[3] = 0x27D4EB2Fu;

    printf("cpubench (PowerPC Mac OS X 10.4/10.5)\n");
    print_cpu();
    printf("phases: FP %.1f s + INT %.1f s (+ ~0.5 s calibration)\n\n",
           phase, phase);

    /* ---------- FP phase ---------- */
    rate = find_rate(fp_run, &t);
    n_fp = (unsigned long long)(rate * phase);
    {
        double t0 = now_us();
        fp_run(&t, n_fp);
        wall_fp = (now_us() - t0) / 1000000.0;
        printf("FP : %llu FMADD in %.2f s\n"
               "     %8.2f million FMADD/s\n"
               "     %8.3f GFLOP/s    (mul+add counted, double 64-bit)\n",
               n_fp, wall_fp,
               (double)n_fp / wall_fp / 1e6,
               2.0 * (double)n_fp / wall_fp / 1e9);
    }
    t.passes = 0;

    /* ---------- INT phase ---------- */
    rate = find_rate(int_run, &t);
    n_int = (unsigned long long)(rate * phase);
    {
        double t0 = now_us();
        int_run(&t, n_int);
        wall_int = (now_us() - t0) / 1000000.0;
        printf("INT: %llu passes in %.2f s\n"
               "     %8.2f million ALU ops/s\n",
               n_int, wall_int,
               192.0 * (double)n_int / wall_int / 1e6);
    }

    printf("\nchecksum: %016llx  (proof of real work)\n"
           "total wall: %.2f s\n",
           t.check, wall_fp + wall_int);
    return 0;
}
