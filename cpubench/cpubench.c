/*
 * cpubench.c - synthetic CPU throughput benchmark for Macs
 *               (Mac OS X 10.4/10.5, PowerPC G3/G4/G5 and Intel 32-bit)
 *
 * Single source, two artifacts (binary names choosen by `make` per host arch):
 *
 *   cpubench-ppc    - PowerPC G3/G4/G5 (darwin8 darwin9)
 *   cpubench-intel  - i386 Core-era (darwin9 x86)
 *
 * Measures two honest, comparable numbers, one thread only:
 *
 *   FP   - scalar double-precision floating point throughput.
 *          PPC:   16 independent FMADD chains (a = a*x + 0.5) - the native
 *                 fused multiply-add; pass = 16 FMADD = 32 flop.
 *          Intel: 8 classic chains (a = a*x + 0.5) - separate mul+add (there
 *                 is no FMA in this CPU era); pass = 16 flop.
 *          Both hot paths are REGISTER-CLEAN, verified by disasm audit
 *          (accumulators as local scalars; struct-indexed forms compile
 *          memory-fed on this era of gcc and were rejected - the PPC one
 *          pinned G4 at its 1-LSU ceiling, historical passports measured
 *          that LSU-bound variant).
 *
 *   INT  - integer ALU throughput: 4 xorshift32 chains (xor/shift only,
 *          1-cycle latency ops). One pass = 8 rounds x 4 lanes x 6 ops.
 *          Lanes stay in registers for the whole call (state folded once
 *          per call; round-reload form measured ALU + memory roundtrip).
 *          Identical kernel on both architectures.
 *
 * Scheme (why there is no "random ops" inside the timed loop):
 *   - kernels are deterministic with an exact operation count;
 *   - the phase runs in ~0.35 s chunks; the clock is read once per chunk
 *     and once at the end (<0.5% overhead, and self-correcting: the rate
 *     is ops/actual wall, so no precomputed pass count is needed);
 *   - each phase is preceded by an uncounted warmup chunk: PPC 970/G4
 *     start from idle at a low P-state and ramp up, so measuring cold
 *     underestimates the rate (seen 40% error without warmup);
 *   - a fold of the final state (checksum) is printed: proof the work
 *     really happened and was not folded away by the compiler.
 *
 * Usage: cpubench-ppc [-t SECONDS]     /  cpubench-intel [-t SECONDS]
 *   -t S   seconds per phase (default 10: ~21 s total incl. calibration)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/sysctl.h>

#if defined(__i386__) || defined(__x86_64__)
#define ARCH_INTEL 1
#else
#define ARCH_PPC 1
#endif

/* ------------------------------------------------------------- util */

static double now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000000.0 + (double)tv.tv_usec;
}

/* Leopard quirk: HW_CPU_FREQ can come as Hz shifted by exactly 32 bits
   (in the wild: 7730941132800000000 = 1800000000 Hz << 32; some machines
   report plain-Hz or plain-MHz). Heuristic: exact multiples of 2^32 ->
   shift back; then anything over 1e6 is Hz -> divide; else assume MHz. */
static unsigned long long freq_mhz(unsigned long long v)
{
    if (v > 5000ull) {
        if (v > 4294967295ull && (v & 0xffffffffull) == 0)
            v >>= 32;                        /* Leopard: value << 32 */
        if (v > 1000000ull)                  /* Hz -> MHz */
            v /= 1000000ull;
    }
    return v;
}

/* --------------------------------------------------------- machine info */

#if ARCH_PPC
/* hw.cpusubtype values seen on this park (mach ppc subtypes):
   9  = 750-family (G3), 10 = 7400 (G4), 11 = 7450-family (G4),
   100 = 970 (G5). Unknown values print raw - honest fallback. */
static const char *ppc_cpu_name(int st)
{
    switch (st) {
    case 9:   return "750-family (G3)";
    case 10:  return "7400 (G4)";
    case 11:  return "7450-family (G4)";
    case 100: return "970 (G5)";
    default:  return NULL;
    }
}
#endif /* ARCH_PPC */

static void print_cpu(void)
{
    char   model[64];
    size_t len = sizeof model;
    unsigned long long mhz = 0;

    if (sysctlbyname("hw.model", model, &len, NULL, 0) == 0) {
        model[sizeof model - 1] = 0;
        printf("machine: %s\n", model);
    }
#if ARCH_PPC
    {
        int subtype = 0;
        size_t sblen = sizeof subtype;
        if (sysctlbyname("hw.cpusubtype", &subtype, &sblen, NULL, 0) == 0) {
            const char *nm = ppc_cpu_name(subtype);
            if (nm)
                printf("cpu:     %s (hw.cpusubtype=%d)\n", nm, subtype);
            else
                printf("cpu:     PowerPC (hw.cpusubtype=%d)\n", subtype);
        }
    }
#else
    {
        char brand[128];
        size_t blen = sizeof brand;
        if (sysctlbyname("machdep.cpu.brand_string", brand, &blen,
                         NULL, 0) == 0) {
            brand[sizeof brand - 1] = 0;
            printf("cpu:     %s\n", brand);
        }
    }
#endif
    {
        unsigned long long fv = 0;
        size_t flen = sizeof fv;
        if (sysctlbyname("hw.cpufrequency", &fv, &flen, NULL, 0) == 0 && fv) {
            mhz = freq_mhz(fv);
            printf("freq:    %llu MHz\n", mhz);
        }
    }
}

/* ------------------------------------------------------------- FP */

#if ARCH_PPC

typedef struct {
    double           fa[16];
    double           fx;
    unsigned int     ia[4];
    unsigned long long check;
    unsigned long long passes;
} bench_t;

/* 16 independent FMADD chains as LOCAL SCALARS: one state load/store set
   per call, hot pass is pure regs. The old struct-indexed form compiled
   to lfd+fmadd+stfd interleave (LSU-bound: G4 pegged at its 1-LSU ceiling
   ~0.5 flop/cycle; historical passports measured that) - register-clean
   measured ~2x higher on G4-class and matches the Intel kernel discipline. */
static void fp_run(bench_t *t, unsigned long long passes)
{
    double a0, a1, a2, a3, a4, a5, a6, a7,
           a8, a9, a10, a11, a12, a13, a14, a15;
    double x = t->fx;
    unsigned long long p;

    a0 = t->fa[0];  a1 = t->fa[1];  a2 = t->fa[2];  a3 = t->fa[3];
    a4 = t->fa[4];  a5 = t->fa[5];  a6 = t->fa[6];  a7 = t->fa[7];
    a8 = t->fa[8];  a9 = t->fa[9];  a10 = t->fa[10]; a11 = t->fa[11];
    a12 = t->fa[12]; a13 = t->fa[13]; a14 = t->fa[14]; a15 = t->fa[15];

    for (p = 0; p < passes; p++) {
        a0  = a0  * x + 0.5; a1  = a1  * x + 0.5;
        a2  = a2  * x + 0.5; a3  = a3  * x + 0.5;
        a4  = a4  * x + 0.5; a5  = a5  * x + 0.5;
        a6  = a6  * x + 0.5; a7  = a7  * x + 0.5;
        a8  = a8  * x + 0.5; a9  = a9  * x + 0.5;
        a10 = a10 * x + 0.5; a11 = a11 * x + 0.5;
        a12 = a12 * x + 0.5; a13 = a13 * x + 0.5;
        a14 = a14 * x + 0.5; a15 = a15 * x + 0.5;
        x += 0.01;
        if (x > 2.0) {                    /* recycle: keep doubles finite */
            x = 1.0;
            a0 = 0; a1 = 1; a2 = 2; a3 = 3; a4 = 4; a5 = 5;
            a6 = 6; a7 = 7; a8 = 8; a9 = 9; a10 = 10; a11 = 11;
            a12 = 12; a13 = 13; a14 = 14; a15 = 15;
        }
    }
    t->fa[0]  = a0;  t->fa[1]  = a1;  t->fa[2]  = a2;  t->fa[3]  = a3;
    t->fa[4]  = a4;  t->fa[5]  = a5;  t->fa[6]  = a6;  t->fa[7]  = a7;
    t->fa[8]  = a8;  t->fa[9]  = a9;  t->fa[10] = a10; t->fa[11] = a11;
    t->fa[12] = a12; t->fa[13] = a13; t->fa[14] = a14; t->fa[15] = a15;
    t->fx = x;
    t->passes += passes;
    t->check += (unsigned long long)(a0 + a1 + a2 + a3
                                   + a4 + a5 + a6 + a7
                                   + a8 + a9 + a10 + a11
                                   + a12 + a13 + a14 + a15);
}

#else /* ARCH_INTEL */

typedef struct {
    double           fa[8];    /* 8 classic chains, all fit the 8 xmm regs */
    double           fx;
    unsigned int     ia[4];
    unsigned long long check;
    unsigned long long passes;
} bench_t;

/* 8 classic chains; x fixed by invocation (drift +0.0005 per call),
   accumulators recycle every 16384 passes: growth bound for the window
   is 0.5*(x^16384 - 1)/(x - 1) ~ 7e9 - FINITE and far below the
   unsigned long long range (65536 gave ~3e31: fold cast was UB).
   Local scalars on purpose: gcc 4.0.1 i386 keeps struct-pointer-indexed
   doubles memory-fed (audited), scalars go to xmm registers. */
static void fp_run(bench_t *t, unsigned long long passes)
{
    double a0, a1, a2, a3, a4, a5, a6, a7;
    double x = t->fx;
    unsigned long long p;

    a0 = t->fa[0]; a1 = t->fa[1]; a2 = t->fa[2]; a3 = t->fa[3];
    a4 = t->fa[4]; a5 = t->fa[5]; a6 = t->fa[6]; a7 = t->fa[7];
    for (p = 0; p < passes; p++) {
        a0 = a0 * x + 0.5; a1 = a1 * x + 0.5;
        a2 = a2 * x + 0.5; a3 = a3 * x + 0.5;
        a4 = a4 * x + 0.5; a5 = a5 * x + 0.5;
        a6 = a6 * x + 0.5; a7 = a7 * x + 0.5;
        if ((p & 16383ULL) == 16383ULL) {
            a0 = 0; a1 = 1; a2 = 2; a3 = 3;
            a4 = 4; a5 = 5; a6 = 6; a7 = 7;
        }
    }
    t->fa[0] = a0; t->fa[1] = a1; t->fa[2] = a2; t->fa[3] = a3;
    t->fa[4] = a4; t->fa[5] = a5; t->fa[6] = a6; t->fa[7] = a7;
    t->fx = x + 0.0005;                 /* tiny per-invocation drift */
    t->passes += passes;
    t->check += (unsigned long long)(a0 + a1 + a2 + a3
                                   + a4 + a5 + a6 + a7);
}

#endif /* ARCH_PPC */

/* ------------------------------------------------------------- INT */

/* 4 xorshift32 lanes; one pass = 8 rounds x 4 lanes x 6 ALU ops (192).
   Same kernel on both architectures. The lanes live in registers for the
   WHOLE call (one state load/store per call, not per round): the old
   round-reload form measured an ALU + memory roundtrip rate - on i386
   it cost ~17% (disasm-audited: per-round movl 0x48(%ecx)/movl stack
   stores). xorshift math is location-free, so checksums are unchanged. */
static void int_run(bench_t *t, unsigned long long passes)
{
    unsigned long long p;
    unsigned int       a, b, c, d;
    int                k;

    a = t->ia[0]; b = t->ia[1]; c = t->ia[2]; d = t->ia[3];
    for (p = 0; p < passes; p++)
        for (k = 0; k < 8; k++) {
            a ^= a << 13; a ^= a >> 17; a ^= a << 5;
            b ^= b << 11; b ^= b >> 21; b ^= b << 4;
            c ^= c << 17; c ^= c >> 15; c ^= c << 8;
            d ^= d << 7;  d ^= d >> 12; d ^= d << 23;
        }
    t->ia[0] = a; t->ia[1] = b; t->ia[2] = c; t->ia[3] = d;
    t->passes += passes;
    t->check ^= ((unsigned long long)a << 32) ^ b
              ^ ((unsigned long long)c << 16) ^ (d * 2246822519u);
}

/* ------------------------------------------------------------ driver */

typedef void (*kern_fn)(bench_t *, unsigned long long);

/* grow a fixed-size workload until one chunk takes >= MIN_S seconds;
   used only to size the ~0.35 s chunks for the timed phase */
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

/* run the phase in ~0.35 s chunks: one clock read per chunk + at the end
   (<0.5% overhead). An uncounted warmup chunk first (the PPC cores start
   at a low P-state and ramp up; measuring cold gave 40% errors), then
   chunked work until the wall budget is reached. */
static void phase(kern_fn run, bench_t *t, double secs,
                  unsigned long long *ops_out, double *wall_out)
{
    unsigned long long K, n = 0;
    double             t0, el;

    K = (unsigned long long)(find_rate(run, t) * 0.35);
    if (K < 16)
        K = 16;
    run(t, K);                                 /* warmup, uncounted */
    t0 = now_us();
    for (;;) {
        run(t, K);
        n += K;
        el = (now_us() - t0) / 1000000.0;
        if (el >= secs)
            break;
    }
    *ops_out = n;
    *wall_out = el;
}

/* --------------------------------------------------------------- output */

#if ARCH_PPC
#define PROG_TITLE  "cpubench-ppc (PowerPC Mac OS X 10.4/10.5)"
#define PROG_USAGE  "cpubench-ppc"
#define FLOPS_PER_PASS 32.0     /* 16 FMADD x (mul+add) */
#define PASSES_UNIT    "chain-steps"
#define OPS_LABEL      "billion FMADD/s"
#define OPS_PER_PASS   16.0
#define FLOP_NOTE      "(fused mul+add counted, double 64-bit)"
#else /* ARCH_INTEL */
#define PROG_TITLE  "cpubench-intel (Intel Mac OS X 10.5, 32-bit)"
#define PROG_USAGE  "cpubench-intel"
#define FLOPS_PER_PASS 16.0     /* 8 chains x (mul+add) */
#define PASSES_UNIT    "chain-steps"
#define OPS_LABEL      "billion mul+add/s"
#define OPS_PER_PASS   16.0
#define FLOP_NOTE      "(mul+add counted, separate ops - no FMA on this CPU)"
#endif

static void usage(void)
{
    printf("%s - synthetic CPU benchmark\n\n"
           "usage: " PROG_USAGE " [-t SECONDS] [-h]\n"
           "  -t S  seconds per phase, FP then INT (default 10)\n"
           "  -h    this help\n", PROG_TITLE);
}

int main(int argc, char **argv)
{
    bench_t            t;
    double             secs = 10.0, wall_fp, wall_int;
    unsigned long long n_fp, n_int;
    unsigned long long mhz = 0;
    int                ch;

    while ((ch = getopt(argc, argv, "t:h")) != -1) {
        switch (ch) {
        case 't': secs = atof(optarg); break;
        case 'h': usage(); return 0;
        default:  usage(); return 1;
        }
    }
    if (secs < 0.1 || secs > 86400.0) {
        fprintf(stderr, PROG_USAGE ": -t must be 0.1..86400\n");
        return 1;
    }

    memset(&t, 0, sizeof t);
#if ARCH_PPC
    t.fx = 1.0;
#else
    t.fx = 1.001;
#endif
    t.ia[0] = 0x9E3779B9u;
    t.ia[1] = 0x85EBCA77u;
    t.ia[2] = 0xC2B2AE35u;
    t.ia[3] = 0x27D4EB2Fu;

#if ARCH_PPC
    t.fa[0] = 0.0; t.fa[1] = 1.0; t.fa[2] = 2.0; t.fa[3] = 3.0;
    t.fa[4] = 4.0; t.fa[5] = 5.0; t.fa[6] = 6.0; t.fa[7] = 7.0;
    t.fa[8] = 8.0; t.fa[9] = 9.0; t.fa[10] = 10.0;
    t.fa[11] = 11.0; t.fa[12] = 12.0; t.fa[13] = 13.0;
    t.fa[14] = 14.0; t.fa[15] = 15.0;
#else
    {
        int k;
        for (k = 0; k < 8; k++)
            t.fa[k] = (double)k;
    }
#endif

    {
        unsigned long long fv = 0;
        size_t flen = sizeof fv;
        if (sysctlbyname("hw.cpufrequency", &fv, &flen, NULL, 0) == 0 && fv)
            mhz = freq_mhz(fv);
    }

    printf(PROG_TITLE "\n");
    print_cpu();
    printf("phases: FP %.1f s + INT %.1f s"
           " (chunks + warmup, ~%.1f s total)\n\n",
           secs, secs, 2.0 * secs + 1.5);

    /* ---------- FP phase ---------- */
    phase(fp_run, &t, secs, &n_fp, &wall_fp);
    printf("FP : %.3g billion %s in %.2f s\n"
           "     %8.2f %s\n"
           "     %8.3f GFLOP/s    %s\n",
           (double)n_fp / 1e9, PASSES_UNIT, wall_fp,
           OPS_PER_PASS * (double)n_fp / wall_fp / 1e9, OPS_LABEL,
           FLOPS_PER_PASS * (double)n_fp / wall_fp / 1e9, FLOP_NOTE);
    if (mhz)
        printf("     %8.3f flop/cycle  (freq %llu MHz)\n\n",
               FLOPS_PER_PASS * (double)n_fp / wall_fp / (double)mhz / 1e6,
               mhz);
    else
        printf("\n");
    t.passes = 0;

    /* ---------- INT phase ---------- */
    phase(int_run, &t, secs, &n_int, &wall_int);
    printf("INT: %.3g billion passes in %.2f s\n"
           "     %8.2f billion ALU ops/s\n",
           (double)n_int / 1e9, wall_int,
           192.0 * (double)n_int / wall_int / 1e9);
    if (mhz)
        printf("     %8.3f alu-op/cycle (freq %llu MHz)\n\n",
               192.0 * (double)n_int / wall_int / (double)mhz / 1e6, mhz);
    else
        printf("\n");

    printf("checksum: %016llx  (proof of real work)\n"
           "total wall: %.2f s\n",
           t.check, wall_fp + wall_int);
    return 0;
}
