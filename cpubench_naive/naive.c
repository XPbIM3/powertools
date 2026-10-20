/*
 * naive.c - deliberately dumb FP/INT/MEM mixing loops, built with -O0.
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
 * MEM phase (RAM tier): naive copy bandwidth + permuted-walk latency.
 *   Window 64 MB = 16 777 216 words (unsigned = 4 B everywhere in the
 *   park) - far beyond any park cache (7450/970FX: 512K L2, Core 2: 4M),
 *   so a single RAM-tier reading, no ladder.
 *   - copy: dst[i] = src[i], time-based, counted per 64 MB pass, clock
 *     checked once per pass (a pass is ~0.2-0.5 s - chunky enough);
 *     in -n mode: 32 fixed passes (deterministic for the checksum).
 *   - latency: single-cycle permutation (Sattolo, built in-place by the
 *     same LCG) over all MEM_WORDS nodes, then ONE full walk p = next[p].
 *     Fixed work on every machine (~5-11 s) because RAM latency, not CPU
 *     clock, dominates - no -t, no overshoot. ns/hop honestly includes
 *     the -O0 loop bookkeeping ("dirty latency", same rules everywhere).
 *   - one warm pass runs untimed before each phase (page faults of the
 *     first touch on small-RAM machines stay out of the measurement).
 *   - buffers: 2 x 64 MB (~128 MB peak - fine on a 384 MB machine when
 *     measured quiet). Alloc failure -> honest skip, FP/INT untouched.
 *
 * -t mode measures the rate; pass counts (and hence checksums) differ per
 * machine. -n mode runs an exact pass count: checksums then are the
 * cross-machine proof that "the same rules" executed everywhere.
 *
 * usage: naive [-t seconds | -n passes]   (default: -t 10 per phase;
 * -n also pins MEM to 32 copy passes + the always-fixed full walk)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/sysctl.h>

#define NL            8
#define CHUNK         1024
#define OPS_PER_PASS  (NL * 2)
#define MEM_BYTES     (64UL << 20)
#define MEM_WORDS     (MEM_BYTES / sizeof (unsigned))
#define MEM_N_PASSES  32      /* -n mode: fixed copy passes */

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
    unsigned long long mem_passes = 0, copychk = 0, walkchk = 0;
    double budget = 10.0, t0, tf, ti, mt = 0, mt2 = 0;
    unsigned *msrc = NULL, *mdst = NULL;
    int mem_ok = 0;
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
               "%d ops/pass (+MEM %d copy passes / full walk)\n\n",
               n_fixed, NL, OPS_PER_PASS, MEM_N_PASSES);
    else
        printf("naive -O0: FP %.0fs + INT %.0fs + MEM copy %.0fs + walk, "
               "%d lanes, %d ops/pass, chunk %d\n\n", budget, budget,
               budget, NL, OPS_PER_PASS, CHUNK);

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

    /* ---- MEM phase: 64 MB RAM-tier copy + full permuted walk ---- */
    msrc = malloc(MEM_BYTES);
    mdst = malloc(MEM_BYTES);
    if (!msrc || !mdst) {
        free(msrc);
        free(mdst);
        msrc = mdst = NULL;
        printf("  MEM: skipped - %lu MB buffers not available\n",
               MEM_BYTES / (1024 * 1024));
    } else {
        unsigned seed = 0x9E3779B9u, j, tmp;
        unsigned long i, h;      /* h: the walk hop / word index */

        /* src fill (deterministic LCG) + one untimed warm copy pass,
         * so page faults of the first touch stay out of the timing */
        for (i = 0; i < MEM_WORDS; i++)
            msrc[i] = seed = seed * 1664526u + 1013904223u;
        for (i = 0; i < MEM_WORDS; i++)
            mdst[i] = msrc[i];

        t0 = now();
        mem_passes = 0;
        do {                     /* naive copy, clock once per pass */
            for (i = 0; i < MEM_WORDS; i++)
                mdst[i] = msrc[i];
            mem_passes++;
        } while (n_fixed ? mem_passes < MEM_N_PASSES
                         : !timed_out(t0, budget));
        mt = now() - t0;

        for (i = 0; i < MEM_WORDS; i++)      /* proof of real copy */
            copychk ^= mdst[i];

        /* latency: Sattolo shuffle (single cycle) over node index,
         * built in-place in msrc (untimed), then one full walk */
        for (i = 0; i < MEM_WORDS; i++)
            msrc[i] = (unsigned)i;
        for (i = MEM_WORDS - 1; i > 0; i--) {
            seed = seed * 1664526u + 1013904223u;
            j = seed % (unsigned)i;          /* Sattolo: j in [0, i-1] */
            tmp = msrc[i];
            msrc[i] = msrc[j];
            msrc[j] = tmp;
        }

        t0 = now();
        h = 0;
        p = 0;
        do {                     /* dependent load chain, full cycle */
            p = msrc[p];
            if ((h & 4095u) == 4095u)
                walkchk ^= (unsigned long long)p;
            h++;
        } while (h < MEM_WORDS);
        mt2 = now() - t0;
        if (p != 0)
            printf("  MEM: WARNING single-cycle walk left p=%u - "
                   "permutation bug\n", p);
        free(msrc);
        free(mdst);
        mem_ok = 1;
    }

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
    if (!mem_ok) {
        printf("  MEM: skipped\n");
        printf("  checksum: fp %016llx  int %016llx\n", fpchk, intchk);
        return 0;
    }
    printf("  MEM: %7.1f MB/s   copy %6.2f s %12llu passes (64M window)\n",
           (double)mem_passes * (double)MEM_BYTES / mt / 1e6, mt,
           mem_passes);
    printf("  MEM: %7.1f ns/hop     %6.2f s %12llu hops   (full walk)\n",
           mt2 * 1e9 / (double)MEM_WORDS, mt2,
           (unsigned long long)MEM_WORDS);
    printf("  checksum: fp %016llx  int %016llx  mem %016llx\n",
           fpchk, intchk, copychk ^ walkchk);
    return 0;
}
