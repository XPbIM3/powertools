/*
 * naive.c - deliberately dumb FP/INT mixing loops, built with -O0.
 *
 * Time-based "dirty throughput" (-t) or fixed-pass determinism (-n):
 * FP phase then INT phase, each phase counts COMPLETED passes;
 * ops achieved / elapsed = the metric. No unrolling, no stack/register
 * sizing, no special opcodes, no per-arch compute - the only per-arch part
 * is what gets PRINTED. At -O0 every local lives on the stack, so each op
 * is load-op-store: the number is "what naive C does", not the FPU/ALU
 * peak. Same rules for G3/G4/G5/Intel.
 *
 *   FP : 8 lanes  a[k] = (a[k] + a[(k+1) & 7]) * x[k]  (1 add + 1 mul = 2 ops)
 *        Mean-with-neighbour has spectral radius EXACTLY 1 (bounded);
 *        x[k] = 0.5 + (k+1)*1e-9 perturbs it so the scale drifts no faster
 *        than e^(2e-8 * passes) - overflow horizon >1e10 passes,
 *        unreachable in any timed window. Values stay positive forever,
 *        no zero trap, no denormals.
 *   INT: 8 lanes  u[k] = u[k] * 1664526u + u[(k+1) & 7]  (1 mul + 1 add = 2)
 *        32-bit LCG with cross-lane term. Transition matrix has
 *        det = c^8 - 1, and even c makes it ODD -> invertible mod 2^32 ->
 *        a nonzero orbit can never reach the zero fixed point or saturate
 *        (the naive u <- 2u+1 jams at 0xFFFFFFFF after 32 passes - exactly
 *        what we avoid).
 *
 * Clock is checked every 1024 passes, NOT every pass: gettimeofday is a
 * ~1 us syscall and at -O0 a pass is of comparable cost, so per-pass checks
 * would measure the syscall. Chunk overshoot < 0.1 %.
 *
 * A "1 op" is one mul OR one add: 16 ops per pass in BOTH phases
 * (internally identical op mix).
 *
 * -t mode measures the rate; pass counts (and hence checksums) differ per
 * machine. -n mode runs an exact pass count: checksums then are the
 * cross-machine proof that "the same rules" executed everywhere.
 *
 * usage: naive [-t seconds | -n passes]     (default: -t 10 per phase)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/sysctl.h>

#define NL            8
#define CHUNK         1024
#define OPS_PER_PASS  (NL * 2)

static double now(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

static int timed_out(double t0, double budget)
{
    return (now() - t0) >= budget;
}

static unsigned long long cpu_mhz(void)
{
    unsigned long long f = 0;
    size_t len = sizeof f;

    if (sysctlbyname("hw.cpufrequency", &f, &len, NULL, 0) != 0 || !f)
        return 0;
    if (f > 4294967295ull && (f & 0xffffffffull) == 0)
        f >>= 32;                       /* Leopard sometimes reports Hz<<32 */
    return f / 1000000ull;
}

static void print_machine(void)
{
    char model[64];
    size_t len = sizeof model;

    if (sysctlbyname("hw.model", model, &len, NULL, 0) == 0) {
        model[sizeof model - 1] = 0;
        printf("machine: %s\n", model);
    }
#if defined(__i386__) || defined(__x86_64__)
    {
        char brand[128];
        size_t blen = sizeof brand;
        if (sysctlbyname("machdep.cpu.brand_string", brand, &blen,
                         NULL, 0) == 0) {
            brand[sizeof brand - 1] = 0;
            printf("cpu:     %s\n", brand);
        }
    }
#else
    {
        /* Constants verified against /usr/include/mach/machine.h
         * (Tiger 10.4): 60x=1..8, 750=9, 7400=10, 7450=11, 970=100. */
        int st = -1;
        size_t slen = sizeof st;
        const char *gen = NULL, *fam = NULL;

        if (sysctlbyname("hw.cpusubtype", &st, &slen, NULL, 0) == 0) {
            switch (st) {
            case  9:  gen = "G3"; fam = "750";         break;
            case 10:  gen = "G4"; fam = "7400";        break;
            case 11:  gen = "G4"; fam = "7450 family"; break;
            case 100: gen = "G5"; fam = "970";         break;
            }
        }
        if (st >= 0) {
            if (gen)
                printf("cpu:     %s (%s, subtype %d)\n", gen, fam, st);
            else
                printf("cpu:     PowerPC (subtype %d)\n", st);
        } else
            printf("cpu:     PowerPC\n");
    }
#endif
    printf("freq:    %llu MHz\n", cpu_mhz());
}

int main(int argc, char **argv)
{
    double x[NL], a[NL];
    unsigned u[NL];
    unsigned k, p;
    unsigned long long fp_passes = 0, int_passes = 0, mhz;
    unsigned long long fpchk = 0, intchk = 0, fpops, intops, n_fixed = 0;
    double budget = 10.0, t0, tf, ti;
    union { double d; unsigned long long u; } cv;

    if (argc == 3 && strcmp(argv[1], "-t") == 0 && atof(argv[2]) > 0) {
        budget = atof(argv[2]);
    } else if (argc == 3 && strcmp(argv[1], "-n") == 0) {
        n_fixed = strtoull(argv[2], NULL, 0);
        n_fixed = (n_fixed + CHUNK - 1) / CHUNK * CHUNK; /* whole chunks */
    } else if (argc != 1) {
        fprintf(stderr, "usage: naive [-t seconds | -n passes]\n");
        return 1;
    }

    for (k = 0; k < NL; k++) {
        x[k] = 0.5 + (double)(k + 1) * 1e-9;
        a[k] = 1.0 + (double)k;
        u[k] = 0x9E3779B9u + (unsigned)k * 2654435761u;
    }

    print_machine();
    if (n_fixed)
        printf("naive -O0: -n %llu passes per phase, %d lanes, "
               "%d ops/pass\n\n", n_fixed, NL, OPS_PER_PASS);
    else
        printf("naive -O0: FP %.0fs + INT %.0fs, %d lanes, %d ops/pass, "
               "chunk %d\n\n", budget, budget, NL, OPS_PER_PASS, CHUNK);

    t0 = now();
    do {
        for (p = 0; p < CHUNK; p++)
            for (k = 0; k < NL; k++)
                a[k] = (a[k] + a[(k + 1) & (NL - 1)]) * x[k];
        fp_passes += CHUNK;
    } while (n_fixed ? fp_passes < n_fixed : !timed_out(t0, budget));
    tf = now() - t0;

    t0 = now();
    do {
        for (p = 0; p < CHUNK; p++)
            for (k = 0; k < NL; k++)
                u[k] = u[k] * 1664526u + u[(k + 1) & (NL - 1)];
        int_passes += CHUNK;
    } while (n_fixed ? int_passes < n_fixed : !timed_out(t0, budget));
    ti = now() - t0;

    fpops  = fp_passes * (unsigned long long)OPS_PER_PASS;
    intops = int_passes * (unsigned long long)OPS_PER_PASS;
    mhz    = cpu_mhz();

    for (k = 0; k < NL; k++) {
        cv.d = a[k];
        fpchk ^= cv.u;
    }
    for (k = 0; k < NL; k++)
        intchk ^= (unsigned long long)u[k] << ((k & 7) * 8);

    printf("  FP : %7.3f GFLOP/s %7.2f s %12llu passes %7.3f op/cycle\n",
           (double)fpops / tf / 1e9, tf, fp_passes,
           mhz ? (double)fpops / tf / ((double)mhz * 1e6) : 0.0);
    printf("  INT: %7.3f Gop/s   %7.2f s %12llu passes %7.3f op/cycle\n",
           (double)intops / ti / 1e9, ti, int_passes,
           mhz ? (double)intops / ti / ((double)mhz * 1e6) : 0.0);
    printf("  checksum: fp %016llx  int %016llx\n", fpchk, intchk);
    return 0;
}
