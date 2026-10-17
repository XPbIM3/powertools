/*
 * cpuburn.c - CPU load generator for PowerPC Macs (Mac OS X 10.4/10.5)
 *
 * Standalone: knows nothing about any monitoring tools. It just burns.
 *
 * Design rules for the 970/G4 era:
 *   - the hot loop must keep ALL pipelines busy: 2 FPU (FMADD), 2
 *     integer ALU, vector (AltiVec) and the load/store unit. A pure
 *     dependency-chained FP loop stalls the machine into a low-power
 *     state and heats nothing.
 *   - 16 independent double accumulators >> FP latency (~6 cyc)
 *   - integer work is xor/shift based (int mul has 4-5 cyc latency)
 *   - a rolling 8 MB buffer keeps the memory controller awake
 *   - every kernel folds its state into a checksum, printed at the
 *     end: proof the work really happened and was not optimized away
 *
 * Usage: cpuburn [-n THREADS] [-t SECONDS] [-d FRACTION] [-m MODE] [-P] [-h]
 *   -n N   threads (default 1; >1 oversubscribes single-core machines)
 *   -t S   duration in seconds (default 10)
 *   -d F   duty cycle 0.01..1.00 (default 1.0)
 *   -m M   mode: fp | int | vec | mem | mixed (default mixed)
 *   -P     do not drop to nice +10 (default: nice +10 so the session
 *          stays responsive on single-core machines)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>
/* NOTE: Apple gcc 4.0 provides AltiVec builtins under -faltivec;
   /usr/lib/.../altivec.h must NOT be included (it self-disables there). */

#define QUANTUM_US   10000.0                       /* scheduling tick */
#define MEM_LEN      (8u << 20)                    /* 8 MB per thread */
#define MEM_STEP     (64u << 10)                   /* advance 64 KB */
#define MEM_WORDS    (MEM_LEN / 4)
#define MEM_TOUCH    (1u << 10)                    /* 4 KB per touch */

#define MODE_FP      0
#define MODE_INT     1
#define MODE_VEC     2
#define MODE_MEM     3
#define MODE_MIXED   4

static const char *mode_names[] = { "fp", "int", "vec", "mem", "mixed" };

typedef struct {
    int            id;
    int            mode;
    double         duty;
    double         deadline;

    double         busy_us;
    unsigned long long check;

    /* fp state */
    double         fa[16];
    double         fx;

    /* int state: 4 independent xorshift32 chains */
    unsigned int   ia[4];

    /* vector state */
#if defined(__ppc__)
    vector float   vv[4];
#endif
    float         *vbuf;                        /* 16-byte aligned */

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

/* ---------------------------------------------------------- altivec */

static void vec_block(burn_thread_t *t, int passes)
{
#if defined(__ppc__)
    vector float v0 = t->vv[0], v1 = t->vv[1];
    vector float v2 = t->vv[2], v3 = t->vv[3];
    vector float vx, vh, vstep, vt;
    float half[4] __attribute__((aligned(16))) = { 0.5f,    0.5f,    0.5f,    0.5f };
    float step[4] __attribute__((aligned(16))) = { 0.0625f, 0.0625f, 0.0625f, 0.0625f };
    float tmp[4] __attribute__((aligned(16)));
    unsigned int u0, u1, u2, u3;
    int p;

    vx    = vec_ld(0, (vector float *)t->vbuf);      /* exercise the LSU */
    vh    = vec_ld(0, (vector float *)half);
    vstep = vec_ld(0, (vector float *)step);

    for (p = 0; p < passes; p++) {
        vt = vec_add(vx, vstep);
        v0 = vec_madd(v0, vt, vh);
        v1 = vec_madd(v1, vt, vh);
        v2 = vec_madd(v2, vt, vh);
        v3 = vec_madd(v3, vt, vh);
        vx = vt;
    }
    t->vv[0] = v0; t->vv[1] = v1;
    t->vv[2] = v2; t->vv[3] = v3;

    vec_st(v0, 0, tmp);
    u0 = *(unsigned int *)&tmp[0] ^ *(unsigned int *)&tmp[1];
    vec_st(v1, 0, tmp);
    u1 = *(unsigned int *)&tmp[0] ^ *(unsigned int *)&tmp[1];
    vec_st(v2, 0, tmp);
    u2 = *(unsigned int *)&tmp[0] ^ *(unsigned int *)&tmp[1];
    vec_st(v3, 0, tmp);
    u3 = *(unsigned int *)&tmp[0] ^ *(unsigned int *)&tmp[1];

    t->check ^= ((unsigned long long)u0 << 16) ^ (u1 * 40503u)
              ^ ((unsigned long long)u2 << 8) ^ u3;
#else
    (void)t; (void)passes;
#endif
}

static double run_vec(burn_thread_t *t, double budget)
{
    double t0, t1;
    unsigned long long pass = 0;

    t0 = now_us();
    for (;;) {
        vec_block(t, 16);
        pass += 16;
        if ((pass & 31) == 0) {
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
    case MODE_VEC:  return run_vec(t, budget);
    case MODE_MEM:  return run_mem(t, budget);
    default: break;
    }

    /* mixed: alternate all kernels in small batches */
    t0 = now_us();
    for (;;) {
        fp_block(t, 4);
        int_block(t, 2);
        vec_block(t, 2);
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

    for (;;) {
        double now = now_us();
        double busy, got;

        if (now >= t->deadline)
            break;
        busy = t->duty * QUANTUM_US;
        if (t->deadline - now < busy)
            busy = t->deadline - now;
        got = run_mode(t, busy);
        t->busy_us += got;
        if (QUANTUM_US - busy > 100.0)
            usleep((useconds_t)(QUANTUM_US - busy));
    }
    return NULL;
}

/* ------------------------------------------------------------- main */

static void usage(void)
{
    int i;
    printf("cpuburn - CPU load generator for PowerPC (Mac OS X 10.4/10.5)\n\n"
           "usage: cpuburn [-n THREADS] [-t SECONDS] [-d FRACTION]"
           " [-m MODE] [-P] [-h]\n"
           "  -n N   threads (default 1; >1 oversubscribes single-core)\n"
           "  -t S   duration in seconds (default 10)\n"
           "  -d F   duty cycle 0.01..1.00 (default 1.0)\n"
           "  -m M   mode: ");
    for (i = 0; i < 5; i++)
        printf("%s%s", i ? "/" : "", mode_names[i]);
    printf(" (default mixed)\n"
           "  -P     keep normal priority (default: nice +10)\n"
           "  -h     this help\n");
}

int main(int argc, char **argv)
{
    int            nthreads = 1;
    int            mode = MODE_MIXED;
    int            no_nice = 0;
    double         duration = 10.0;
    double         duty = 1.0;
    double         start, end;
    pthread_t     *pt;
    burn_thread_t *th;
    int            i, ch, mi = 5;

    while ((ch = getopt(argc, argv, "n:t:d:m:Ph")) != -1) {
        switch (ch) {
        case 'n': nthreads = atoi(optarg); break;
        case 't': duration = atof(optarg); break;
        case 'd': duty = atof(optarg); break;
        case 'm':
            for (mi = 4; mi >= 0; mi--)
                if (strcmp(optarg, mode_names[mi]) == 0)
                    break;
            if (mi < 0) {
                fprintf(stderr, "cpuburn: unknown mode '%s'\n", optarg);
                return 1;
            }
            mode = mi;
            break;
        case 'P': no_nice = 1; break;
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
    if (duty < 0.01 || duty > 1.0) {
        fprintf(stderr, "cpuburn: -d must be 0.01..1.00\n");
        return 1;
    }

    if (!no_nice)
        nice(10);

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
        t->duty = duty;
        memset(t->fa, 0, sizeof t->fa);
        t->fx = 1.0;
        t->ia[0] = 0x9E3779B9u + (unsigned)i;
        t->ia[1] = 0x85EBCA77u - (unsigned)i;
        t->ia[2] = 0xC2B2AE35u + (unsigned)(i << 8);
        t->ia[3] = 0x27D4EB2Fu ^ (unsigned)(i << 16);
        t->vbuf = (float *)valloc(4096);
        t->mem  = (unsigned int *)malloc(MEM_LEN);
        if (!t->vbuf || !t->mem) {
            fprintf(stderr, "cpuburn: out of memory\n");
            return 1;
        }
        memset(t->vbuf, 0, 4096);
        memset(t->mem, 0, MEM_LEN);
        t->mpos = (unsigned)i * (MEM_STEP / 4);
#if defined(__ppc__)
        t->vv[0] = (vector float)(1.0f, 2.0f, 3.0f, 4.0f);
        t->vv[1] = (vector float)(1.1f, 2.2f, 3.3f, 4.4f);
        t->vv[2] = (vector float)(1.2f, 2.3f, 3.4f, 4.5f);
        t->vv[3] = (vector float)(1.3f, 2.4f, 3.5f, 4.6f);
#endif
    }

    printf("cpuburn: mode=%s threads=%d duty=%.0f%% duration=%.1f s\n",
           mode_names[mode], nthreads, duty * 100.0, duration);

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
        free(th[i].vbuf);
        free(th[i].mem);
    }
    free(pt);
    free(th);
    return 0;
}
