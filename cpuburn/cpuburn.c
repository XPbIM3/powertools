/*
 * cpuburn.c - CPU load generator for PowerPC Macs (Mac OS X 10.4/10.5)
 *
 * Standalone: knows nothing about any monitoring tools. It just burns.
 *
 * Design rules for the 970/G4 era:
 *   - the hot loop must keep ALL scalar pipelines busy: 2 FPU (FMADD),
 *     2 integer ALU and the load/store unit. A pure dependency-chained
 *     FP loop stalls the machine into a low-power state and heats
 *     nothing.
 *   - 16 independent double accumulators >> FP latency (~6 cyc)
 *   - integer work is xor/shift based (int mul has 4-5 cyc latency)
 *   - a rolling 8 MB buffer keeps the memory controller awake
 *   - every kernel folds its state into a checksum, printed at the
 *     end: proof the work really happened and was not optimized away
 *
 * Usage: cpuburn [-n THREADS] [-t SECONDS] [-m MODE] [-h]
 *   -n N   threads (default 1; >1 oversubscribes single-core machines)
 *   -t S   duration in seconds (default 600 = 10 min)
 *   -m M   mode: fp | int | mem | mixed (default mixed)
 *
 * Always runs at normal priority and at full duty: no throttling,
 * no scheduling tricks - the whole point is to heat the machine.
 *
 * AltiVec REMOVED (decision 22.10.2026): the default state of this
 * build is WITHOUT AltiVec - all VA code paths and the runtime guard
 * are gone; the binary contains no VA instructions at all, G3-safe
 * by both layers (-mcpu=G3, subtype ppc/ALL).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/sysctl.h>

/* NOTE (22.10.2026): AltiVec removed entirely - no builtins, no guard,
 * no -faltivec. See the header note. */

/* ------------------------------------------------------------ modes */

#define MEM_LEN      (8u << 20)                    /* 8 MB per thread */
#define MEM_STEP     (64u << 10)                   /* advance 64 KB */
#define MEM_WORDS    (MEM_LEN / 4)
#define MEM_TOUCH    (1u << 10)                    /* 4 KB per touch */

#define MODE_FP      0
#define MODE_INT     1
#define MODE_MEM     2
#define MODE_MIXED   3

static const char *mode_names[] = { "fp", "int", "mem", "mixed" };

typedef struct {
    int            id;
    int            mode;
    double         deadline;

    double         busy_us;
    unsigned long long check;

    /* fp state */
    double         fa[16];
    double         fx;

    /* int state: 4 independent xorshift32 chains */
    unsigned int   ia[4];

    /* mem state */
    unsigned int  *mem;
    unsigned int   mpos;
} burn_thread_t;

/* ------------------------------------------------------------- util */

static double now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000000.0 + (double)tv.tv_usec;
}

static unsigned long long bits64(double d)
{
    unsigned long long u;
    memcpy(&u, &d, 8);
    return u;
}

/* ------------------------------------------------------------- fp */

/* 16 independent FMADD chains; x varies per pass:
   gcc 4.0 folds a*x+.5 into fmadd, keeping both FPUs busy */
#define FP_PASS(a, x) \
    ((a)[0]  = (a)[0]  * (x) + 0.5, (a)[1]  = (a)[1]  * (x) + 0.5, \
     (a)[2]  = (a)[2]  * (x) + 0.5, (a)[3]  = (a)[3]  * (x) + 0.5, \
     (a)[4]  = (a)[4]  * (x) + 0.5, (a)[5]  = (a)[5]  * (x) + 0.5, \
     (a)[6]  = (a)[6]  * (x) + 0.5, (a)[7]  = (a)[7]  * (x) + 0.5, \
     (a)[8]  = (a)[8]  * (x) + 0.5, (a)[9]  = (a)[9]  * (x) + 0.5, \
     (a)[10] = (a)[10] * (x) + 0.5, (a)[11] = (a)[11] * (x) + 0.5, \
     (a)[12] = (a)[12] * (x) + 0.5, (a)[13] = (a)[13] * (x) + 0.5, \
     (a)[14] = (a)[14] * (x) + 0.5, (a)[15] = (a)[15] * (x) + 0.5)

static void fp_block(burn_thread_t *t, int passes)
{
    double a[16];
    double x = t->fx;
    int    k;

    memcpy(a, t->fa, sizeof a);
    for (k = 0; k < passes; k++) {
        FP_PASS(a, x);
        x += 0.03125;
    }
    if (x > 16.0) {                /* recycle: keep doubles finite */
        x = 1.0;
        for (k = 0; k < 16; k++)
            a[k] = (double)k;
    }
    t->fx = x;
    memcpy(t->fa, a, sizeof a);
    {
        double s = a[0] + a[1] + a[2] + a[3] + a[4] + a[5] + a[6] + a[7]
                 + a[8] + a[9] + a[10] + a[11] + a[12] + a[13] + a[14] + a[15];
        t->check += bits64(s);
    }
}

static double run_fp(burn_thread_t *t, double budget)
{
    double a[16];
    double x;
    double t0, t1;
    unsigned long long pass = 0;
    int k;

    memcpy(a, t->fa, sizeof a);
    x = t->fx;

    t0 = now_us();
    for (;;) {
        /* 16 passes = 256 FMADD, then a cheap time check */
        for (k = 0; k < 16; k++) {
            FP_PASS(a, x);
            x += 0.03125;
        }
        pass += 16;
        if (x > 16.0) {            /* recycle: keep doubles finite */
            x = 1.0;
            for (k = 0; k < 16; k++)
                a[k] = (double)k;
        }
        if ((pass & 255) == 0) {
            t1 = now_us();
            if (t1 - t0 >= budget)
                break;
        }
    }
    t1 = now_us();
    memcpy(t->fa, a, sizeof a);
    t->fx = x;
    {
        double sum = a[0] + a[1] + a[2] + a[3] + a[4] + a[5] + a[6] + a[7]
                   + a[8] + a[9] + a[10] + a[11] + a[12] + a[13] + a[14] + a[15];
        t->check += bits64(sum);
    }
    return t1 - t0;
}

/* ------------------------------------------------------------ integer */

static void int_block(burn_thread_t *t, int passes)
{
    unsigned int a = t->ia[0], b = t->ia[1], c = t->ia[2], d = t->ia[3];
    int k;

    for (k = 0; k < passes; k++) {
        int j;
        for (j = 0; j < 8; j++) {
            a ^= a << 13; a ^= a >> 17; a ^= a << 5;
            b ^= b << 11; b ^= b >> 21; b ^= b << 4;
            c ^= c << 17; c ^= c >> 15; c ^= c << 8;
            d ^= d << 7;  d ^= d >> 12; d ^= d << 23;
        }
    }
    t->ia[0] = a; t->ia[1] = b; t->ia[2] = c; t->ia[3] = d;
    t->check ^= ((unsigned long long)a << 32) ^ b
              ^ ((unsigned long long)c << 16) ^ (d * 2246822519u);
}

static double run_int(burn_thread_t *t, double budget)
{
    double t0, t1;
    unsigned long long pass = 0;

    t0 = now_us();
    for (;;) {
        int_block(t, 16);          /* 16 passes = 8x4 = 512 ALU ops */
        pass += 16;
        if ((pass & 255) == 0) {
            t1 = now_us();
            if (t1 - t0 >= budget)
                break;
        }
    }
    t1 = now_us();
    return t1 - t0;
}

/* -------------------------------------------------------------- mem */

static void mem_block(burn_thread_t *t)
{
    unsigned int *m = t->mem;
    unsigned int  base = t->mpos;
    unsigned long long s = 0;
    int i;

    for (i = 0; i < (int)MEM_TOUCH; i++) {
        unsigned int v = m[(base + i) & (MEM_WORDS - 1)];
        s += v;
    }
    m[base & (MEM_WORDS - 1)] ^= (unsigned int)((s) & 0xFF);
    t->mpos = (base + (MEM_STEP / 4)) & (MEM_WORDS - 1);
    t->check ^= (s << 13) ^ s;
}

static double run_mem(burn_thread_t *t, double budget)
{
    double t0, t1;
    unsigned long long pass = 0;

    t0 = now_us();
    for (;;) {
        int_block(t, 1);           /* keep ALUs fed between sweeps */
        mem_block(t);
        pass++;
        if ((pass & 15) == 0) {
            t1 = now_us();
            if (t1 - t0 >= budget)
                break;
        }
    }
    t1 = now_us();
    return t1 - t0;
}

/* ----------------------------------------------------------- driver */

static double run_mode(burn_thread_t *t, double budget)
{
    double t0, t1;
    unsigned long long n = 0;

    switch (t->mode) {
    case MODE_FP:   return run_fp(t, budget);
    case MODE_INT:  return run_int(t, budget);
    case MODE_MEM:  return run_mem(t, budget);
    default: break;
    }

    /* mixed: alternate scalar kernels in small batches - fp + int + mem */
    t0 = now_us();
    for (;;) {
        fp_block(t, 4);
        int_block(t, 2);
        if ((n & 31) == 0)
            mem_block(t);
        n++;
        if ((n & 15) == 0) {
            t1 = now_us();
            if (t1 - t0 >= budget)
                break;
        }
    }
    t1 = now_us();
    return t1 - t0;
}

static void *thread_main(void *arg)
{
    burn_thread_t *t = (burn_thread_t *)arg;

    /* full duty: burn until the deadline, no sleep quanta */
    while (now_us() < t->deadline)
        t->busy_us += run_mode(t, t->deadline - now_us());
    return NULL;
}

/* ------------------------------------------------------------- main */

static void usage(void)
{
    int i;
    printf("cpuburn - CPU load generator for PowerPC (Mac OS X 10.4/10.5)\n\n"
           "usage: cpuburn [-n THREADS] [-t SECONDS] [-m MODE] [-h]\n"
           "  -n N   threads (default 1; >1 oversubscribes single-core)\n"
           "  -t S   duration in seconds (default 600 = 10 min)\n"
           "  -m M   mode: ");
    for (i = 0; i < 4; i++)
        printf("%s%s", i ? "/" : "", mode_names[i]);
    printf(" (default mixed)\n"
           "  -h     this help\n\n"
           "Runs at normal priority, full duty (always burning at 100%%).\n");
}

int main(int argc, char **argv)
{
    int            nthreads = 1;
    int            mode = MODE_MIXED;
    double         duration = 600.0;
    double         start, end;
    pthread_t     *pt;
    burn_thread_t *th;
    int            i, ch, mi = MODE_MIXED;

    while ((ch = getopt(argc, argv, "n:t:m:h")) != -1) {
        switch (ch) {
        case 'n': nthreads = atoi(optarg); break;
        case 't': duration = atof(optarg); break;
        case 'm':
            for (mi = 3; mi >= 0; mi--)
                if (strcmp(optarg, mode_names[mi]) == 0)
                    break;
            if (mi < 0) {
                fprintf(stderr, "cpuburn: unknown mode '%s'\n", optarg);
                return 1;
            }
            mode = mi;
            break;
        case 'h': usage(); return 0;
        default:  usage(); return 1;
        }
    }
    if (nthreads < 1 || nthreads > 64) {
        fprintf(stderr, "cpuburn: -n must be 1..64\n");
        return 1;
    }
    if (duration <= 0.0 || duration > 86400.0) {
        fprintf(stderr, "cpuburn: -t must be 0..86400 s\n");
        return 1;
    }

    pt = (pthread_t *)calloc((size_t)nthreads, sizeof(pthread_t));
    th = (burn_thread_t *)calloc((size_t)nthreads, sizeof(burn_thread_t));
    if (!pt || !th) {
        fprintf(stderr, "cpuburn: out of memory\n");
        return 1;
    }

    for (i = 0; i < nthreads; i++) {
        burn_thread_t *t = &th[i];
        t->id = i;
        t->mode = mode;
        memset(t->fa, 0, sizeof t->fa);
        t->fx = 1.0;
        t->ia[0] = 0x9E3779B9u + (unsigned)i;
        t->ia[1] = 0x85EBCA77u - (unsigned)i;
        t->ia[2] = 0xC2B2AE35u + (unsigned)(i << 8);
        t->ia[3] = 0x27D4EB2Fu ^ (unsigned)(i << 16);
        t->mem  = (unsigned int *)malloc(MEM_LEN);
        if (!t->mem) {
            fprintf(stderr, "cpuburn: out of memory\n");
            return 1;
        }
        memset(t->mem, 0, MEM_LEN);
        t->mpos = (unsigned)i * (MEM_STEP / 4);
    }

    printf("cpuburn: mode=%s threads=%d duration=%.1f s\n",
           mode_names[mode], nthreads, duration);

    start = now_us();
    for (i = 0; i < nthreads; i++) {
        th[i].deadline = start + duration * 1000000.0;
        if (pthread_create(&pt[i], NULL, thread_main, &th[i]) != 0) {
            fprintf(stderr, "cpuburn: pthread_create failed\n");
            return 1;
        }
    }
    for (i = 0; i < nthreads; i++)
        pthread_join(pt[i], NULL);
    end = now_us();

    {
        double wall = (end - start) / 1000000.0;
        double busy_total = 0.0;

        for (i = 0; i < nthreads; i++) {
            burn_thread_t *t = &th[i];
            busy_total += t->busy_us;
            printf("  thread %d: busy %.0f/%.0f ms (%.1f%%)  check %016llx\n",
                   i, t->busy_us / 1000.0, wall * 1000.0,
                   100.0 * t->busy_us / (wall * 1000000.0),
                   t->check);
        }
        printf("  wall %.2f s, aggregate busy %.2f thread-s, all-work-done\n",
               wall, busy_total / 1000000.0);
    }

    for (i = 0; i < nthreads; i++) {
        free(th[i].mem);
    }
    free(pt);
    free(th);
    return 0;
}
