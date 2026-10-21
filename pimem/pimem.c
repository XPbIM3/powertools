/* pimem.c - merged from: Ooura pi_fftcs (kernel, verbatim) + driver + naive MEM phase. Generated 22.10.2026 */
/*
---- calculation of PI(= 3.14159...) using FFT and AGM ----
    by T.Ooura, Nov. 1999.

Example compilation:
    GNU      : gcc -O -funroll-loops -fomit-frame-pointer pi_fftcs.c fftsg_h.c -lm -o pi_css5
    SUN      : cc -fast pi_fftcs.c fftsg_h.c -lm -o pi_css5
    HP       : aCC -fast pi_fftcs.c fftsg_h.c -lm -o pi_css5
    Microsoft: cl -O2 pi_fftcs.c fftsg_h.c -o pi_css5
    ...
    etc.
*/

#define PI_FFTC_VER "ver. LG1.1.2-MP1.5.2a.memsave"

/* Please check the following macros before compiling */
#ifndef DBL_ERROR_MARGIN
#define DBL_ERROR_MARGIN 0.4	/* must be < 0.5 */
#endif

#define DGTINT short int	/* sizeof(DGTINT) == 2 */
#define DGTINT_MAX SHRT_MAX

#define DGT_PACK 10
#define DGT_PACK_LINE 5
#define DGT_LINE_BLOCK 20

#include <math.h>
#include <limits.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <ctype.h>

void mp_load_0 (int n, int radix, int out[]);
void mp_load_1 (int n, int radix, int out[]);
void mp_round (int n, int radix, int m, int inout[]);
int mp_cmp (int n, int radix, int in1[], int in2[]);
void mp_add (int n, int radix, int in1[], int in2[], int out[]);
void mp_sub (int n, int radix, int in1[], int in2[], int out[]);
void mp_imul (int n, int radix, int in1[], int in2, int out[]);
int mp_idiv (int n, int radix, int in1[], int in2, int out[]);
void mp_idiv_2 (int n, int radix, int in[], int out[]);
double mp_mul_radix_test (int n, int radix, int nfft, double tmpfft[]);
void mp_mul (int n, int radix, int in1[], int in2[], int out[],
	     int tmp[], int nfft, double tmp1fft[], double tmp2fft[],
	     double tmp3fft[]);
void mp_squ (int n, int radix, int in[], int out[], int tmp[],
	     int nfft, double tmp1fft[], double tmp2fft[]);
void mp_mulhf (int n, int radix, int in1[], int in2[], int out[],
	       int tmp[], int nfft, double in1fft[], double tmpfft[]);
void mp_mulhf_use_in1fft (int n, int radix, double in1fft[], int in2[],
			  int out[], int tmp[], int nfft, double tmpfft[]);
void mp_squhf_use_infft (int n, int radix, double infft[], int in[],
			 int out[], int tmp[], int nfft, double tmpfft[]);
void mp_mulh (int n, int radix, int in1[], int in2[], int out[],
	      int nfft, double in1fft[], double outfft[]);
void mp_squh (int n, int radix, int in[], int out[],
	      int nfft, double outfft[]);
int mp_inv (int n, int radix, int in[], int out[],
	    int tmp1[], int tmp2[], int nfft,
	    double tmp1fft[], double tmp2fft[]);
int mp_sqrt (int n, int radix, int in[], int out[],
	     int tmp1[], int tmp2[], int nfft,
	     double tmp1fft[], double tmp2fft[]);
int mp_invisqrt (int n, int radix, int in, int out[],
		 int tmp1[], int tmp2[], int nfft,
		 double tmp1fft[], double tmp2fft[]);
void mp_sprintf (int n, int log10_radix, int in[], char out[]);
void mp_sscanf (int n, int log10_radix, char in[], int out[]);

/*
 * pimem.c - one binary, two worlds: Ooura's pi (SuperPI-class) + MEM tier.
 *
 * pi phase : Ooura pi_fftcs + fftsg (AGM/FFT, merged verbatim, kernel
 *            untouched) run at -O2; tiers 128k/256k/512k/1M decimal
 *            digits, wall-clock seconds per tier + digit checksum.
 * mem phase: the MEM part of cpubench_naive carried over as-is
 *            (LCG fill, honest copy of volatile buffers, Sattolo
 *            single-cycle shuffle, one full dependent-load walk).
 *            Built under the same -O2 as the pi phase: buffers are
 *            volatile so the compiler cannot rewrite the copy loop
 *            into memcpy() - same C on every gcc and every arch.
 *
 * build (intel-leopard cross, both targets):
 *   gcc  -std=gnu99 -O2 -Wall -fno-tree-vectorize \
 *        -o pimem-intel pimem.c -lm
 *   gcc  -arch ppc -mcpu=G3 -Wl,-force_cpusubtype_ALL \
 *        -mmacosx-version-min=10.3 -std=gnu99 -O2 -Wall \
 *        -fno-tree-vectorize -o pimem-ppc pimem.c -lm
 *
 * usage: pimem [-t seconds] [-w MB] [-n] [-o FILE]
 *   default: pi tiers always run (about 2 s on Core2 / 150 s on G3-500),
 *            MEM copy is timed by -t (default 10 s), -w = total of BOTH
 *            buffers (default 128 MB; machines with 192 MB RAM: -w 64),
 *            -n pins copy to 32 passes (proof mode), -o FILE writes the
 *            pi digits of every tier into one file for cross-arch cmp.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/sysctl.h>

#define MEM_BYTES    (64UL << 20)
#define MEM_WORDS    (MEM_BYTES / sizeof (unsigned))
#define MEM_N_PASSES 32      /* -n mode: fixed copy passes */

static double now(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

static int usage(void)
{
    fprintf(stderr, "usage: pimem [-t seconds] [-w MB] [-n] [-o FILE]\n");
    return 1;
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

/* ---- pi tier wrapper: same kernel as Ooura's main, timed by wall clock.
 * digits: 131072..1048576 (all exact powers of two -> no rounding shifts);
 * f_out:  optional stream for the digits (NULL = silent);
 * returns elapsed seconds; writes seconds + checksum to stdout itself.  */
static double pi_tier(int digits, FILE *f_out);

/* ================== [Ooura part merged below] ================== */
/* ================== main: driver + MEM phase ================== */

int main(int argc, char **argv)
{
    double budget = 10.0, t0, mt = 0, mt2 = 0;
    unsigned long long wtotal_mb = 128;   /* -w: BOTH buffers, default 128 */
    unsigned long long n_fixed = 0;       /* -n: proof mode                */
    unsigned long wbytes = MEM_BYTES;     /* one buffer, bytes             */
    unsigned mw = MEM_WORDS;              /* one buffer, words             */
    unsigned *fb_src = NULL, *fb_dst = NULL;
    volatile unsigned *msrc = NULL, *mdst = NULL;
    FILE *fout = NULL;
    int i;

    static const int tiers[6] =
        { 131072, 262144, 524288, 1048576, 2097152, 4194304 };

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc && atof(argv[i + 1]) > 0)
            budget = atof(argv[++i]);
        else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            wtotal_mb = strtoull(argv[++i], NULL, 0);
            if (wtotal_mb < 2 || wtotal_mb > 1024)
                return usage();
        } else if (strcmp(argv[i], "-n") == 0) {
            n_fixed = 1;
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            fout = fopen(argv[++i], "w");
            if (!fout) {
                fprintf(stderr, "pimem: cannot write digits file\n");
                return 1;
            }
        } else {
            return usage();
        }
    }
    wbytes = (unsigned long)(wtotal_mb / 2) * 1024UL * 1024UL;
    mw = (unsigned)(wbytes / sizeof (unsigned));

    print_machine();
    if (n_fixed)
        printf("pimem: pi 128k/256k/512k/1M/2M/4M + MEM proof (%d copy passes, "
               "full walk, %lux2 MB)\n\n", MEM_N_PASSES,
               wbytes / (1024 * 1024));
    else
        printf("pimem: pi 128k/256k/512k/1M/2M/4M + MEM copy %.0fs / walk, "
               "%lux2 MB\n\n", budget, wbytes / (1024 * 1024));

    for (i = 0; i < 6; i++)
        pi_tier(tiers[i], fout);
    if (fout)
        fclose(fout);

    /* ---- MEM phase: RAM-tier copy + full permuted walk ----
     * volatile buffers: under -O2 the compiler must not rewrite the
     * copy loop into a builtin memcpy() - honest load/store on all arcs */
    printf("\n");
    fb_src = malloc(wbytes);
    fb_dst = malloc(wbytes);
    if (!fb_src || !fb_dst) {
        free(fb_src);
        free(fb_dst);
        printf("  MEM: skipped - 2 x %lu MB buffers not available\n",
               wbytes / (1024 * 1024));
        printf("  checksum: mem skipped (pi tiers above ran)\n");
        return 0;
    }
    msrc = (volatile unsigned *)fb_src;
    mdst = (volatile unsigned *)fb_dst;

    {
        unsigned seed = 0x9E3779B9u, jx, tmpx;
        unsigned long i2, h;
        unsigned long long mem_passes = 0;
        unsigned long long copychk = 0, walkchk = 0;

        /* src fill (deterministic LCG) + one untimed warm copy pass,
         * so page faults of the first touch stay out of the timing */
        for (i2 = 0; i2 < mw; i2++)
            msrc[i2] = seed = seed * 1664526u + 1013904223u;
        for (i2 = 0; i2 < mw; i2++)
            mdst[i2] = msrc[i2];

        t0 = now();
        do {                     /* naive copy, clock once per pass */
            for (i2 = 0; i2 < mw; i2++)
                mdst[i2] = msrc[i2];
            mem_passes++;
        } while (n_fixed ? mem_passes < MEM_N_PASSES
                         : !timed_out(t0, budget));
        mt = now() - t0;

        for (i2 = 0; i2 < mw; i2++)             /* proof of real copy */
            copychk ^= mdst[i2];

        /* latency: Sattolo shuffle (single cycle) over node index,
         * built in-place in msrc (untimed), then one full walk */
        for (i2 = 0; i2 < mw; i2++)
            msrc[i2] = (unsigned)i2;
        for (i2 = mw - 1; i2 > 0; i2--) {
            seed = seed * 1664526u + 1013904223u;
            jx = seed % (unsigned)i2;           /* Sattolo: j in [0, i-1] */
            tmpx = msrc[i2];
            msrc[i2] = msrc[jx];
            msrc[jx] = tmpx;
        }

        t0 = now();
        h = 0;
        i2 = 0;
        do {                     /* dependent load chain, full cycle */
            i2 = msrc[i2];
            if ((h & 4095u) == 4095u)
                walkchk ^= (unsigned long long)i2;
            h++;
        } while (h < mw);
        mt2 = now() - t0;

        if ((unsigned)i2 != 0)
            printf("  MEM: WARNING single-cycle walk left i2=%u - "
                   "permutation bug\n", (unsigned)i2);
        free(fb_src);
        free(fb_dst);

        printf("  MEM: %7.1f MB/s   copy %6.2f s %12llu passes (%luM "
               "window)\n", (double)mem_passes * (double)wbytes / mt / 1e6,
               mt, mem_passes, wbytes / (1024 * 1024));
        printf("  MEM: %7.1f ns/hop     %6.2f s %12lu hops   (full walk)\n",
               mt2 * 1e9 / (double)mw, mt2, (unsigned long)mw);
        printf("  checksum: mem %016llx\n", copychk ^ walkchk);
    }
    return 0;
}
static double pi_tier(int digits, FILE *f_out) {
  int nfft, log2_nfft, radix, log10_radix, n, j = 0, k = 0, l = 0, npow, nprc;
  double err;
  int *a, *b, *c, *e, *i1, *i2;
  double *d1, *d2, *d3;
  char *dgt;
  const char *q;
  unsigned dchk = 0;
  long ndg = 0;
  double t0, elap_time, loop_time;
  fprintf(stdout, "Calculation of PI using FFT and AGM, %s\n", PI_FFTC_VER);

  nfft = digits;
  t0 = now();
  nfft /= 4;
  for (log2_nfft = 1; (1 << log2_nfft) < nfft; log2_nfft++)
    ;
  nfft = 1 << log2_nfft;
  n = nfft + 2;
  a = (int *)malloc(2 * sizeof(int) + n * sizeof(DGTINT));
  b = (int *)malloc(2 * sizeof(int) + n * sizeof(DGTINT));
  c = (int *)malloc(2 * sizeof(int) + n * sizeof(DGTINT));
  e = (int *)malloc(2 * sizeof(int) + n * sizeof(DGTINT));
  i1 = (int *)malloc(2 * sizeof(int) + n * sizeof(DGTINT));
  i2 = (int *)malloc(2 * sizeof(int) + n * sizeof(DGTINT));
  d1 = (double *)malloc((nfft + 2) * sizeof(double));
  d2 = (double *)malloc((nfft + 2) * sizeof(double));
  d3 = (double *)malloc((nfft + 2) * sizeof(double));
  if (d3 == NULL) {
    printf("Allocation Failure!\n");
    exit(1);
  }
  /* ---- radix test ---- */
  log10_radix = 1;
  radix = 10;
  err = mp_mul_radix_test(n, radix, nfft, d1);
  err += DBL_EPSILON * (n * radix * radix / 4);
  while (100 * err < DBL_ERROR_MARGIN && radix <= DGTINT_MAX / 20) {
    err *= 100;
    log10_radix++;
    radix *= 10;
  }
#ifndef QUIET_OUT
  fprintf(stdout, "nfft= %d\nradix= %d\nerror_margin= %g\n", nfft, radix, err);
  fprintf(stdout, "calculating %d digits of PI...\n", log10_radix * (n - 2));
#endif
  /*
   * ---- a formula based on the AGM (Arithmetic-Geometric Mean) ----
   *   c = sqrt(0.125);
   *   a = 1 + 3 * c;
   *   b = sqrt(a);
   *   e = b - 0.625;
   *   b = 2 * b;
   *   c = e - c;
   *   a = a + e;
   *   npow = 4;
   *   do {
   *       npow = 2 * npow;
   *       e = (a + b) / 2;
   *       b = sqrt(a * b);
   *       e = e - b;
   *       b = 2 * b;
   *       c = c - e;
   *       a = e + b;
   *   } while (e > SQRT_SQRT_EPSILON);
   *   e = e * e / 4;
   *   a = a + b;
   *   pi = (a * a - e - e / 2) / (a * c - e) / npow;
   * ---- modification ----
   *   This is a modified version of Gauss-Legendre formula
   *   (by T.Ooura). It is faster than original version.
   * ---- reference ----
   *   1. E.Salamin,
   *      Computation of PI Using Arithmetic-Geometric Mean,
   *      Mathematics of Computation, Vol.30 1976.
   *   2. R.P.Brent,
   *      Fast Multiple-Precision Evaluation of Elementary Functions,
   *      J. ACM 23 1976.
   *   3. D.Takahasi, Y.Kanada,
   *      Calculation of PI to 51.5 Billion Decimal Digits on
   *      Distributed Memoriy Parallel Processors,
   *      Transactions of Information Processing Society of Japan,
   *      Vol.39 No.7 1998.
   *   4. T.Ooura,
   *      Improvement of the PI Calculation Algorithm and
   *      Implementation of Fast Multiple-Precision Computation,
   *      Information Processing Society of Japan SIG Notes,
   *      98-HPC-74, 1998.
   */
  /* ---- c = 1 / sqrt(8) ---- */
  mp_invisqrt(n, radix, 8, c, i1, i2, nfft, d1, d2);
  /* ---- a = 1 + 3 * c ---- */
  mp_imul(n, radix, c, 3, e);
  mp_sscanf(n, log10_radix, "1", a);
  mp_add(n, radix, a, e, a);
  /* ---- b = sqrt(a) ---- */
  mp_sqrt(n, radix, a, b, i1, i2, nfft, d1, d2);
  /* ---- e = b - 0.625 ---- */
  mp_sscanf(n, log10_radix, "0.625", e);
  mp_sub(n, radix, b, e, e);
  /* ---- b = 2 * b ---- */
  mp_add(n, radix, b, b, b);
  /* ---- c = e - c ---- */
  mp_sub(n, radix, e, c, c);
  /* ---- a = a + e ---- */
  mp_add(n, radix, a, e, a);
#ifndef QUIET_OUT
  fprintf(stdout, "AGM iteration\n");
#endif
  npow = 4;
  elap_time = now() - t0;

  do {
    double lt0 = now();
    npow *= 2;
    /* ---- e = (a + b) / 2 ---- */
    mp_add(n, radix, a, b, e);
    mp_idiv_2(n, radix, e, e);
    /* ---- b = sqrt(a * b) ---- */
    mp_mul(n, radix, a, b, a, i1, nfft, d1, d2, d3);
    mp_sqrt(n, radix, a, b, i1, i2, nfft, d1, d2);
    /* ---- e = e - b ---- */
    mp_sub(n, radix, e, b, e);
    /* ---- b = 2 * b ---- */
    mp_add(n, radix, b, b, b);
    /* ---- c = c - e ---- */
    mp_sub(n, radix, c, e, c);
    /* ---- a = e + b ---- */
    mp_add(n, radix, e, b, a);
    /* ---- convergence check ---- */
    nprc = -e[1];
    if (e[0] == 0) {
      nprc = n;
    }
    loop_time = now() - lt0;
    elap_time += loop_time;
#ifndef QUIET_OUT
    fprintf(stdout, "precision= %d: %0.2f sec\n", 4 * nprc * log10_radix,
            loop_time);
#endif
  } while (4 * nprc <= n);
  t0 = now();
  /* ---- e = e * e / 4 (half precision) ---- */
  mp_idiv_2(n, radix, e, e);
  mp_squh(n, radix, e, e, nfft, d1);
  /* ---- a = a + b ---- */
  mp_add(n, radix, a, b, a);
  /* ---- a = (a * a - e - e / 2) / (a * c - e) / npow ---- */
  mp_mulhf(n, radix, a, c, c, i1, nfft, d1, d2);
  mp_sub(n, radix, c, e, c);
  mp_inv(n, radix, c, b, i1, i2, nfft, d2, d3);
  mp_squhf_use_infft(n, radix, d1, a, a, i1, nfft, d2);
  mp_sub(n, radix, a, e, a);
  mp_idiv_2(n, radix, e, e);
  mp_sub(n, radix, a, e, a);
  mp_mul(n, radix, a, b, a, i1, nfft, d1, d2, d3);
  mp_idiv(n, radix, a, npow, a);
  /* ---- output ---- */
  dgt = (char *)d1;
  mp_sprintf(n - 1, log10_radix, a, dgt);
  elap_time += now() - t0;

  /* ---- digit checksum: rotate-xor over the digit string, same
   * stop condition as the writer below ('e' begins the exponent
   * tail) - the cross-arch proof line, no file needed ---- */
  for (q = dgt; *q && *q != 'e'; q++) {
    dchk = (dchk << 1 | (dchk >> 31)) ^ (unsigned char)*q;
    if (*q >= '0' && *q <= '9')
      ndg++;
  }
  printf("pi %d: chk %08x %ld digits\n", log10_radix * (n - 2),
         dchk, ndg);

  if (f_out) {
  do {
    if (!isdigit(*dgt)) {
      if (isalpha(*dgt) != 0) {
        fputc('\n', f_out);
        fputc('\n', f_out);
      }
      fputc(*dgt, f_out);
      fputc('\n', f_out);
      fputc('\n', f_out);
      j = 0;
      k = 0;
      l = 0;
      continue;
    }
    fputc(*dgt, f_out);
    if (++j >= DGT_PACK) {
      j = 0;
      if (++k >= DGT_PACK_LINE) {
        k = 0;
        fputc('\n', f_out);
        if (++l >= DGT_LINE_BLOCK) {
          l = 0;
          fputc('\n', f_out);
        }
      } else {
        fputc(' ', f_out);
      }
    }
  } while (*dgt++ && *dgt != 'e');
  fputc('\n', f_out);
  fprintf(f_out, "%s\n", dgt);
  fclose(f_out);
  }
  free(d3);
  free(d2);
  free(d1);
  free(i2);
  free(i1);
  free(e);
  free(c);
  free(b);
  free(a);
  /* ---- difftime ---- */
  fprintf(stdout, "pi %.0f total %0.2f sec.\n",
          (double)log10_radix * (n - 2), elap_time);

  return elap_time;
}

/* ---- multiple precision routines (Ooura, verbatim) ---- */

/* -------- multiple precision routines -------- */


#include <math.h>
#include <float.h>
#include <stdio.h>

/* ---- floating point format ----
    data := data[0] * pow(radix, data[1]) * 
            (data[2] + data[3]/radix + data[4]/radix/radix + ...), 
    data[0]       : sign (1;data>0, -1;data<0, 0;data==0)
    data[1]       : exponent (0;data==0)
    data[2...n+1] : digits
   ---- function prototypes ----
    void mp_load_0(int n, int radix, int out[]);
    void mp_load_1(int n, int radix, int out[]);
    void mp_round(int n, int radix, int m, int inout[]);
    int mp_cmp(int n, int radix, int in1[], int in2[]);
    void mp_add(int n, int radix, int in1[], int in2[], int out[]);
    void mp_sub(int n, int radix, int in1[], int in2[], int out[]);
    void mp_imul(int n, int radix, int in1[], int in2, int out[]);
    int mp_idiv(int n, int radix, int in1[], int in2, int out[]);
    void mp_idiv_2(int n, int radix, int in[], int out[]);
    double mp_mul_radix_test(int n, int radix, int nfft, 
            double tmpfft[]);
    void mp_mul(int n, int radix, int in1[], int in2[], int out[], 
            int tmp[], int nfft, double tmp1fft[], double tmp2fft[], 
            double tmp3fft[]);
    void mp_squ(int n, int radix, int in[], int out[], int tmp[], 
            int nfft, double tmp1fft[], double tmp2fft[]);
    void mp_mulhf(int n, int radix, int in1[], int in2[], int out[], 
            int tmp[], int nfft, double in1fft[], double tmpfft[]);
    void mp_mulhf_use_in1fft(int n, int radix, double in1fft[], int in2[], 
            int out[], int tmp[], int nfft, double tmpfft[]);
    void mp_squhf_use_infft(int n, int radix, double infft[], int in[], 
            int out[], int tmp[], int nfft, double tmpfft[]);
    void mp_mulh(int n, int radix, int in1[], int in2[], int out[], 
            int nfft, double in1fft[], double outfft[]);
    void mp_squh(int n, int radix, int in[], int out[], 
            int nfft, double outfft[]);
    int mp_inv(int n, int radix, int in[], int out[], 
            int tmp1[], int tmp2[], int nfft, 
            double tmp1fft[], double tmp2fft[]);
    int mp_sqrt(int n, int radix, int in[], int out[], 
            int tmp1[], int tmp2[], int nfft, 
            double tmp1fft[], double tmp2fft[]);
    int mp_invisqrt(int n, int radix, int in, int out[], 
            int tmp1[], int tmp2[], int nfft, 
            double tmp1fft[], double tmp2fft[]);
    void mp_sprintf(int n, int log10_radix, int in[], char out[]);
    void mp_sscanf(int n, int log10_radix, char in[], int out[]);
   ----
*/


/* -------- mp_load routines -------- */


void
mp_load_0 (int n, int radix, int out[])
{
  int j;
  DGTINT *outr;

  outr = ((DGTINT *) & out[2]) - 2;
  out[0] = 0;
  out[1] = 0;
  for (j = 2; j <= n + 1; j++)
    {
      outr[j] = 0;
    }
}


void
mp_load_1 (int n, int radix, int out[])
{
  int j;
  DGTINT *outr;

  outr = ((DGTINT *) & out[2]) - 2;
  out[0] = 1;
  out[1] = 0;
  outr[2] = 1;
  for (j = 3; j <= n + 1; j++)
    {
      outr[j] = 0;
    }
}


void
mp_round (int n, int radix, int m, int inout[])
{
  int j, x;
  DGTINT *inoutr;

  inoutr = ((DGTINT *) & inout[2]) - 2;
  if (m < n)
    {
      for (j = n + 1; j > m + 2; j--)
	{
	  inoutr[j] = 0;
	}
      x = 2 * inoutr[m + 2];
      inoutr[m + 2] = 0;
      if (x >= radix)
	{
	  for (j = m + 1; j >= 2; j--)
	    {
	      x = inoutr[j] + 1;
	      if (x < radix)
		{
		  inoutr[j] = (DGTINT) x;
		  break;
		}
	      inoutr[j] = 0;
	    }
	  if (x >= radix)
	    {
	      inoutr[2] = 1;
	      inout[1]++;
	    }
	}
    }
}


/* -------- mp_add routines -------- */


int
mp_cmp (int n, int radix, int in1[], int in2[])
{
  int mp_unsgn_cmp (int n, int in1[], int in2[]);

  if (in1[0] > in2[0])
    {
      return 1;
    }
  else if (in1[0] < in2[0])
    {
      return -1;
    }
  return in1[0] * mp_unsgn_cmp (n, &in1[1], &in2[1]);
}


void
mp_add (int n, int radix, int in1[], int in2[], int out[])
{
  int mp_unsgn_cmp (int n, int in1[], int in2[]);
  int mp_unexp_add (int n, int radix, int expdif,
		    DGTINT in1[], DGTINT in2[], DGTINT out[]);
  int mp_unexp_sub (int n, int radix, int expdif,
		    DGTINT in1[], DGTINT in2[], DGTINT out[]);
  int outsgn, outexp, expdif;

  expdif = in1[1] - in2[1];
  outexp = in1[1];
  if (expdif < 0)
    {
      outexp = in2[1];
    }
  outsgn = in1[0] * in2[0];
  if (outsgn >= 0)
    {
      if (outsgn > 0)
	{
	  outsgn = in1[0];
	}
      else
	{
	  outsgn = in1[0] + in2[0];
	  outexp = in1[1] + in2[1];
	  expdif = 0;
	}
      if (expdif >= 0)
	{
	  outexp += mp_unexp_add (n, radix, expdif,
				  (DGTINT *) & in1[2], (DGTINT *) & in2[2],
				  (DGTINT *) & out[2]);
	}
      else
	{
	  outexp += mp_unexp_add (n, radix, -expdif,
				  (DGTINT *) & in2[2], (DGTINT *) & in1[2],
				  (DGTINT *) & out[2]);
	}
    }
  else
    {
      outsgn = mp_unsgn_cmp (n, &in1[1], &in2[1]);
      if (outsgn >= 0)
	{
	  expdif = mp_unexp_sub (n, radix, expdif,
				 (DGTINT *) & in1[2], (DGTINT *) & in2[2],
				 (DGTINT *) & out[2]);
	}
      else
	{
	  expdif = mp_unexp_sub (n, radix, -expdif,
				 (DGTINT *) & in2[2], (DGTINT *) & in1[2],
				 (DGTINT *) & out[2]);
	}
      outexp -= expdif;
      outsgn *= in1[0];
      if (expdif == n)
	{
	  outsgn = 0;
	}
    }
  if (outsgn == 0)
    {
      outexp = 0;
    }
  out[0] = outsgn;
  out[1] = outexp;
}


void
mp_sub (int n, int radix, int in1[], int in2[], int out[])
{
  int mp_unsgn_cmp (int n, int in1[], int in2[]);
  int mp_unexp_add (int n, int radix, int expdif,
		    DGTINT in1[], DGTINT in2[], DGTINT out[]);
  int mp_unexp_sub (int n, int radix, int expdif,
		    DGTINT in1[], DGTINT in2[], DGTINT out[]);
  int outsgn, outexp, expdif;

  expdif = in1[1] - in2[1];
  outexp = in1[1];
  if (expdif < 0)
    {
      outexp = in2[1];
    }
  outsgn = in1[0] * in2[0];
  if (outsgn <= 0)
    {
      if (outsgn < 0)
	{
	  outsgn = in1[0];
	}
      else
	{
	  outsgn = in1[0] - in2[0];
	  outexp = in1[1] + in2[1];
	  expdif = 0;
	}
      if (expdif >= 0)
	{
	  outexp += mp_unexp_add (n, radix, expdif,
				  (DGTINT *) & in1[2], (DGTINT *) & in2[2],
				  (DGTINT *) & out[2]);
	}
      else
	{
	  outexp += mp_unexp_add (n, radix, -expdif,
				  (DGTINT *) & in2[2], (DGTINT *) & in1[2],
				  (DGTINT *) & out[2]);
	}
    }
  else
    {
      outsgn = mp_unsgn_cmp (n, &in1[1], &in2[1]);
      if (outsgn >= 0)
	{
	  expdif = mp_unexp_sub (n, radix, expdif,
				 (DGTINT *) & in1[2], (DGTINT *) & in2[2],
				 (DGTINT *) & out[2]);
	}
      else
	{
	  expdif = mp_unexp_sub (n, radix, -expdif,
				 (DGTINT *) & in2[2], (DGTINT *) & in1[2],
				 (DGTINT *) & out[2]);
	}
      outexp -= expdif;
      outsgn *= in1[0];
      if (expdif == n)
	{
	  outsgn = 0;
	}
    }
  if (outsgn == 0)
    {
      outexp = 0;
    }
  out[0] = outsgn;
  out[1] = outexp;
}


/* -------- mp_add child routines -------- */


int
mp_unsgn_cmp (int n, int in1[], int in2[])
{
  int j, cmp;
  DGTINT *in1r, *in2r;

  in1r = ((DGTINT *) & in1[1]) - 1;
  in2r = ((DGTINT *) & in2[1]) - 1;
  cmp = in1[0] - in2[0];
  for (j = 1; j <= n && cmp == 0; j++)
    {
      cmp = in1r[j] - in2r[j];
    }
  if (cmp > 0)
    {
      cmp = 1;
    }
  else if (cmp < 0)
    {
      cmp = -1;
    }
  return cmp;
}


int
mp_unexp_add (int n, int radix, int expdif,
	      DGTINT in1[], DGTINT in2[], DGTINT out[])
{
  int j, x, carry;

  carry = 0;
  if (expdif == 0 && in1[0] + in2[0] >= radix)
    {
      x = in1[n - 1] + in2[n - 1];
      carry = x >= radix ? -1 : 0;
      for (j = n - 1; j > 0; j--)
	{
	  x = in1[j - 1] + in2[j - 1] - carry;
	  carry = x >= radix ? -1 : 0;
	  out[j] = (DGTINT) (x - (radix & carry));
	}
      out[0] = (DGTINT) - carry;
    }
  else
    {
      if (expdif > n)
	{
	  expdif = n;
	}
      for (j = n - 1; j >= expdif; j--)
	{
	  x = in1[j] + in2[j - expdif] - carry;
	  carry = x >= radix ? -1 : 0;
	  out[j] = (DGTINT) (x - (radix & carry));
	}
      for (j = expdif - 1; j >= 0; j--)
	{
	  x = in1[j] - carry;
	  carry = x >= radix ? -1 : 0;
	  out[j] = (DGTINT) (x - (radix & carry));
	}
      if (carry != 0)
	{
	  for (j = n - 1; j > 0; j--)
	    {
	      out[j] = out[j - 1];
	    }
	  out[0] = (DGTINT) - carry;
	}
    }
  return -carry;
}


int
mp_unexp_sub (int n, int radix, int expdif,
	      DGTINT in1[], DGTINT in2[], DGTINT out[])
{
  int j, x, borrow, ncancel;

  if (expdif > n)
    {
      expdif = n;
    }
  borrow = 0;
  for (j = n - 1; j >= expdif; j--)
    {
      x = in1[j] - in2[j - expdif] + borrow;
      borrow = x < 0 ? -1 : 0;
      out[j] = (DGTINT) (x + (radix & borrow));
    }
  for (j = expdif - 1; j >= 0; j--)
    {
      x = in1[j] + borrow;
      borrow = x < 0 ? -1 : 0;
      out[j] = (DGTINT) (x + (radix & borrow));
    }
  ncancel = 0;
  for (j = 0; j < n && out[j] == 0; j++)
    {
      ncancel = j + 1;
    }
  if (ncancel > 0 && ncancel < n)
    {
      for (j = 0; j < n - ncancel; j++)
	{
	  out[j] = out[j + ncancel];
	}
      for (j = n - ncancel; j < n; j++)
	{
	  out[j] = 0;
	}
    }
  return ncancel;
}


/* -------- mp_imul routines -------- */


void
mp_imul (int n, int radix, int in1[], int in2, int out[])
{
  void mp_unsgn_imul (int n, double dradix, int in1[], double din2,
		      int out[]);

  if (in2 > 0)
    {
      out[0] = in1[0];
    }
  else if (in2 < 0)
    {
      out[0] = -in1[0];
      in2 = -in2;
    }
  else
    {
      out[0] = 0;
    }
  mp_unsgn_imul (n, radix, &in1[1], in2, &out[1]);
  if (out[0] == 0)
    {
      out[1] = 0;
    }
}


int
mp_idiv (int n, int radix, int in1[], int in2, int out[])
{
  void mp_load_0 (int n, int radix, int out[]);
  void mp_unsgn_idiv (int n, double dradix, int in1[], double din2,
		      int out[]);

  if (in2 == 0)
    {
      return -1;
    }
  if (in2 > 0)
    {
      out[0] = in1[0];
    }
  else
    {
      out[0] = -in1[0];
      in2 = -in2;
    }
  if (in1[0] == 0)
    {
      mp_load_0 (n, radix, out);
      return 0;
    }
  mp_unsgn_idiv (n, radix, &in1[1], in2, &out[1]);
  return 0;
}


void
mp_idiv_2 (int n, int radix, int in[], int out[])
{
  int j, ix, carry, shift;
  DGTINT *inr, *outr;

  inr = ((DGTINT *) & in[2]) - 2;
  outr = ((DGTINT *) & out[2]) - 2;
  out[0] = in[0];
  shift = 0;
  if (inr[2] == 1)
    {
      shift = 1;
    }
  out[1] = in[1] - shift;
  carry = -shift;
  for (j = 2; j <= n + 1 - shift; j++)
    {
      ix = inr[j + shift] + (radix & carry);
      carry = -(ix & 1);
      outr[j] = (DGTINT) (ix >> 1);
    }
  if (shift > 0)
    {
      outr[n + 1] = (DGTINT) ((radix & carry) >> 1);
    }
}


/* -------- mp_imul child routines -------- */


void
mp_unsgn_imul (int n, double dradix, int in1[], double din2, int out[])
{
  int j, carry, shift;
  double x, d1_radix;
  DGTINT *in1r, *outr;

  in1r = ((DGTINT *) & in1[1]) - 1;
  outr = ((DGTINT *) & out[1]) - 1;
  d1_radix = 1.0 / dradix;
  carry = 0;
  for (j = n; j >= 1; j--)
    {
      x = din2 * in1r[j] + carry + 0.5;
      carry = (int) (d1_radix * x);
      outr[j] = (DGTINT) (x - dradix * carry);
    }
  shift = 0;
  x = carry + 0.5;
  while (x > 1)
    {
      x *= d1_radix;
      shift++;
    }
  out[0] = in1[0] + shift;
  if (shift > 0)
    {
      while (shift > n)
	{
	  carry = (int) (d1_radix * carry + 0.5);
	  shift--;
	}
      for (j = n; j >= shift + 1; j--)
	{
	  outr[j] = outr[j - shift];
	}
      for (j = shift; j >= 1; j--)
	{
	  x = carry + 0.5;
	  carry = (int) (d1_radix * x);
	  outr[j] = (DGTINT) (x - dradix * carry);
	}
    }
}


void
mp_unsgn_idiv (int n, double dradix, int in1[], double din2, int out[])
{
  int j, ix, carry, shift;
  double x, d1_in2;
  DGTINT *in1r, *outr;

  in1r = ((DGTINT *) & in1[1]) - 1;
  outr = ((DGTINT *) & out[1]) - 1;
  d1_in2 = 1.0 / din2;
  shift = 0;
  x = 0;
  do
    {
      shift++;
      x *= dradix;
      if (shift <= n)
	{
	  x += in1r[shift];
	}
    }
  while (x < din2 - 0.5);
  x += 0.5;
  ix = (int) (d1_in2 * x);
  carry = (int) (x - din2 * ix);
  outr[1] = (DGTINT) ix;
  shift--;
  out[0] = in1[0] - shift;
  if (shift >= n)
    {
      shift = n - 1;
    }
  for (j = 2; j <= n - shift; j++)
    {
      x = in1r[j + shift] + dradix * carry + 0.5;
      ix = (int) (d1_in2 * x);
      carry = (int) (x - din2 * ix);
      outr[j] = (DGTINT) ix;
    }
  for (j = n - shift + 1; j <= n; j++)
    {
      x = dradix * carry + 0.5;
      ix = (int) (d1_in2 * x);
      carry = (int) (x - din2 * ix);
      outr[j] = (DGTINT) ix;
    }
}


/* -------- mp_mul routines -------- */


double
mp_mul_radix_test (int n, int radix, int nfft, double tmpfft[])
{
  void mp_mul_csqu (int nfft, double d1[]);
  double mp_mul_d2i_test (int radix, int nfft, double din[]);
  int j, ndata, radix_2;

  ndata = (nfft >> 1) + 1;
  if (ndata > n)
    {
      ndata = n;
    }
  tmpfft[nfft + 1] = radix - 1;
  for (j = nfft; j > ndata; j--)
    {
      tmpfft[j] = 0;
    }
  radix_2 = (radix + 1) / 2;
  for (j = ndata; j > 2; j--)
    {
      tmpfft[j] = radix_2;
    }
  tmpfft[2] = radix;
  tmpfft[1] = radix - 1;
  tmpfft[0] = 0;
  mp_mul_csqu (nfft, tmpfft);
  return 2 * mp_mul_d2i_test (radix, nfft, tmpfft);
}


void
mp_mul (int n, int radix, int in1[], int in2[], int out[],
	int tmp[], int nfft, double tmp1fft[], double tmp2fft[],
	double tmp3fft[])
{
  void mp_add (int n, int radix, int in1[], int in2[], int out[]);
  void mp_mul_i2d (int n, int radix, int nfft, int shift,
		   int in[], double dout[]);
  void mp_mul_cmul_nt_out (int nfft, double d1[], double d2[]);
  void mp_mul_cmul_nt_d2 (int nfft, double d1[], double d2[]);
  void mp_mul_cmul_nt_d1_add (int nfft, double d1[], double d2[],
			      double d3[]);
  void mp_mul_d2i (int n, int radix, int nfft, double din[], int out[]);
  int n_h, shift;
  DGTINT *in1r, *in2r;

  in1r = ((DGTINT *) & in1[2]) - 2;
  in2r = ((DGTINT *) & in2[2]) - 2;
  shift = (nfft >> 1) + 1;
  while (n > shift)
    {
      if (in1r[shift + 2] + in2r[shift + 2] != 0)
	{
	  break;
	}
      shift++;
    }
  n_h = n / 2 + 1;
  if (n_h < n - shift)
    {
      n_h = n - shift;
    }
  /* ---- tmp3fft = (upper) in1 * (lower) in2 ---- */
  mp_mul_i2d (n, radix, nfft, 0, in1, tmp1fft);
  mp_mul_i2d (n, radix, nfft, shift, in2, tmp3fft);
  mp_mul_cmul_nt_out (nfft, tmp1fft, tmp3fft);
  /* ---- tmp = (upper) in1 * (upper) in2 ---- */
  mp_mul_i2d (n, radix, nfft, 0, in2, tmp2fft);
  mp_mul_cmul_nt_d2 (nfft, tmp2fft, tmp1fft);
  mp_mul_d2i (n, radix, nfft, tmp1fft, tmp);
  /* ---- tmp3fft += (upper) in2 * (lower) in1 ---- */
  mp_mul_i2d (n, radix, nfft, shift, in1, tmp1fft);
  mp_mul_cmul_nt_d1_add (nfft, tmp2fft, tmp1fft, tmp3fft);
  /* ---- out = tmp + tmp3fft ---- */
  mp_mul_d2i (n_h, radix, nfft, tmp3fft, out);
  mp_add (n, radix, out, tmp, out);
}


void
mp_squ (int n, int radix, int in[], int out[], int tmp[],
	int nfft, double tmp1fft[], double tmp2fft[])
{
  void mp_add (int n, int radix, int in1[], int in2[], int out[]);
  void mp_mul_i2d (int n, int radix, int nfft, int shift,
		   int in[], double dout[]);
  void mp_mul_cmul (int nfft, double d1[], double d2[]);
  void mp_mul_csqu_nt_d1 (int nfft, double d1[]);
  void mp_mul_d2i (int n, int radix, int nfft, double din[], int out[]);
  int n_h, shift;
  DGTINT *inr;

  inr = ((DGTINT *) & in[2]) - 2;
  shift = (nfft >> 1) + 1;
  while (n > shift)
    {
      if (inr[shift + 2] != 0)
	{
	  break;
	}
      shift++;
    }
  n_h = n / 2 + 1;
  if (n_h < n - shift)
    {
      n_h = n - shift;
    }
  /* ---- tmp = 2 * (upper) in * (lower) in ---- */
  mp_mul_i2d (n, radix, nfft, 0, in, tmp1fft);
  mp_mul_i2d (n, radix, nfft, shift, in, tmp2fft);
  mp_mul_cmul (nfft, tmp1fft, tmp2fft);
  mp_mul_d2i (n_h, radix, nfft, tmp2fft, tmp);
  mp_add (n_h, radix, tmp, tmp, tmp);
  /* ---- out = tmp + ((upper) in)^2 ---- */
  mp_mul_csqu_nt_d1 (nfft, tmp1fft);
  mp_mul_d2i (n, radix, nfft, tmp1fft, out);
  mp_add (n, radix, out, tmp, out);
}


void
mp_mulhf (int n, int radix, int in1[], int in2[], int out[],
	  int tmp[], int nfft, double in1fft[], double tmpfft[])
{
  void mp_add (int n, int radix, int in1[], int in2[], int out[]);
  void mp_mul_i2d (int n, int radix, int nfft, int shift,
		   int in[], double dout[]);
  void mp_mul_cmul (int nfft, double d1[], double d2[]);
  void mp_mul_cmul_nt_d1 (int nfft, double d1[], double d2[]);
  void mp_mul_d2i (int n, int radix, int nfft, double din[], int out[]);
  int n_h, shift;
  DGTINT *in2r;

  in2r = ((DGTINT *) & in2[2]) - 2;
  shift = (nfft >> 1) + 1;
  while (n > shift)
    {
      if (in2r[shift + 2] != 0)
	{
	  break;
	}
      shift++;
    }
  n_h = n / 2 + 1;
  if (n_h < n - shift)
    {
      n_h = n - shift;
    }
  /* ---- tmp = (upper) in1 * (upper) in2 ---- */
  mp_mul_i2d (n, radix, nfft, 0, in1, in1fft);
  mp_mul_i2d (n, radix, nfft, 0, in2, tmpfft);
  mp_mul_cmul (nfft, in1fft, tmpfft);
  mp_mul_d2i (n, radix, nfft, tmpfft, tmp);
  /* ---- out = tmp + (upper) in1 * (lower) in2 ---- */
  mp_mul_i2d (n, radix, nfft, shift, in2, tmpfft);
  mp_mul_cmul_nt_d1 (nfft, in1fft, tmpfft);
  mp_mul_d2i (n_h, radix, nfft, tmpfft, out);
  mp_add (n, radix, out, tmp, out);
}


void
mp_mulhf_use_in1fft (int n, int radix, double in1fft[], int in2[],
		     int out[], int tmp[], int nfft, double tmpfft[])
{
  void mp_add (int n, int radix, int in1[], int in2[], int out[]);
  void mp_mul_i2d (int n, int radix, int nfft, int shift,
		   int in[], double dout[]);
  void mp_mul_cmul_nt_d1 (int nfft, double d1[], double d2[]);
  void mp_mul_d2i (int n, int radix, int nfft, double din[], int out[]);
  int n_h, shift;
  DGTINT *in2r;

  in2r = ((DGTINT *) & in2[2]) - 2;
  shift = (nfft >> 1) + 1;
  while (n > shift)
    {
      if (in2r[shift + 2] != 0)
	{
	  break;
	}
      shift++;
    }
  n_h = n / 2 + 1;
  if (n_h < n - shift)
    {
      n_h = n - shift;
    }
  /* ---- tmp = (upper) in1fft * (upper) in2 ---- */
  mp_mul_i2d (n, radix, nfft, 0, in2, tmpfft);
  mp_mul_cmul_nt_d1 (nfft, in1fft, tmpfft);
  mp_mul_d2i (n, radix, nfft, tmpfft, tmp);
  /* ---- out = tmp + (upper) in1 * (lower) in2 ---- */
  mp_mul_i2d (n, radix, nfft, shift, in2, tmpfft);
  mp_mul_cmul_nt_d1 (nfft, in1fft, tmpfft);
  mp_mul_d2i (n_h, radix, nfft, tmpfft, out);
  mp_add (n, radix, out, tmp, out);
}


void
mp_squhf_use_infft (int n, int radix, double infft[], int in[],
		    int out[], int tmp[], int nfft, double tmpfft[])
{
  void mp_add (int n, int radix, int in1[], int in2[], int out[]);
  void mp_mul_i2d (int n, int radix, int nfft, int shift,
		   int in[], double dout[]);
  void mp_mul_cmul_nt_d1 (int nfft, double d1[], double d2[]);
  void mp_mul_csqu_nt_d1 (int nfft, double d1[]);
  void mp_mul_d2i (int n, int radix, int nfft, double din[], int out[]);
  int n_h, shift;
  DGTINT *inr;

  inr = ((DGTINT *) & in[2]) - 2;
  shift = (nfft >> 1) + 1;
  while (n > shift)
    {
      if (inr[shift + 2] != 0)
	{
	  break;
	}
      shift++;
    }
  n_h = n / 2 + 1;
  if (n_h < n - shift)
    {
      n_h = n - shift;
    }
  /* ---- tmp = (upper) infft * (lower) in ---- */
  mp_mul_i2d (n, radix, nfft, shift, in, tmpfft);
  mp_mul_cmul_nt_d1 (nfft, infft, tmpfft);
  mp_mul_d2i (n_h, radix, nfft, tmpfft, tmp);
  /* ---- out = tmp + ((upper) infft)^2 ---- */
  mp_mul_csqu_nt_d1 (nfft, infft);
  mp_mul_d2i (n, radix, nfft, infft, out);
  mp_add (n, radix, out, tmp, out);
}


void
mp_mulh (int n, int radix, int in1[], int in2[], int out[],
	 int nfft, double in1fft[], double outfft[])
{
  void mp_mul_i2d (int n, int radix, int nfft, int shift,
		   int in[], double dout[]);
  void mp_mul_cmul (int nfft, double d1[], double d2[]);
  void mp_mul_d2i (int n, int radix, int nfft, double din[], int out[]);

  mp_mul_i2d (n, radix, nfft, 0, in1, in1fft);
  mp_mul_i2d (n, radix, nfft, 0, in2, outfft);
  mp_mul_cmul (nfft, in1fft, outfft);
  mp_mul_d2i (n, radix, nfft, outfft, out);
}


void
mp_mulh_use_in1fft (int n, int radix, double in1fft[],
		    int shift, int in2[], int out[], int nfft,
		    double outfft[])
{
  void mp_mul_i2d (int n, int radix, int nfft, int shift,
		   int in[], double dout[]);
  void mp_mul_cmul_nt_d1 (int nfft, double d1[], double d2[]);
  void mp_mul_d2i (int n, int radix, int nfft, double din[], int out[]);
  DGTINT *in2r;

  in2r = ((DGTINT *) & in2[2]) - 2;
  while (n > shift)
    {
      if (in2r[shift + 2] != 0)
	{
	  break;
	}
      shift++;
    }
  mp_mul_i2d (n, radix, nfft, shift, in2, outfft);
  mp_mul_cmul_nt_d1 (nfft, in1fft, outfft);
  mp_mul_d2i (n, radix, nfft, outfft, out);
}


void
mp_squh (int n, int radix, int in[], int out[], int nfft, double outfft[])
{
  void mp_mul_i2d (int n, int radix, int nfft, int shift,
		   int in[], double dout[]);
  void mp_mul_csqu (int nfft, double d1[]);
  void mp_mul_d2i (int n, int radix, int nfft, double din[], int out[]);

  mp_mul_i2d (n, radix, nfft, 0, in, outfft);
  mp_mul_csqu (nfft, outfft);
  mp_mul_d2i (n, radix, nfft, outfft, out);
}


void
mp_squh_save_infft (int n, int radix, int in[], int out[],
		    int nfft, double infft[], double outfft[])
{
  void mp_mul_i2d (int n, int radix, int nfft, int shift,
		   int in[], double dout[]);
  void mp_mul_csqu_save_d1 (int nfft, double d1[], double d2[]);
  void mp_mul_d2i (int n, int radix, int nfft, double din[], int out[]);

  mp_mul_i2d (n, radix, nfft, 0, in, infft);
  mp_mul_csqu_save_d1 (nfft, infft, outfft);
  mp_mul_d2i (n, radix, nfft, outfft, out);
}


void
mp_squh_use_in1fft (int n, int radix, double inoutfft[], int out[], int nfft)
{
  void mp_mul_csqu_nt_d1 (int nfft, double d1[]);
  void mp_mul_d2i (int n, int radix, int nfft, double din[], int out[]);

  mp_mul_csqu_nt_d1 (nfft, inoutfft);
  mp_mul_d2i (n, radix, nfft, inoutfft, out);
}


/* -------- mp_mul child routines -------- */


void
mp_mul_i2d (int n, int radix, int nfft, int shift, int in[], double dout[])
{
  int j, x, carry, ndata, radix_2, topdgt;
  DGTINT *inr;

  inr = ((DGTINT *) & in[2]) - 2;
  ndata = 0;
  topdgt = 0;
  if (n > shift)
    {
      topdgt = inr[shift + 2];
      ndata = (nfft >> 1) + 1;
      if (ndata > n - shift)
	{
	  ndata = n - shift;
	}
    }
  dout[nfft + 1] = in[0] * topdgt;
  for (j = nfft; j > ndata; j--)
    {
      dout[j] = 0;
    }
  /* ---- abs(dout[j]) <= radix/2 (to keep FFT precision) ---- */
  if (ndata > 1)
    {
      radix_2 = radix / 2;
      carry = 0;
      for (j = ndata + 1; j > 3; j--)
	{
	  x = inr[j + shift] - carry;
	  carry = x >= radix_2 ? -1 : 0;
	  dout[j - 1] = x - (radix & carry);
	}
      dout[2] = inr[shift + 3] - carry;
    }
  dout[1] = topdgt;
  dout[0] = in[1] - shift;
}


void
mp_mul_cmul (int nfft, double d1[], double d2[])
{
  void cdft (int n, int isgn, double *a);
  void mp_mul_rcmul (int n, double *a, double *b);
  double xr, xi;

  cdft (nfft, 1, &d1[1]);
  cdft (nfft, 1, &d2[1]);
  d2[0] += d1[0];
  xr = d1[1] * d2[1] + d1[2] * d2[2];
  xi = d1[1] * d2[2] + d1[2] * d2[1];
  d2[1] = xr;
  d2[2] = xi;
  if (nfft > 2)
    {
      mp_mul_rcmul (nfft, &d1[1], &d2[1]);
    }
  d2[nfft + 1] *= d1[nfft + 1];
  cdft (nfft, -1, &d2[1]);
}


void
mp_mul_cmul_nt_d1 (int nfft, double d1[], double d2[])
{
  void cdft (int n, int isgn, double *a);
  void mp_mul_rcmul_nt_in1 (int n, double *a, double *b);
  double xr, xi;

  cdft (nfft, 1, &d2[1]);
  d2[0] += d1[0];
  xr = d1[1] * d2[1] + d1[2] * d2[2];
  xi = d1[1] * d2[2] + d1[2] * d2[1];
  d2[1] = xr;
  d2[2] = xi;
  if (nfft > 2)
    {
      mp_mul_rcmul_nt_in1 (nfft, &d1[1], &d2[1]);
    }
  d2[nfft + 1] *= d1[nfft + 1];
  cdft (nfft, -1, &d2[1]);
}


void
mp_mul_cmul_nt_d2 (int nfft, double d1[], double d2[])
{
  void cdft (int n, int isgn, double *a);
  void mp_mul_rcmul_nt_in2 (int n, double *a, double *b);
  double xr, xi;

  cdft (nfft, 1, &d1[1]);
  d2[0] += d1[0];
  xr = d1[1] * d2[1] + d1[2] * d2[2];
  xi = d1[1] * d2[2] + d1[2] * d2[1];
  d2[1] = xr;
  d2[2] = xi;
  if (nfft > 2)
    {
      mp_mul_rcmul_nt_in2 (nfft, &d1[1], &d2[1]);
    }
  d2[nfft + 1] *= d1[nfft + 1];
  cdft (nfft, -1, &d2[1]);
}


void
mp_mul_cmul_nt_out (int nfft, double d1[], double d2[])
{
  void cdft (int n, int isgn, double *a);
  void mp_mul_rcmul_nt_out (int n, double *a, double *b);
  double xr, xi;

  cdft (nfft, 1, &d1[1]);
  cdft (nfft, 1, &d2[1]);
  d2[0] += d1[0];
  xr = d1[1] * d2[1] + d1[2] * d2[2];
  xi = d1[1] * d2[2] + d1[2] * d2[1];
  d2[1] = xr;
  d2[2] = xi;
  if (nfft > 2)
    {
      mp_mul_rcmul_nt_out (nfft, &d1[1], &d2[1]);
    }
  d2[nfft + 1] *= d1[nfft + 1];
}


void
mp_mul_cmul_nt_d1_add (int nfft, double d1[], double d2[], double d3[])
{
  void cdft (int n, int isgn, double *a);
  void mp_mul_rcmul_nt_in1_add (int n, double *a, double *b, double *badd);
  double xr, xi;

  cdft (nfft, 1, &d2[1]);
  xr = d1[1] * d2[1] + d1[2] * d2[2];
  xi = d1[1] * d2[2] + d1[2] * d2[1];
  d3[1] += xr;
  d3[2] += xi;
  if (nfft > 2)
    {
      mp_mul_rcmul_nt_in1_add (nfft, &d1[1], &d2[1], &d3[1]);
    }
  d3[nfft + 1] += d1[nfft + 1] * d2[nfft + 1];
  cdft (nfft, -1, &d3[1]);
}


void
mp_mul_csqu (int nfft, double d1[])
{
  void cdft (int n, int isgn, double *a);
  void mp_mul_rcsqu (int n, double *a);
  double xr, xi;

  cdft (nfft, 1, &d1[1]);
  d1[0] *= 2;
  xr = d1[1] * d1[1] + d1[2] * d1[2];
  xi = 2 * d1[1] * d1[2];
  d1[1] = xr;
  d1[2] = xi;
  if (nfft > 2)
    {
      mp_mul_rcsqu (nfft, &d1[1]);
    }
  d1[nfft + 1] *= d1[nfft + 1];
  cdft (nfft, -1, &d1[1]);
}


void
mp_mul_csqu_save_d1 (int nfft, double d1[], double d2[])
{
  void cdft (int n, int isgn, double *a);
  void mp_mul_rcsqu_save (int n, double *a, double *b);
  double xr, xi;

  cdft (nfft, 1, &d1[1]);
  d2[0] = 2 * d1[0];
  xr = d1[1] * d1[1] + d1[2] * d1[2];
  xi = 2 * d1[1] * d1[2];
  d2[1] = xr;
  d2[2] = xi;
  if (nfft > 2)
    {
      mp_mul_rcsqu_save (nfft, &d1[1], &d2[1]);
    }
  d2[nfft + 1] = d1[nfft + 1] * d1[nfft + 1];
  cdft (nfft, -1, &d2[1]);
}


void
mp_mul_csqu_nt_d1 (int nfft, double d1[])
{
  void cdft (int n, int isgn, double *a);
  void mp_mul_rcsqu_nt_in (int n, double *a);
  double xr, xi;

  d1[0] *= 2;
  xr = d1[1] * d1[1] + d1[2] * d1[2];
  xi = 2 * d1[1] * d1[2];
  d1[1] = xr;
  d1[2] = xi;
  if (nfft > 2)
    {
      mp_mul_rcsqu_nt_in (nfft, &d1[1]);
    }
  d1[nfft + 1] *= d1[nfft + 1];
  cdft (nfft, -1, &d1[1]);
}


void
mp_mul_d2i (int n, int radix, int nfft, double din[], int out[])
{
  int j, carry, carry1, carry2, shift, ndata;
  double x, scale, d1_radix, d1_radix2, pow_radix, topdgt;
  DGTINT *outr;

  outr = ((DGTINT *) & out[2]) - 2;
  scale = 2.0 / nfft;
  d1_radix = 1.0 / radix;
  d1_radix2 = d1_radix * d1_radix;
  topdgt = din[nfft + 1];
  x = topdgt < 0 ? -topdgt : topdgt;
  shift = x + 0.5 >= radix ? 1 : 0;
  /* ---- correction of cyclic convolution of din[1] ---- */
  x *= nfft * 0.5;
  din[nfft + 1] = din[1] - x;
  din[1] = x;
  /* ---- output of digits ---- */
  ndata = n;
  if (n > nfft + 1 + shift)
    {
      ndata = nfft + 1 + shift;
      for (j = n + 1; j > ndata + 1; j--)
	{
	  outr[j] = 0;
	}
    }
  x = 0;
  pow_radix = 1;
  for (j = ndata + 1 - shift; j <= nfft + 1; j++)
    {
      x += pow_radix * din[j];
      pow_radix *= d1_radix;
      if (pow_radix < DBL_EPSILON)
	{
	  break;
	}
    }
  x = d1_radix2 * (scale * x + 0.5);
  carry2 = ((int) x) - 1;
  carry = (int) (radix * (x - carry2) + 0.5);
  for (j = ndata; j > 1; j--)
    {
      x = d1_radix2 * (scale * din[j - shift] + carry + 0.5);
      carry = carry2;
      carry2 = ((int) x) - 1;
      x = radix * (x - carry2);
      carry1 = (int) x;
      outr[j + 1] = (DGTINT) (radix * (x - carry1));
      carry += carry1;
    }
  x = carry + ((double) radix) * carry2 + 0.5;
  if (shift == 0)
    {
      x += scale * din[1];
    }
  carry = (int) (d1_radix * x);
  outr[2] = (DGTINT) (x - ((double) radix) * carry);
  if (carry > 0)
    {
      for (j = n + 1; j > 2; j--)
	{
	  outr[j] = outr[j - 1];
	}
      outr[2] = (DGTINT) carry;
      shift++;
    }
  /* ---- output of exp, sgn ---- */
  x = din[0] + shift + 0.5;
  shift = ((int) x) - 1;
  out[1] = shift + ((int) (x - shift));
  out[0] = topdgt > 0.5 ? 1 : -1;
  if (outr[2] == 0)
    {
      out[0] = 0;
      out[1] = 0;
    }
}


double
mp_mul_d2i_test (int radix, int nfft, double din[])
{
  int j, carry, carry1, carry2;
  double x, scale, d1_radix, d1_radix2, err;

  scale = 2.0 / nfft;
  d1_radix = 1.0 / radix;
  d1_radix2 = d1_radix * d1_radix;
  /* ---- correction of cyclic convolution of din[1] ---- */
  x = din[nfft + 1] * nfft * 0.5;
  if (x < 0)
    {
      x = -x;
    }
  din[nfft + 1] = din[1] - x;
  /* ---- check of digits ---- */
  err = 0;
  carry = 0;
  carry2 = 0;
  for (j = nfft + 1; j > 1; j--)
    {
      x = d1_radix2 * (scale * din[j] + carry + 0.5);
      carry = carry2;
      carry2 = ((int) x) - 1;
      x = radix * (x - carry2);
      carry1 = (int) x;
      x = radix * (x - carry1);
      carry += carry1;
      x = x - 0.5 - ((int) x);
      if (x > err)
	{
	  err = x;
	}
      else if (-x > err)
	{
	  err = -x;
	}
    }
  return err;
}


/* -------- mp_mul child^2 routines (mix RFFT routines) -------- */


#ifndef M_PI_2
#define M_PI_2      1.570796326794896619231321691639751442098584699687
#endif


#ifndef RDFT_LOOP_DIV		/* control of the RDFT's speed & tolerance */
#define RDFT_LOOP_DIV 64
#endif


void
mp_mul_rcmul (int n, double *a, double *b)
{
  int i, i0, j, k;
  double ec, w1r, w1i, wkr, wki, wdr, wdi, ss;
  double xr, xi, yr, yi, ajr, aji, akr, aki, bjr, bji, bkr, bki;

  ec = 2 * M_PI_2 / n;
  wkr = 0;
  wki = 0;
  wdi = cos (ec);
  wdr = sin (ec);
  wdi *= wdr;
  wdr *= wdr;
  w1r = 1 - 2 * wdr;
  w1i = 2 * wdi;
  ss = 2 * w1i;
  i = n >> 1;
  xr = a[i];
  xi = a[i + 1];
  yr = b[i];
  yi = b[i + 1];
  b[i] = xr * yr - xi * yi;
  b[i + 1] = xr * yi + xi * yr;
  for (;;)
    {
      i0 = i - 4 * RDFT_LOOP_DIV;
      if (i0 < 2)
	{
	  i0 = 2;
	}
      for (j = i - 2; j >= i0; j -= 2)
	{
	  k = n - j;
	  xr = wkr + ss * wdi;
	  xi = wki + ss * (0.5 - wdr);
	  wkr = wdr;
	  wki = wdi;
	  wdr = xr;
	  wdi = xi;
	  /* ---- transform CFFT data a[] into RFFT data ---- */
	  xr = a[j] - a[k];
	  xi = a[j + 1] + a[k + 1];
	  yr = wkr * xr - wki * xi;
	  yi = wkr * xi + wki * xr;
	  ajr = a[j] - yr;
	  aji = a[j + 1] - yi;
	  akr = a[k] + yr;
	  aki = a[k + 1] - yi;
	  a[j] = ajr;
	  a[j + 1] = aji;
	  a[k] = akr;
	  a[k + 1] = aki;
	  /* ---- transform CFFT data b[] into RFFT data ---- */
	  xr = b[j] - b[k];
	  xi = b[j + 1] + b[k + 1];
	  yr = wkr * xr - wki * xi;
	  yi = wkr * xi + wki * xr;
	  xr = b[j] - yr;
	  xi = b[j + 1] - yi;
	  yr = b[k] + yr;
	  yi = b[k + 1] - yi;
	  /* ---- cmul ---- */
	  bjr = ajr * xr - aji * xi;
	  bji = ajr * xi + aji * xr;
	  bkr = akr * yr - aki * yi;
	  bki = akr * yi + aki * yr;
	  /* ---- transform RFFT data bxx into CFFT data ---- */
	  xr = bjr - bkr;
	  xi = bji + bki;
	  yr = wkr * xr + wki * xi;
	  yi = wkr * xi - wki * xr;
	  b[j] = bjr - yr;
	  b[j + 1] = bji - yi;
	  b[k] = bkr + yr;
	  b[k + 1] = bki - yi;
	}
      if (i0 == 2)
	{
	  break;
	}
      wkr = 0.5 * sin (ec * i0);
      wki = 0.5 * cos (ec * i0);
      wdr = 0.5 - (wkr * w1r - wki * w1i);
      wdi = wkr * w1i + wki * w1r;
      wkr = 0.5 - wkr;
      i = i0;
    }
}


void
mp_mul_rcmul_nt_in1 (int n, double *a, double *b)
{
  int i, i0, j, k;
  double ec, w1r, w1i, wkr, wki, wdr, wdi, ss;
  double xr, xi, yr, yi, bjr, bji, bkr, bki;

  ec = 2 * M_PI_2 / n;
  wkr = 0;
  wki = 0;
  wdi = cos (ec);
  wdr = sin (ec);
  wdi *= wdr;
  wdr *= wdr;
  w1r = 1 - 2 * wdr;
  w1i = 2 * wdi;
  ss = 2 * w1i;
  i = n >> 1;
  xr = a[i];
  xi = a[i + 1];
  yr = b[i];
  yi = b[i + 1];
  b[i] = xr * yr - xi * yi;
  b[i + 1] = xr * yi + xi * yr;
  for (;;)
    {
      i0 = i - 4 * RDFT_LOOP_DIV;
      if (i0 < 2)
	{
	  i0 = 2;
	}
      for (j = i - 2; j >= i0; j -= 2)
	{
	  k = n - j;
	  xr = wkr + ss * wdi;
	  xi = wki + ss * (0.5 - wdr);
	  wkr = wdr;
	  wki = wdi;
	  wdr = xr;
	  wdi = xi;
	  /* ---- transform CFFT data b[] into RFFT data ---- */
	  xr = b[j] - b[k];
	  xi = b[j + 1] + b[k + 1];
	  yr = wkr * xr - wki * xi;
	  yi = wkr * xi + wki * xr;
	  xr = b[j] - yr;
	  xi = b[j + 1] - yi;
	  yr = b[k] + yr;
	  yi = b[k + 1] - yi;
	  /* ---- cmul ---- */
	  bjr = a[j] * xr - a[j + 1] * xi;
	  bji = a[j] * xi + a[j + 1] * xr;
	  bkr = a[k] * yr - a[k + 1] * yi;
	  bki = a[k] * yi + a[k + 1] * yr;
	  /* ---- transform RFFT data bxx into CFFT data ---- */
	  xr = bjr - bkr;
	  xi = bji + bki;
	  yr = wkr * xr + wki * xi;
	  yi = wkr * xi - wki * xr;
	  b[j] = bjr - yr;
	  b[j + 1] = bji - yi;
	  b[k] = bkr + yr;
	  b[k + 1] = bki - yi;
	}
      if (i0 == 2)
	{
	  break;
	}
      wkr = 0.5 * sin (ec * i0);
      wki = 0.5 * cos (ec * i0);
      wdr = 0.5 - (wkr * w1r - wki * w1i);
      wdi = wkr * w1i + wki * w1r;
      wkr = 0.5 - wkr;
      i = i0;
    }
}


void
mp_mul_rcmul_nt_in2 (int n, double *a, double *b)
{
  int i, i0, j, k;
  double ec, w1r, w1i, wkr, wki, wdr, wdi, ss;
  double xr, xi, yr, yi, bjr, bji, bkr, bki;

  ec = 2 * M_PI_2 / n;
  wkr = 0;
  wki = 0;
  wdi = cos (ec);
  wdr = sin (ec);
  wdi *= wdr;
  wdr *= wdr;
  w1r = 1 - 2 * wdr;
  w1i = 2 * wdi;
  ss = 2 * w1i;
  i = n >> 1;
  xr = a[i];
  xi = a[i + 1];
  yr = b[i];
  yi = b[i + 1];
  b[i] = xr * yr - xi * yi;
  b[i + 1] = xr * yi + xi * yr;
  for (;;)
    {
      i0 = i - 4 * RDFT_LOOP_DIV;
      if (i0 < 2)
	{
	  i0 = 2;
	}
      for (j = i - 2; j >= i0; j -= 2)
	{
	  k = n - j;
	  xr = wkr + ss * wdi;
	  xi = wki + ss * (0.5 - wdr);
	  wkr = wdr;
	  wki = wdi;
	  wdr = xr;
	  wdi = xi;
	  /* ---- transform CFFT data a[] into RFFT data ---- */
	  xr = a[j] - a[k];
	  xi = a[j + 1] + a[k + 1];
	  yr = wkr * xr - wki * xi;
	  yi = wkr * xi + wki * xr;
	  xr = a[j] - yr;
	  xi = a[j + 1] - yi;
	  yr = a[k] + yr;
	  yi = a[k + 1] - yi;
	  a[j] = xr;
	  a[j + 1] = xi;
	  a[k] = yr;
	  a[k + 1] = yi;
	  /* ---- cmul ---- */
	  bjr = b[j] * xr - b[j + 1] * xi;
	  bji = b[j] * xi + b[j + 1] * xr;
	  bkr = b[k] * yr - b[k + 1] * yi;
	  bki = b[k] * yi + b[k + 1] * yr;
	  /* ---- transform RFFT data bxx into CFFT data ---- */
	  xr = bjr - bkr;
	  xi = bji + bki;
	  yr = wkr * xr + wki * xi;
	  yi = wkr * xi - wki * xr;
	  b[j] = bjr - yr;
	  b[j + 1] = bji - yi;
	  b[k] = bkr + yr;
	  b[k + 1] = bki - yi;
	}
      if (i0 == 2)
	{
	  break;
	}
      wkr = 0.5 * sin (ec * i0);
      wki = 0.5 * cos (ec * i0);
      wdr = 0.5 - (wkr * w1r - wki * w1i);
      wdi = wkr * w1i + wki * w1r;
      wkr = 0.5 - wkr;
      i = i0;
    }
}


void
mp_mul_rcmul_nt_out (int n, double *a, double *b)
{
  int i, i0, j, k;
  double ec, w1r, w1i, wkr, wki, wdr, wdi, ss;
  double xr, xi, yr, yi, ajr, aji, akr, aki;

  ec = 2 * M_PI_2 / n;
  wkr = 0;
  wki = 0;
  wdi = cos (ec);
  wdr = sin (ec);
  wdi *= wdr;
  wdr *= wdr;
  w1r = 1 - 2 * wdr;
  w1i = 2 * wdi;
  ss = 2 * w1i;
  i = n >> 1;
  xr = a[i];
  xi = a[i + 1];
  yr = b[i];
  yi = b[i + 1];
  b[i] = xr * yr - xi * yi;
  b[i + 1] = xr * yi + xi * yr;
  for (;;)
    {
      i0 = i - 4 * RDFT_LOOP_DIV;
      if (i0 < 2)
	{
	  i0 = 2;
	}
      for (j = i - 2; j >= i0; j -= 2)
	{
	  k = n - j;
	  xr = wkr + ss * wdi;
	  xi = wki + ss * (0.5 - wdr);
	  wkr = wdr;
	  wki = wdi;
	  wdr = xr;
	  wdi = xi;
	  /* ---- transform CFFT data a[] into RFFT data ---- */
	  xr = a[j] - a[k];
	  xi = a[j + 1] + a[k + 1];
	  yr = wkr * xr - wki * xi;
	  yi = wkr * xi + wki * xr;
	  ajr = a[j] - yr;
	  aji = a[j + 1] - yi;
	  akr = a[k] + yr;
	  aki = a[k + 1] - yi;
	  a[j] = ajr;
	  a[j + 1] = aji;
	  a[k] = akr;
	  a[k + 1] = aki;
	  /* ---- transform CFFT data b[] into RFFT data ---- */
	  xr = b[j] - b[k];
	  xi = b[j + 1] + b[k + 1];
	  yr = wkr * xr - wki * xi;
	  yi = wkr * xi + wki * xr;
	  xr = b[j] - yr;
	  xi = b[j + 1] - yi;
	  yr = b[k] + yr;
	  yi = b[k + 1] - yi;
	  /* ---- cmul ---- */
	  b[j] = ajr * xr - aji * xi;
	  b[j + 1] = ajr * xi + aji * xr;
	  b[k] = akr * yr - aki * yi;
	  b[k + 1] = akr * yi + aki * yr;
	}
      if (i0 == 2)
	{
	  break;
	}
      wkr = 0.5 * sin (ec * i0);
      wki = 0.5 * cos (ec * i0);
      wdr = 0.5 - (wkr * w1r - wki * w1i);
      wdi = wkr * w1i + wki * w1r;
      wkr = 0.5 - wkr;
      i = i0;
    }
}


void
mp_mul_rcmul_nt_in1_add (int n, double *a, double *b, double *badd)
{
  int i, i0, j, k;
  double ec, w1r, w1i, wkr, wki, wdr, wdi, ss;
  double xr, xi, yr, yi, bjr, bji, bkr, bki;

  ec = 2 * M_PI_2 / n;
  wkr = 0;
  wki = 0;
  wdi = cos (ec);
  wdr = sin (ec);
  wdi *= wdr;
  wdr *= wdr;
  w1r = 1 - 2 * wdr;
  w1i = 2 * wdi;
  ss = 2 * w1i;
  i = n >> 1;
  xr = a[i];
  xi = a[i + 1];
  yr = b[i];
  yi = b[i + 1];
  badd[i] += xr * yr - xi * yi;
  badd[i + 1] += xr * yi + xi * yr;
  for (;;)
    {
      i0 = i - 4 * RDFT_LOOP_DIV;
      if (i0 < 2)
	{
	  i0 = 2;
	}
      for (j = i - 2; j >= i0; j -= 2)
	{
	  k = n - j;
	  xr = wkr + ss * wdi;
	  xi = wki + ss * (0.5 - wdr);
	  wkr = wdr;
	  wki = wdi;
	  wdr = xr;
	  wdi = xi;
	  /* ---- transform CFFT data b[] into RFFT data ---- */
	  xr = b[j] - b[k];
	  xi = b[j + 1] + b[k + 1];
	  yr = wkr * xr - wki * xi;
	  yi = wkr * xi + wki * xr;
	  xr = b[j] - yr;
	  xi = b[j + 1] - yi;
	  yr = b[k] + yr;
	  yi = b[k + 1] - yi;
	  /* ---- cmul + add ---- */
	  bjr = badd[j] + (a[j] * xr - a[j + 1] * xi);
	  bji = badd[j + 1] + (a[j] * xi + a[j + 1] * xr);
	  bkr = badd[k] + (a[k] * yr - a[k + 1] * yi);
	  bki = badd[k + 1] + (a[k] * yi + a[k + 1] * yr);
	  /* ---- transform RFFT data bxx into CFFT data ---- */
	  xr = bjr - bkr;
	  xi = bji + bki;
	  yr = wkr * xr + wki * xi;
	  yi = wkr * xi - wki * xr;
	  badd[j] = bjr - yr;
	  badd[j + 1] = bji - yi;
	  badd[k] = bkr + yr;
	  badd[k + 1] = bki - yi;
	}
      if (i0 == 2)
	{
	  break;
	}
      wkr = 0.5 * sin (ec * i0);
      wki = 0.5 * cos (ec * i0);
      wdr = 0.5 - (wkr * w1r - wki * w1i);
      wdi = wkr * w1i + wki * w1r;
      wkr = 0.5 - wkr;
      i = i0;
    }
}


void
mp_mul_rcsqu (int n, double *a)
{
  int i, i0, j, k;
  double ec, w1r, w1i, wkr, wki, wdr, wdi, ss;
  double xr, xi, yr, yi, ajr, aji, akr, aki;

  ec = 2 * M_PI_2 / n;
  wkr = 0;
  wki = 0;
  wdi = cos (ec);
  wdr = sin (ec);
  wdi *= wdr;
  wdr *= wdr;
  w1r = 1 - 2 * wdr;
  w1i = 2 * wdi;
  ss = 2 * w1i;
  i = n >> 1;
  xr = a[i];
  xi = a[i + 1];
  a[i] = xr * xr - xi * xi;
  a[i + 1] = 2 * xr * xi;
  for (;;)
    {
      i0 = i - 4 * RDFT_LOOP_DIV;
      if (i0 < 2)
	{
	  i0 = 2;
	}
      for (j = i - 2; j >= i0; j -= 2)
	{
	  k = n - j;
	  xr = wkr + ss * wdi;
	  xi = wki + ss * (0.5 - wdr);
	  wkr = wdr;
	  wki = wdi;
	  wdr = xr;
	  wdi = xi;
	  /* ---- transform CFFT data a[] into RFFT data ---- */
	  xr = a[j] - a[k];
	  xi = a[j + 1] + a[k + 1];
	  yr = wkr * xr - wki * xi;
	  yi = wkr * xi + wki * xr;
	  xr = a[j] - yr;
	  xi = a[j + 1] - yi;
	  yr = a[k] + yr;
	  yi = a[k + 1] - yi;
	  /* ---- csqu ---- */
	  ajr = xr * xr - xi * xi;
	  aji = 2 * xr * xi;
	  akr = yr * yr - yi * yi;
	  aki = 2 * yr * yi;
	  /* ---- transform RFFT data axx into CFFT data ---- */
	  xr = ajr - akr;
	  xi = aji + aki;
	  yr = wkr * xr + wki * xi;
	  yi = wkr * xi - wki * xr;
	  a[j] = ajr - yr;
	  a[j + 1] = aji - yi;
	  a[k] = akr + yr;
	  a[k + 1] = aki - yi;
	}
      if (i0 == 2)
	{
	  break;
	}
      wkr = 0.5 * sin (ec * i0);
      wki = 0.5 * cos (ec * i0);
      wdr = 0.5 - (wkr * w1r - wki * w1i);
      wdi = wkr * w1i + wki * w1r;
      wkr = 0.5 - wkr;
      i = i0;
    }
}


void
mp_mul_rcsqu_save (int n, double *a, double *b)
{
  int i, i0, j, k;
  double ec, w1r, w1i, wkr, wki, wdr, wdi, ss;
  double xr, xi, yr, yi, ajr, aji, akr, aki;

  ec = 2 * M_PI_2 / n;
  wkr = 0;
  wki = 0;
  wdi = cos (ec);
  wdr = sin (ec);
  wdi *= wdr;
  wdr *= wdr;
  w1r = 1 - 2 * wdr;
  w1i = 2 * wdi;
  ss = 2 * w1i;
  i = n >> 1;
  xr = a[i];
  xi = a[i + 1];
  b[i] = xr * xr - xi * xi;
  b[i + 1] = 2 * xr * xi;
  for (;;)
    {
      i0 = i - 4 * RDFT_LOOP_DIV;
      if (i0 < 2)
	{
	  i0 = 2;
	}
      for (j = i - 2; j >= i0; j -= 2)
	{
	  k = n - j;
	  xr = wkr + ss * wdi;
	  xi = wki + ss * (0.5 - wdr);
	  wkr = wdr;
	  wki = wdi;
	  wdr = xr;
	  wdi = xi;
	  /* ---- transform CFFT data a[] into RFFT data ---- */
	  xr = a[j] - a[k];
	  xi = a[j + 1] + a[k + 1];
	  yr = wkr * xr - wki * xi;
	  yi = wkr * xi + wki * xr;
	  xr = a[j] - yr;
	  xi = a[j + 1] - yi;
	  yr = a[k] + yr;
	  yi = a[k + 1] - yi;
	  a[j] = xr;
	  a[j + 1] = xi;
	  a[k] = yr;
	  a[k + 1] = yi;
	  /* ---- csqu ---- */
	  ajr = xr * xr - xi * xi;
	  aji = 2 * xr * xi;
	  akr = yr * yr - yi * yi;
	  aki = 2 * yr * yi;
	  /* ---- transform RFFT data axx into CFFT data ---- */
	  xr = ajr - akr;
	  xi = aji + aki;
	  yr = wkr * xr + wki * xi;
	  yi = wkr * xi - wki * xr;
	  b[j] = ajr - yr;
	  b[j + 1] = aji - yi;
	  b[k] = akr + yr;
	  b[k + 1] = aki - yi;
	}
      if (i0 == 2)
	{
	  break;
	}
      wkr = 0.5 * sin (ec * i0);
      wki = 0.5 * cos (ec * i0);
      wdr = 0.5 - (wkr * w1r - wki * w1i);
      wdi = wkr * w1i + wki * w1r;
      wkr = 0.5 - wkr;
      i = i0;
    }
}


void
mp_mul_rcsqu_nt_in (int n, double *a)
{
  int i, i0, j, k;
  double ec, w1r, w1i, wkr, wki, wdr, wdi, ss;
  double xr, xi, yr, yi, ajr, aji, akr, aki;

  ec = 2 * M_PI_2 / n;
  wkr = 0;
  wki = 0;
  wdi = cos (ec);
  wdr = sin (ec);
  wdi *= wdr;
  wdr *= wdr;
  w1r = 1 - 2 * wdr;
  w1i = 2 * wdi;
  ss = 2 * w1i;
  i = n >> 1;
  xr = a[i];
  xi = a[i + 1];
  a[i] = xr * xr - xi * xi;
  a[i + 1] = 2 * xr * xi;
  for (;;)
    {
      i0 = i - 4 * RDFT_LOOP_DIV;
      if (i0 < 2)
	{
	  i0 = 2;
	}
      for (j = i - 2; j >= i0; j -= 2)
	{
	  k = n - j;
	  xr = wkr + ss * wdi;
	  xi = wki + ss * (0.5 - wdr);
	  wkr = wdr;
	  wki = wdi;
	  wdr = xr;
	  wdi = xi;
	  /* ---- csqu ---- */
	  xr = a[j];
	  xi = a[j + 1];
	  yr = a[k];
	  yi = a[k + 1];
	  ajr = xr * xr - xi * xi;
	  aji = 2 * xr * xi;
	  akr = yr * yr - yi * yi;
	  aki = 2 * yr * yi;
	  /* ---- transform RFFT data axx into CFFT data ---- */
	  xr = ajr - akr;
	  xi = aji + aki;
	  yr = wkr * xr + wki * xi;
	  yi = wkr * xi - wki * xr;
	  a[j] = ajr - yr;
	  a[j + 1] = aji - yi;
	  a[k] = akr + yr;
	  a[k + 1] = aki - yi;
	}
      if (i0 == 2)
	{
	  break;
	}
      wkr = 0.5 * sin (ec * i0);
      wki = 0.5 * cos (ec * i0);
      wdr = 0.5 - (wkr * w1r - wki * w1i);
      wdi = wkr * w1i + wki * w1r;
      wkr = 0.5 - wkr;
      i = i0;
    }
}


/* -------- mp_inv routines -------- */


int
mp_inv (int n, int radix, int in[], int out[],
	int tmp1[], int tmp2[], int nfft, double tmp1fft[], double tmp2fft[])
{
  int mp_get_nfft_init (int radix, int nfft_max);
  void mp_inv_init (int n, int radix, int in[], int out[]);
  int mp_inv_newton (int n, int radix, int in[], int inout[],
		     int tmp1[], int tmp2[], int nfft, double tmp1fft[],
		     double tmp2fft[]);
  int n_nwt, nfft_nwt, thr, prc;

  if (in[0] == 0)
    {
      return -1;
    }
  nfft_nwt = mp_get_nfft_init (radix, nfft);
  n_nwt = nfft_nwt + 2;
  if (n_nwt > n)
    {
      n_nwt = n;
    }
  mp_inv_init (n_nwt, radix, in, out);
  thr = 8;
  do
    {
      n_nwt = nfft_nwt + 2;
      if (n_nwt > n)
	{
	  n_nwt = n;
	}
      prc = mp_inv_newton (n_nwt, radix, in, out,
			   tmp1, tmp2, nfft_nwt, tmp1fft, tmp2fft);
#ifdef DEBUG
      printf ("n=%d, nfft=%d, prc=%d\n", n_nwt, nfft_nwt, prc);
#endif
      if (thr * nfft_nwt >= nfft)
	{
	  thr = 0;
	  if (2 * prc <= n_nwt - 2)
	    {
	      nfft_nwt >>= 1;
	    }
	}
      else
	{
	  if (3 * prc < n_nwt - 2)
	    {
	      nfft_nwt >>= 1;
	    }
	}
      nfft_nwt <<= 1;
    }
  while (nfft_nwt <= nfft);
  return 0;
}


int
mp_sqrt (int n, int radix, int in[], int out[],
	 int tmp1[], int tmp2[], int nfft, double tmp1fft[], double tmp2fft[])
{
  void mp_load_0 (int n, int radix, int out[]);
  int mp_get_nfft_init (int radix, int nfft_max);
  void mp_sqrt_init (int n, int radix, int in[], int out[], int out_rev[]);
  int mp_sqrt_newton (int n, int radix, int in[], int inout[],
		      int inout_rev[], int tmp[], int nfft, double tmp1fft[],
		      double tmp2fft[], int *n_tmp1fft);
  int n_nwt, nfft_nwt, thr, prc, n_tmp1fft;

  if (in[0] < 0)
    {
      return -1;
    }
  else if (in[0] == 0)
    {
      mp_load_0 (n, radix, out);
      return 0;
    }
  nfft_nwt = mp_get_nfft_init (radix, nfft);
  n_nwt = nfft_nwt + 2;
  if (n_nwt > n)
    {
      n_nwt = n;
    }
  mp_sqrt_init (n_nwt, radix, in, out, tmp1);
  n_tmp1fft = 0;
  thr = 8;
  do
    {
      n_nwt = nfft_nwt + 2;
      if (n_nwt > n)
	{
	  n_nwt = n;
	}
      prc = mp_sqrt_newton (n_nwt, radix, in, out,
			    tmp1, tmp2, nfft_nwt, tmp1fft, tmp2fft,
			    &n_tmp1fft);
#ifdef DEBUG
      printf ("n=%d, nfft=%d, prc=%d\n", n_nwt, nfft_nwt, prc);
#endif
      if (thr * nfft_nwt >= nfft)
	{
	  thr = 0;
	  if (2 * prc <= n_nwt - 2)
	    {
	      nfft_nwt >>= 1;
	    }
	}
      else
	{
	  if (3 * prc < n_nwt - 2)
	    {
	      nfft_nwt >>= 1;
	    }
	}
      nfft_nwt <<= 1;
    }
  while (nfft_nwt <= nfft);
  return 0;
}


int
mp_invisqrt (int n, int radix, int in, int out[],
	     int tmp1[], int tmp2[], int nfft,
	     double tmp1fft[], double tmp2fft[])
{
  int mp_get_nfft_init (int radix, int nfft_max);
  void mp_invisqrt_init (int n, int radix, int in, int out[]);
  int mp_invisqrt_newton (int n, int radix, int in, int inout[],
			  int tmp1[], int tmp2[], int nfft, double tmp1fft[],
			  double tmp2fft[]);
  int n_nwt, nfft_nwt, thr, prc;

  if (in <= 0)
    {
      return -1;
    }
  nfft_nwt = mp_get_nfft_init (radix, nfft);
  n_nwt = nfft_nwt + 2;
  if (n_nwt > n)
    {
      n_nwt = n;
    }
  mp_invisqrt_init (n_nwt, radix, in, out);
  thr = 8;
  do
    {
      n_nwt = nfft_nwt + 2;
      if (n_nwt > n)
	{
	  n_nwt = n;
	}
      prc = mp_invisqrt_newton (n_nwt, radix, in, out,
				tmp1, tmp2, nfft_nwt, tmp1fft, tmp2fft);
#ifdef DEBUG
      printf ("n=%d, nfft=%d, prc=%d\n", n_nwt, nfft_nwt, prc);
#endif
      if (thr * nfft_nwt >= nfft)
	{
	  thr = 0;
	  if (2 * prc <= n_nwt - 2)
	    {
	      nfft_nwt >>= 1;
	    }
	}
      else
	{
	  if (3 * prc < n_nwt - 2)
	    {
	      nfft_nwt >>= 1;
	    }
	}
      nfft_nwt <<= 1;
    }
  while (nfft_nwt <= nfft);
  return 0;
}


/* -------- mp_inv child routines -------- */


int
mp_get_nfft_init (int radix, int nfft_max)
{
  int nfft_init;
  double r;

  r = radix;
  nfft_init = 1;
  do
    {
      r *= r;
      nfft_init <<= 1;
    }
  while (DBL_EPSILON * r < 1 && nfft_init < nfft_max);
  return nfft_init;
}


void
mp_inv_init (int n, int radix, int in[], int out[])
{
  void mp_unexp_d2mp (int n, int radix, double din, DGTINT out[]);
  double mp_unexp_mp2d (int n, int radix, DGTINT in[]);
  int outexp;
  double din;

  out[0] = in[0];
  outexp = -in[1];
  din = 1.0 / mp_unexp_mp2d (n, radix, (DGTINT *) & in[2]);
  while (din < 1)
    {
      din *= radix;
      outexp--;
    }
  out[1] = outexp;
  mp_unexp_d2mp (n, radix, din, (DGTINT *) & out[2]);
}


void
mp_sqrt_init (int n, int radix, int in[], int out[], int out_rev[])
{
  void mp_unexp_d2mp (int n, int radix, double din, DGTINT out[]);
  double mp_unexp_mp2d (int n, int radix, DGTINT in[]);
  int outexp;
  double din;

  out[0] = 1;
  out_rev[0] = 1;
  outexp = in[1];
  din = mp_unexp_mp2d (n, radix, (DGTINT *) & in[2]);
  if (outexp % 2 != 0)
    {
      din *= radix;
      outexp--;
    }
  outexp /= 2;
  din = sqrt (din);
  if (din < 1)
    {
      din *= radix;
      outexp--;
    }
  out[1] = outexp;
  mp_unexp_d2mp (n, radix, din, (DGTINT *) & out[2]);
  outexp = -outexp;
  din = 1.0 / din;
  while (din < 1)
    {
      din *= radix;
      outexp--;
    }
  out_rev[1] = outexp;
  mp_unexp_d2mp (n, radix, din, (DGTINT *) & out_rev[2]);
}


void
mp_invisqrt_init (int n, int radix, int in, int out[])
{
  void mp_unexp_d2mp (int n, int radix, double din, DGTINT out[]);
  int outexp;
  double dout;

  out[0] = 1;
  outexp = 0;
  dout = sqrt (1.0 / in);
  while (dout < 1)
    {
      dout *= radix;
      outexp--;
    }
  out[1] = outexp;
  mp_unexp_d2mp (n, radix, dout, (DGTINT *) & out[2]);
}


void
mp_unexp_d2mp (int n, int radix, double din, DGTINT out[])
{
  int j, x;

  for (j = 0; j < n; j++)
    {
      x = (int) din;
      if (x >= radix)
	{
	  x = radix - 1;
	  din = radix;
	}
      din = radix * (din - x);
      out[j] = (DGTINT) x;
    }
}


double
mp_unexp_mp2d (int n, int radix, DGTINT in[])
{
  int j;
  double d1_radix, dout;

  d1_radix = 1.0 / radix;
  dout = 0;
  for (j = n - 1; j >= 0; j--)
    {
      dout = d1_radix * dout + in[j];
    }
  return dout;
}


int
mp_inv_newton (int n, int radix, int in[], int inout[],
	       int tmp1[], int tmp2[], int nfft, double tmp1fft[],
	       double tmp2fft[])
{
  void mp_load_1 (int n, int radix, int out[]);
  void mp_round (int n, int radix, int m, int inout[]);
  void mp_add (int n, int radix, int in1[], int in2[], int out[]);
  void mp_sub (int n, int radix, int in1[], int in2[], int out[]);
  void mp_mulh (int n, int radix, int in1[], int in2[], int out[],
		int nfft, double in1fft[], double outfft[]);
  void mp_mulh_use_in1fft (int n, int radix, double in1fft[],
			   int shift, int in2[], int out[], int nfft,
			   double outfft[]);
  int n_h, shift, prc;

  shift = (nfft >> 1) + 1;
  n_h = n / 2 + 1;
  if (n_h < n - shift)
    {
      n_h = n - shift;
    }
  /* ---- tmp1 = inout * (upper) in (half to normal precision) ---- */
  mp_round (n, radix, shift, inout);
  mp_mulh (n, radix, inout, in, tmp1, nfft, tmp1fft, tmp2fft);
  /* ---- tmp2 = 1 - tmp1 ---- */
  mp_load_1 (n, radix, tmp2);
  mp_sub (n, radix, tmp2, tmp1, tmp2);
  /* ---- tmp2 -= inout * (lower) in (half precision) ---- */
  mp_mulh_use_in1fft (n, radix, tmp1fft, shift, in, tmp1, nfft, tmp2fft);
  mp_sub (n_h, radix, tmp2, tmp1, tmp2);
  /* ---- get precision ---- */
  prc = -tmp2[1];
  if (tmp2[0] == 0)
    {
      prc = nfft + 1;
    }
  /* ---- tmp2 *= inout (half precision) ---- */
  mp_mulh_use_in1fft (n_h, radix, tmp1fft, 0, tmp2, tmp2, nfft, tmp2fft);
  /* ---- inout += tmp2 ---- */
  mp_add (n, radix, inout, tmp2, inout);
  return prc;
}


int
mp_sqrt_newton (int n, int radix, int in[], int inout[],
		int inout_rev[], int tmp[], int nfft, double tmp1fft[],
		double tmp2fft[], int *n_tmp1fft)
{
  void mp_round (int n, int radix, int m, int inout[]);
  void mp_add (int n, int radix, int in1[], int in2[], int out[]);
  void mp_sub (int n, int radix, int in1[], int in2[], int out[]);
  void mp_idiv_2 (int n, int radix, int in[], int out[]);
  void mp_mulh (int n, int radix, int in1[], int in2[], int out[],
		int nfft, double in1fft[], double outfft[]);
  void mp_squh (int n, int radix, int in[], int out[],
		int nfft, double outfft[]);
  void mp_squh_use_in1fft (int n, int radix, double inoutfft[], int out[],
			   int nfft);
  int n_h, nfft_h, shift, prc;

  nfft_h = nfft >> 1;
  shift = nfft_h + 1;
  if (nfft_h < 2)
    {
      nfft_h = 2;
    }
  n_h = n / 2 + 1;
  if (n_h < n - shift)
    {
      n_h = n - shift;
    }
  /* ---- tmp = inout_rev^2 (1/4 to half precision) ---- */
  mp_round (n_h, radix, (nfft_h >> 1) + 1, inout_rev);
  if (*n_tmp1fft != nfft_h)
    {
      mp_squh (n_h, radix, inout_rev, tmp, nfft_h, tmp1fft);
    }
  else
    {
      mp_squh_use_in1fft (n_h, radix, tmp1fft, tmp, nfft_h);
    }
  /* ---- tmp = inout_rev - inout * tmp (half precision) ---- */
  mp_round (n, radix, shift, inout);
  mp_mulh (n_h, radix, inout, tmp, tmp, nfft, tmp1fft, tmp2fft);
  mp_sub (n_h, radix, inout_rev, tmp, tmp);
  /* ---- inout_rev += tmp ---- */
  mp_add (n_h, radix, inout_rev, tmp, inout_rev);
  /* ---- tmp = in - inout^2 (half to normal precision) ---- */
  mp_squh_use_in1fft (n, radix, tmp1fft, tmp, nfft);
  mp_sub (n, radix, in, tmp, tmp);
  /* ---- get precision ---- */
  prc = in[1] - tmp[1];
  if (((DGTINT *) & in[2])[0] > ((DGTINT *) & tmp[2])[0])
    {
      prc++;
    }
  if (tmp[0] == 0)
    {
      prc = nfft + 1;
    }
  /* ---- tmp = tmp * inout_rev / 2 (half precision) ---- */
  mp_round (n_h, radix, shift, inout_rev);
  mp_mulh (n_h, radix, inout_rev, tmp, tmp, nfft, tmp1fft, tmp2fft);
  *n_tmp1fft = nfft;
  mp_idiv_2 (n_h, radix, tmp, tmp);
  /* ---- inout += tmp ---- */
  mp_add (n, radix, inout, tmp, inout);
  return prc;
}


int
mp_invisqrt_newton (int n, int radix, int in, int inout[],
		    int tmp1[], int tmp2[], int nfft, double tmp1fft[],
		    double tmp2fft[])
{
  void mp_load_1 (int n, int radix, int out[]);
  void mp_round (int n, int radix, int m, int inout[]);
  void mp_add (int n, int radix, int in1[], int in2[], int out[]);
  void mp_sub (int n, int radix, int in1[], int in2[], int out[]);
  void mp_imul (int n, int radix, int in1[], int in2, int out[]);
  void mp_idiv_2 (int n, int radix, int in[], int out[]);
  void mp_squh_save_infft (int n, int radix, int in[], int out[],
			   int nfft, double infft[], double outfft[]);
  void mp_mulh_use_in1fft (int n, int radix, double in1fft[],
			   int shift, int in2[], int out[], int nfft,
			   double outfft[]);
  int n_h, shift, prc;

  shift = (nfft >> 1) + 1;
  n_h = n / 2 + 1;
  if (n_h < n - shift)
    {
      n_h = n - shift;
    }
  /* ---- tmp1 = in * inout^2 (half to normal precision) ---- */
  mp_round (n, radix, shift, inout);
  mp_squh_save_infft (n, radix, inout, tmp1, nfft, tmp1fft, tmp2fft);
  mp_imul (n, radix, tmp1, in, tmp1);
  /* ---- tmp2 = 1 - tmp1 ---- */
  mp_load_1 (n, radix, tmp2);
  mp_sub (n, radix, tmp2, tmp1, tmp2);
  /* ---- get precision ---- */
  prc = -tmp2[1];
  if (tmp2[0] == 0)
    {
      prc = nfft + 1;
    }
  /* ---- tmp2 *= inout / 2 (half precision) ---- */
  mp_mulh_use_in1fft (n_h, radix, tmp1fft, 0, tmp2, tmp2, nfft, tmp2fft);
  mp_idiv_2 (n_h, radix, tmp2, tmp2);
  /* ---- inout += tmp2 ---- */
  mp_add (n, radix, inout, tmp2, inout);
  return prc;
}


/* -------- mp_io routines -------- */


void
mp_sprintf (int n, int log10_radix, int in[], char out[])
{
  int j, k, x, y, outexp, shift;
  DGTINT *inr;

  inr = ((DGTINT *) & in[2]) - 2;
  if (in[0] < 0)
    {
      *out++ = '-';
    }
  x = inr[2];
  shift = log10_radix;
  for (k = log10_radix; k > 0; k--)
    {
      y = x % 10;
      x /= 10;
      out[k] = '0' + y;
      if (y != 0)
	{
	  shift = k;
	}
    }
  out[0] = out[shift];
  out[1] = '.';
  for (k = 1; k <= log10_radix - shift; k++)
    {
      out[k + 1] = out[k + shift];
    }
  outexp = log10_radix - shift;
  out += outexp + 2;
  for (j = 3; j <= n + 1; j++)
    {
      x = inr[j];
      for (k = log10_radix - 1; k >= 0; k--)
	{
	  y = x % 10;
	  x /= 10;
	  out[k] = '0' + y;
	}
      out += log10_radix;
    }
  *out++ = 'e';
  outexp += log10_radix * in[1];
  sprintf (out, "%d", outexp);
}


void
mp_sscanf (int n, int log10_radix, char in[], int out[])
{
  char *s;
  int j, x, outexp, outexp_mod;
  DGTINT *outr;

  outr = ((DGTINT *) & out[2]) - 2;
  while (*in == ' ')
    {
      in++;
    }
  out[0] = 1;
  if (*in == '-')
    {
      out[0] = -1;
      in++;
    }
  else if (*in == '+')
    {
      in++;
    }
  while (*in == ' ' || *in == '0')
    {
      in++;
    }
  outexp = 0;
  for (s = in; *s != '\0'; s++)
    {
      if (*s == 'e' || *s == 'E' || *s == 'd' || *s == 'D')
	{
	  if (sscanf (++s, "%d", &outexp) != 1)
	    {
	      outexp = 0;
	    }
	  break;
	}
    }
  if (*in == '.')
    {
      do
	{
	  outexp--;
	  while (*++in == ' ');
	}
      while (*in == '0' && *in != '\0');
    }
  else if (*in != '\0')
    {
      s = in;
      while (*++s == ' ');
      while (*s >= '0' && *s <= '9' && *s != '\0')
	{
	  outexp++;
	  while (*++s == ' ');
	}
    }
  x = outexp / log10_radix;
  outexp_mod = outexp - log10_radix * x;
  if (outexp_mod < 0)
    {
      x--;
      outexp_mod += log10_radix;
    }
  out[1] = x;
  x = 0;
  j = 2;
  for (s = in; *s != '\0'; s++)
    {
      if (*s == '.' || *s == ' ')
	{
	  continue;
	}
      if (*s < '0' || *s > '9')
	{
	  break;
	}
      x = 10 * x + (*s - '0');
      if (--outexp_mod < 0)
	{
	  if (j > n + 1)
	    {
	      break;
	    }
	  outr[j++] = (DGTINT) x;
	  x = 0;
	  outexp_mod = log10_radix - 1;
	}
    }
  while (outexp_mod-- >= 0)
    {
      x *= 10;
    }
  while (j <= n + 1)
    {
      outr[j++] = (DGTINT) x;
      x = 0;
    }
  if (outr[2] == 0)
    {
      out[0] = 0;
      out[1] = 0;
    }
}

/* ---- FFT (Ooura fftsg_h, verbatim) ---- */
/*
Fast Fourier/Cosine/Sine Transform
    dimension   :one
    data length :power of 2
    decimation  :frequency
    radix       :split-radix
    data        :inplace
    table       :not use
functions
    cdft: Complex Discrete Fourier Transform
    rdft: Real Discrete Fourier Transform
    ddct: Discrete Cosine Transform
    ddst: Discrete Sine Transform
    dfct: Cosine Transform of RDFT (Real Symmetric DFT)
    dfst: Sine Transform of RDFT (Real Anti-symmetric DFT)
function prototypes
    void cdft(int, int, double *);
    void rdft(int, int, double *);
    void ddct(int, int, double *);
    void ddst(int, int, double *);
    void dfct(int, double *);
    void dfst(int, double *);


-------- Complex DFT (Discrete Fourier Transform) --------
    [definition]
        <case1>
            X[k] = sum_j=0^n-1 x[j]*exp(2*pi*i*j*k/n), 0<=k<n
        <case2>
            X[k] = sum_j=0^n-1 x[j]*exp(-2*pi*i*j*k/n), 0<=k<n
        (notes: sum_j=0^n-1 is a summation from j=0 to n-1)
    [usage]
        <case1>
            cdft(2*n, 1, a);
        <case2>
            cdft(2*n, -1, a);
    [parameters]
        2*n            :data length (int)
                        n >= 1, n = power of 2
        a[0...2*n-1]   :input/output data (double *)
                        input data
                            a[2*j] = Re(x[j]), 
                            a[2*j+1] = Im(x[j]), 0<=j<n
                        output data
                            a[2*k] = Re(X[k]), 
                            a[2*k+1] = Im(X[k]), 0<=k<n
    [remark]
        Inverse of 
            cdft(2*n, -1, a);
        is 
            cdft(2*n, 1, a);
            for (j = 0; j <= 2 * n - 1; j++) {
                a[j] *= 1.0 / n;
            }
        .


-------- Real DFT / Inverse of Real DFT --------
    [definition]
        <case1> RDFT
            R[k] = sum_j=0^n-1 a[j]*cos(2*pi*j*k/n), 0<=k<=n/2
            I[k] = sum_j=0^n-1 a[j]*sin(2*pi*j*k/n), 0<k<n/2
        <case2> IRDFT (excluding scale)
            a[k] = (R[0] + R[n/2]*cos(pi*k))/2 + 
                   sum_j=1^n/2-1 R[j]*cos(2*pi*j*k/n) + 
                   sum_j=1^n/2-1 I[j]*sin(2*pi*j*k/n), 0<=k<n
    [usage]
        <case1>
            rdft(n, 1, a);
        <case2>
            rdft(n, -1, a);
    [parameters]
        n              :data length (int)
                        n >= 2, n = power of 2
        a[0...n-1]     :input/output data (double *)
                        <case1>
                            output data
                                a[2*k] = R[k], 0<=k<n/2
                                a[2*k+1] = I[k], 0<k<n/2
                                a[1] = R[n/2]
                        <case2>
                            input data
                                a[2*j] = R[j], 0<=j<n/2
                                a[2*j+1] = I[j], 0<j<n/2
                                a[1] = R[n/2]
    [remark]
        Inverse of 
            rdft(n, 1, a);
        is 
            rdft(n, -1, a);
            for (j = 0; j <= n - 1; j++) {
                a[j] *= 2.0 / n;
            }
        .


-------- DCT (Discrete Cosine Transform) / Inverse of DCT --------
    [definition]
        <case1> IDCT (excluding scale)
            C[k] = sum_j=0^n-1 a[j]*cos(pi*j*(k+1/2)/n), 0<=k<n
        <case2> DCT
            C[k] = sum_j=0^n-1 a[j]*cos(pi*(j+1/2)*k/n), 0<=k<n
    [usage]
        <case1>
            ddct(n, 1, a);
        <case2>
            ddct(n, -1, a);
    [parameters]
        n              :data length (int)
                        n >= 2, n = power of 2
        a[0...n-1]     :input/output data (double *)
                        output data
                            a[k] = C[k], 0<=k<n
    [remark]
        Inverse of 
            ddct(n, -1, a);
        is 
            a[0] *= 0.5;
            ddct(n, 1, a);
            for (j = 0; j <= n - 1; j++) {
                a[j] *= 2.0 / n;
            }
        .


-------- DST (Discrete Sine Transform) / Inverse of DST --------
    [definition]
        <case1> IDST (excluding scale)
            S[k] = sum_j=1^n A[j]*sin(pi*j*(k+1/2)/n), 0<=k<n
        <case2> DST
            S[k] = sum_j=0^n-1 a[j]*sin(pi*(j+1/2)*k/n), 0<k<=n
    [usage]
        <case1>
            ddst(n, 1, a);
        <case2>
            ddst(n, -1, a);
    [parameters]
        n              :data length (int)
                        n >= 2, n = power of 2
        a[0...n-1]     :input/output data (double *)
                        <case1>
                            input data
                                a[j] = A[j], 0<j<n
                                a[0] = A[n]
                            output data
                                a[k] = S[k], 0<=k<n
                        <case2>
                            output data
                                a[k] = S[k], 0<k<n
                                a[0] = S[n]
    [remark]
        Inverse of 
            ddst(n, -1, a);
        is 
            a[0] *= 0.5;
            ddst(n, 1, a);
            for (j = 0; j <= n - 1; j++) {
                a[j] *= 2.0 / n;
            }
        .


-------- Cosine Transform of RDFT (Real Symmetric DFT) --------
    [definition]
        C[k] = sum_j=0^n a[j]*cos(pi*j*k/n), 0<=k<=n
    [usage]
        dfct(n, a);
    [parameters]
        n              :data length - 1 (int)
                        n >= 2, n = power of 2
        a[0...n]       :input/output data (double *)
                        output data
                            a[k] = C[k], 0<=k<=n
    [remark]
        Inverse of 
            a[0] *= 0.5;
            a[n] *= 0.5;
            dfct(n, a);
        is 
            a[0] *= 0.5;
            a[n] *= 0.5;
            dfct(n, a);
            for (j = 0; j <= n; j++) {
                a[j] *= 2.0 / n;
            }
        .


-------- Sine Transform of RDFT (Real Anti-symmetric DFT) --------
    [definition]
        S[k] = sum_j=1^n-1 a[j]*sin(pi*j*k/n), 0<k<n
    [usage]
        dfst(n, a);
    [parameters]
        n              :data length + 1 (int)
                        n >= 2, n = power of 2
        a[0...n-1]     :input/output data (double *)
                        output data
                            a[k] = S[k], 0<k<n
                        (a[0] is used for work area)
    [remark]
        Inverse of 
            dfst(n, a);
        is 
            dfst(n, a);
            for (j = 1; j <= n - 1; j++) {
                a[j] *= 2.0 / n;
            }
        .
*/


void
cdft (int n, int isgn, double *a)
{
  void cftfsub (int n, double *a);
  void cftbsub (int n, double *a);

  if (isgn >= 0)
    {
      cftfsub (n, a);
    }
  else
    {
      cftbsub (n, a);
    }
}


void
rdft (int n, int isgn, double *a)
{
  void cftfsub (int n, double *a);
  void cftbsub (int n, double *a);
  void rftfsub (int n, double *a);
  void rftbsub (int n, double *a);
  double xi;

  if (isgn >= 0)
    {
      if (n > 4)
	{
	  cftfsub (n, a);
	  rftfsub (n, a);
	}
      else if (n == 4)
	{
	  cftfsub (n, a);
	}
      xi = a[0] - a[1];
      a[0] += a[1];
      a[1] = xi;
    }
  else
    {
      a[1] = 0.5 * (a[0] - a[1]);
      a[0] -= a[1];
      if (n > 4)
	{
	  rftbsub (n, a);
	  cftbsub (n, a);
	}
      else if (n == 4)
	{
	  cftbsub (n, a);
	}
    }
}


void
ddct (int n, int isgn, double *a)
{
  void cftfsub (int n, double *a);
  void cftbsub (int n, double *a);
  void rftfsub (int n, double *a);
  void rftbsub (int n, double *a);
  void dctsub (int n, double *a);
  void dctsub4 (int n, double *a);
  int j;
  double xr;

  if (isgn < 0)
    {
      xr = a[n - 1];
      for (j = n - 2; j >= 2; j -= 2)
	{
	  a[j + 1] = a[j] - a[j - 1];
	  a[j] += a[j - 1];
	}
      a[1] = a[0] - xr;
      a[0] += xr;
      if (n > 4)
	{
	  rftbsub (n, a);
	  cftbsub (n, a);
	}
      else if (n == 4)
	{
	  cftbsub (n, a);
	}
    }
  if (n > 4)
    {
      dctsub (n, a);
    }
  else
    {
      dctsub4 (n, a);
    }
  if (isgn >= 0)
    {
      if (n > 4)
	{
	  cftfsub (n, a);
	  rftfsub (n, a);
	}
      else if (n == 4)
	{
	  cftfsub (n, a);
	}
      xr = a[0] - a[1];
      a[0] += a[1];
      for (j = 2; j < n; j += 2)
	{
	  a[j - 1] = a[j] - a[j + 1];
	  a[j] += a[j + 1];
	}
      a[n - 1] = xr;
    }
}


void
ddst (int n, int isgn, double *a)
{
  void cftfsub (int n, double *a);
  void cftbsub (int n, double *a);
  void rftfsub (int n, double *a);
  void rftbsub (int n, double *a);
  void dstsub (int n, double *a);
  void dstsub4 (int n, double *a);
  int j;
  double xr;

  if (isgn < 0)
    {
      xr = a[n - 1];
      for (j = n - 2; j >= 2; j -= 2)
	{
	  a[j + 1] = -a[j] - a[j - 1];
	  a[j] -= a[j - 1];
	}
      a[1] = a[0] + xr;
      a[0] -= xr;
      if (n > 4)
	{
	  rftbsub (n, a);
	  cftbsub (n, a);
	}
      else if (n == 4)
	{
	  cftbsub (n, a);
	}
    }
  if (n > 4)
    {
      dstsub (n, a);
    }
  else
    {
      dstsub4 (n, a);
    }
  if (isgn >= 0)
    {
      if (n > 4)
	{
	  cftfsub (n, a);
	  rftfsub (n, a);
	}
      else if (n == 4)
	{
	  cftfsub (n, a);
	}
      xr = a[0] - a[1];
      a[0] += a[1];
      for (j = 2; j < n; j += 2)
	{
	  a[j - 1] = -a[j] - a[j + 1];
	  a[j] -= a[j + 1];
	}
      a[n - 1] = -xr;
    }
}


void
dfct (int n, double *a)
{
  void ddct (int n, int isgn, double *a);
  void bitrv1 (int n, double *a);
  int j, k, m, mh;
  double xr, xi, yr, yi, an;

  m = n >> 1;
  for (j = 0; j < m; j++)
    {
      k = n - j;
      xr = a[j] + a[k];
      a[j] -= a[k];
      a[k] = xr;
    }
  an = a[n];
  while (m >= 2)
    {
      ddct (m, 1, a);
      bitrv1 (m, a);
      mh = m >> 1;
      xi = a[m];
      a[m] = a[0];
      a[0] = an - xi;
      an += xi;
      for (j = 1; j < mh; j++)
	{
	  k = m - j;
	  xr = a[m + k];
	  xi = a[m + j];
	  yr = a[j];
	  yi = a[k];
	  a[m + j] = yr;
	  a[m + k] = yi;
	  a[j] = xr - xi;
	  a[k] = xr + xi;
	}
      xr = a[mh];
      a[mh] = a[m + mh];
      a[m + mh] = xr;
      m = mh;
    }
  xi = a[1];
  a[1] = a[0];
  a[0] = an + xi;
  a[n] = an - xi;
  bitrv1 (n, a);
}


void
dfst (int n, double *a)
{
  void ddst (int n, int isgn, double *a);
  void bitrv1 (int n, double *a);
  int j, k, m, mh;
  double xr, xi, yr, yi;

  m = n >> 1;
  for (j = 1; j < m; j++)
    {
      k = n - j;
      xr = a[j] - a[k];
      a[j] += a[k];
      a[k] = xr;
    }
  a[0] = a[m];
  while (m >= 2)
    {
      ddst (m, 1, a);
      bitrv1 (m, a);
      mh = m >> 1;
      for (j = 1; j < mh; j++)
	{
	  k = m - j;
	  xr = a[m + k];
	  xi = a[m + j];
	  yr = a[j];
	  yi = a[k];
	  a[m + j] = yr;
	  a[m + k] = yi;
	  a[j] = xr + xi;
	  a[k] = xr - xi;
	}
      a[m] = a[0];
      a[0] = a[m + mh];
      a[m + mh] = a[mh];
      m = mh;
    }
  a[1] = a[0];
  a[0] = 0;
  bitrv1 (n, a);
}


/* -------- child routines -------- */


#include <math.h>
#ifndef M_PI_2
#define M_PI_2      1.570796326794896619231321691639751442098584699687
#endif
#ifndef WR5000			/* cos(M_PI_2*0.5000) */
#define WR5000      0.707106781186547524400844362104849039284835937688
#endif
#ifndef WR2500			/* cos(M_PI_2*0.2500) */
#define WR2500      0.923879532511286756128183189396788286822416625863
#endif
#ifndef WI2500			/* sin(M_PI_2*0.2500) */
#define WI2500      0.382683432365089771728459984030398866761344562485
#endif
#ifndef WR1250			/* cos(M_PI_2*0.1250) */
#define WR1250      0.980785280403230449126182236134239036973933730893
#endif
#ifndef WI1250			/* sin(M_PI_2*0.1250) */
#define WI1250      0.195090322016128267848284868477022240927691617751
#endif
#ifndef WR3750			/* cos(M_PI_2*0.3750) */
#define WR3750      0.831469612302545237078788377617905756738560811987
#endif
#ifndef WI3750			/* sin(M_PI_2*0.3750) */
#define WI3750      0.555570233019602224742830813948532874374937190754
#endif


#ifndef CDFT_RECURSIVE_N	/* length of the recursive FFT mode */
#define CDFT_RECURSIVE_N 512	/* <= (L1 cache size) / 16 */
#endif

#ifndef CDFT_LOOP_DIV		/* control of the CDFT's speed & tolerance */
#define CDFT_LOOP_DIV 32
#endif

#ifndef RDFT_LOOP_DIV		/* control of the RDFT's speed & tolerance */
#define RDFT_LOOP_DIV 64
#endif

#ifndef DCST_LOOP_DIV		/* control of the DCT,DST's speed & tolerance */
#define DCST_LOOP_DIV 64
#endif


void
cftfsub (int n, double *a)
{
  void bitrv2 (int n, double *a);
  void bitrv216 (double *a);
  void bitrv208 (double *a);
  void cftmdl1 (int n, double *a);
  void cftrec1 (int n, double *a);
  void cftrec2 (int n, double *a);
  void cftexp1 (int n, double *a);
  void cftfx41 (int n, double *a);
  void cftf161 (double *a);
  void cftf081 (double *a);
  void cftf040 (double *a);
  void cftx020 (double *a);
  int m;

  if (n > 32)
    {
      m = n >> 2;
      cftmdl1 (n, a);
      if (n > CDFT_RECURSIVE_N)
	{
	  cftrec1 (m, a);
	  cftrec2 (m, &a[m]);
	  cftrec1 (m, &a[2 * m]);
	  cftrec1 (m, &a[3 * m]);
	}
      else if (m > 32)
	{
	  cftexp1 (n, a);
	}
      else
	{
	  cftfx41 (n, a);
	}
      bitrv2 (n, a);
    }
  else if (n > 8)
    {
      if (n == 32)
	{
	  cftf161 (a);
	  bitrv216 (a);
	}
      else
	{
	  cftf081 (a);
	  bitrv208 (a);
	}
    }
  else if (n == 8)
    {
      cftf040 (a);
    }
  else if (n == 4)
    {
      cftx020 (a);
    }
}


void
cftbsub (int n, double *a)
{
  void bitrv2conj (int n, double *a);
  void bitrv216neg (double *a);
  void bitrv208neg (double *a);
  void cftb1st (int n, double *a);
  void cftrec1 (int n, double *a);
  void cftrec2 (int n, double *a);
  void cftexp1 (int n, double *a);
  void cftfx41 (int n, double *a);
  void cftf161 (double *a);
  void cftf081 (double *a);
  void cftb040 (double *a);
  void cftx020 (double *a);
  int m;

  if (n > 32)
    {
      m = n >> 2;
      cftb1st (n, a);
      if (n > CDFT_RECURSIVE_N)
	{
	  cftrec1 (m, a);
	  cftrec2 (m, &a[m]);
	  cftrec1 (m, &a[2 * m]);
	  cftrec1 (m, &a[3 * m]);
	}
      else if (m > 32)
	{
	  cftexp1 (n, a);
	}
      else
	{
	  cftfx41 (n, a);
	}
      bitrv2conj (n, a);
    }
  else if (n > 8)
    {
      if (n == 32)
	{
	  cftf161 (a);
	  bitrv216neg (a);
	}
      else
	{
	  cftf081 (a);
	  bitrv208neg (a);
	}
    }
  else if (n == 8)
    {
      cftb040 (a);
    }
  else if (n == 4)
    {
      cftx020 (a);
    }
}


void
bitrv2 (int n, double *a)
{
  int j0, k0, j1, k1, l, m, i, j, k;
  double xr, xi, yr, yi;

  l = n >> 2;
  m = 2;
  while (m < l)
    {
      l >>= 1;
      m <<= 1;
    }
  if (m == l)
    {
      j0 = 0;
      for (k0 = 0; k0 < m; k0 += 2)
	{
	  k = k0;
	  for (j = j0; j < j0 + k0; j += 2)
	    {
	      xr = a[j];
	      xi = a[j + 1];
	      yr = a[k];
	      yi = a[k + 1];
	      a[j] = yr;
	      a[j + 1] = yi;
	      a[k] = xr;
	      a[k + 1] = xi;
	      j1 = j + m;
	      k1 = k + 2 * m;
	      xr = a[j1];
	      xi = a[j1 + 1];
	      yr = a[k1];
	      yi = a[k1 + 1];
	      a[j1] = yr;
	      a[j1 + 1] = yi;
	      a[k1] = xr;
	      a[k1 + 1] = xi;
	      j1 += m;
	      k1 -= m;
	      xr = a[j1];
	      xi = a[j1 + 1];
	      yr = a[k1];
	      yi = a[k1 + 1];
	      a[j1] = yr;
	      a[j1 + 1] = yi;
	      a[k1] = xr;
	      a[k1 + 1] = xi;
	      j1 += m;
	      k1 += 2 * m;
	      xr = a[j1];
	      xi = a[j1 + 1];
	      yr = a[k1];
	      yi = a[k1 + 1];
	      a[j1] = yr;
	      a[j1 + 1] = yi;
	      a[k1] = xr;
	      a[k1 + 1] = xi;
	      for (i = n >> 1; i > (k ^= i); i >>= 1);
	    }
	  j1 = j0 + k0 + m;
	  k1 = j1 + m;
	  xr = a[j1];
	  xi = a[j1 + 1];
	  yr = a[k1];
	  yi = a[k1 + 1];
	  a[j1] = yr;
	  a[j1 + 1] = yi;
	  a[k1] = xr;
	  a[k1 + 1] = xi;
	  for (i = n >> 1; i > (j0 ^= i); i >>= 1);
	}
    }
  else
    {
      j0 = 0;
      for (k0 = 2; k0 < m; k0 += 2)
	{
	  for (i = n >> 1; i > (j0 ^= i); i >>= 1);
	  k = k0;
	  for (j = j0; j < j0 + k0; j += 2)
	    {
	      xr = a[j];
	      xi = a[j + 1];
	      yr = a[k];
	      yi = a[k + 1];
	      a[j] = yr;
	      a[j + 1] = yi;
	      a[k] = xr;
	      a[k + 1] = xi;
	      j1 = j + m;
	      k1 = k + m;
	      xr = a[j1];
	      xi = a[j1 + 1];
	      yr = a[k1];
	      yi = a[k1 + 1];
	      a[j1] = yr;
	      a[j1 + 1] = yi;
	      a[k1] = xr;
	      a[k1 + 1] = xi;
	      for (i = n >> 1; i > (k ^= i); i >>= 1);
	    }
	}
    }
}


void
bitrv2conj (int n, double *a)
{
  int j0, k0, j1, k1, l, m, i, j, k;
  double xr, xi, yr, yi;

  l = n >> 2;
  m = 2;
  while (m < l)
    {
      l >>= 1;
      m <<= 1;
    }
  if (m == l)
    {
      j0 = 0;
      for (k0 = 0; k0 < m; k0 += 2)
	{
	  k = k0;
	  for (j = j0; j < j0 + k0; j += 2)
	    {
	      xr = a[j];
	      xi = -a[j + 1];
	      yr = a[k];
	      yi = -a[k + 1];
	      a[j] = yr;
	      a[j + 1] = yi;
	      a[k] = xr;
	      a[k + 1] = xi;
	      j1 = j + m;
	      k1 = k + 2 * m;
	      xr = a[j1];
	      xi = -a[j1 + 1];
	      yr = a[k1];
	      yi = -a[k1 + 1];
	      a[j1] = yr;
	      a[j1 + 1] = yi;
	      a[k1] = xr;
	      a[k1 + 1] = xi;
	      j1 += m;
	      k1 -= m;
	      xr = a[j1];
	      xi = -a[j1 + 1];
	      yr = a[k1];
	      yi = -a[k1 + 1];
	      a[j1] = yr;
	      a[j1 + 1] = yi;
	      a[k1] = xr;
	      a[k1 + 1] = xi;
	      j1 += m;
	      k1 += 2 * m;
	      xr = a[j1];
	      xi = -a[j1 + 1];
	      yr = a[k1];
	      yi = -a[k1 + 1];
	      a[j1] = yr;
	      a[j1 + 1] = yi;
	      a[k1] = xr;
	      a[k1 + 1] = xi;
	      for (i = n >> 1; i > (k ^= i); i >>= 1);
	    }
	  k1 = j0 + k0;
	  a[k1 + 1] = -a[k1 + 1];
	  j1 = k1 + m;
	  k1 = j1 + m;
	  xr = a[j1];
	  xi = -a[j1 + 1];
	  yr = a[k1];
	  yi = -a[k1 + 1];
	  a[j1] = yr;
	  a[j1 + 1] = yi;
	  a[k1] = xr;
	  a[k1 + 1] = xi;
	  k1 += m;
	  a[k1 + 1] = -a[k1 + 1];
	  for (i = n >> 1; i > (j0 ^= i); i >>= 1);
	}
    }
  else
    {
      a[1] = -a[1];
      a[m + 1] = -a[m + 1];
      j0 = 0;
      for (k0 = 2; k0 < m; k0 += 2)
	{
	  for (i = n >> 1; i > (j0 ^= i); i >>= 1);
	  k = k0;
	  for (j = j0; j < j0 + k0; j += 2)
	    {
	      xr = a[j];
	      xi = -a[j + 1];
	      yr = a[k];
	      yi = -a[k + 1];
	      a[j] = yr;
	      a[j + 1] = yi;
	      a[k] = xr;
	      a[k + 1] = xi;
	      j1 = j + m;
	      k1 = k + m;
	      xr = a[j1];
	      xi = -a[j1 + 1];
	      yr = a[k1];
	      yi = -a[k1 + 1];
	      a[j1] = yr;
	      a[j1 + 1] = yi;
	      a[k1] = xr;
	      a[k1 + 1] = xi;
	      for (i = n >> 1; i > (k ^= i); i >>= 1);
	    }
	  k1 = j0 + k0;
	  a[k1 + 1] = -a[k1 + 1];
	  a[k1 + m + 1] = -a[k1 + m + 1];
	}
    }
}


void
bitrv216 (double *a)
{
  double x1r, x1i, x2r, x2i, x3r, x3i, x4r, x4i,
    x5r, x5i, x7r, x7i, x8r, x8i, x10r, x10i,
    x11r, x11i, x12r, x12i, x13r, x13i, x14r, x14i;

  x1r = a[2];
  x1i = a[3];
  x2r = a[4];
  x2i = a[5];
  x3r = a[6];
  x3i = a[7];
  x4r = a[8];
  x4i = a[9];
  x5r = a[10];
  x5i = a[11];
  x7r = a[14];
  x7i = a[15];
  x8r = a[16];
  x8i = a[17];
  x10r = a[20];
  x10i = a[21];
  x11r = a[22];
  x11i = a[23];
  x12r = a[24];
  x12i = a[25];
  x13r = a[26];
  x13i = a[27];
  x14r = a[28];
  x14i = a[29];
  a[2] = x8r;
  a[3] = x8i;
  a[4] = x4r;
  a[5] = x4i;
  a[6] = x12r;
  a[7] = x12i;
  a[8] = x2r;
  a[9] = x2i;
  a[10] = x10r;
  a[11] = x10i;
  a[14] = x14r;
  a[15] = x14i;
  a[16] = x1r;
  a[17] = x1i;
  a[20] = x5r;
  a[21] = x5i;
  a[22] = x13r;
  a[23] = x13i;
  a[24] = x3r;
  a[25] = x3i;
  a[26] = x11r;
  a[27] = x11i;
  a[28] = x7r;
  a[29] = x7i;
}


void
bitrv216neg (double *a)
{
  double x1r, x1i, x2r, x2i, x3r, x3i, x4r, x4i,
    x5r, x5i, x6r, x6i, x7r, x7i, x8r, x8i,
    x9r, x9i, x10r, x10i, x11r, x11i, x12r, x12i,
    x13r, x13i, x14r, x14i, x15r, x15i;

  x1r = a[2];
  x1i = a[3];
  x2r = a[4];
  x2i = a[5];
  x3r = a[6];
  x3i = a[7];
  x4r = a[8];
  x4i = a[9];
  x5r = a[10];
  x5i = a[11];
  x6r = a[12];
  x6i = a[13];
  x7r = a[14];
  x7i = a[15];
  x8r = a[16];
  x8i = a[17];
  x9r = a[18];
  x9i = a[19];
  x10r = a[20];
  x10i = a[21];
  x11r = a[22];
  x11i = a[23];
  x12r = a[24];
  x12i = a[25];
  x13r = a[26];
  x13i = a[27];
  x14r = a[28];
  x14i = a[29];
  x15r = a[30];
  x15i = a[31];
  a[2] = x15r;
  a[3] = x15i;
  a[4] = x7r;
  a[5] = x7i;
  a[6] = x11r;
  a[7] = x11i;
  a[8] = x3r;
  a[9] = x3i;
  a[10] = x13r;
  a[11] = x13i;
  a[12] = x5r;
  a[13] = x5i;
  a[14] = x9r;
  a[15] = x9i;
  a[16] = x1r;
  a[17] = x1i;
  a[18] = x14r;
  a[19] = x14i;
  a[20] = x6r;
  a[21] = x6i;
  a[22] = x10r;
  a[23] = x10i;
  a[24] = x2r;
  a[25] = x2i;
  a[26] = x12r;
  a[27] = x12i;
  a[28] = x4r;
  a[29] = x4i;
  a[30] = x8r;
  a[31] = x8i;
}


void
bitrv208 (double *a)
{
  double x1r, x1i, x3r, x3i, x4r, x4i, x6r, x6i;

  x1r = a[2];
  x1i = a[3];
  x3r = a[6];
  x3i = a[7];
  x4r = a[8];
  x4i = a[9];
  x6r = a[12];
  x6i = a[13];
  a[2] = x4r;
  a[3] = x4i;
  a[6] = x6r;
  a[7] = x6i;
  a[8] = x1r;
  a[9] = x1i;
  a[12] = x3r;
  a[13] = x3i;
}


void
bitrv208neg (double *a)
{
  double x1r, x1i, x2r, x2i, x3r, x3i, x4r, x4i, x5r, x5i, x6r, x6i, x7r, x7i;

  x1r = a[2];
  x1i = a[3];
  x2r = a[4];
  x2i = a[5];
  x3r = a[6];
  x3i = a[7];
  x4r = a[8];
  x4i = a[9];
  x5r = a[10];
  x5i = a[11];
  x6r = a[12];
  x6i = a[13];
  x7r = a[14];
  x7i = a[15];
  a[2] = x7r;
  a[3] = x7i;
  a[4] = x3r;
  a[5] = x3i;
  a[6] = x5r;
  a[7] = x5i;
  a[8] = x1r;
  a[9] = x1i;
  a[10] = x6r;
  a[11] = x6i;
  a[12] = x2r;
  a[13] = x2i;
  a[14] = x4r;
  a[15] = x4i;
}


void
bitrv1 (int n, double *a)
{
  int j0, k0, j1, k1, l, m, i, j, k;
  double x;

  l = n >> 2;
  m = 1;
  while (m < l)
    {
      l >>= 1;
      m <<= 1;
    }
  if (m == l)
    {
      j0 = 0;
      for (k0 = 0; k0 < m; k0++)
	{
	  k = k0;
	  for (j = j0; j < j0 + k0; j++)
	    {
	      x = a[j];
	      a[j] = a[k];
	      a[k] = x;
	      j1 = j + m;
	      k1 = k + 2 * m;
	      x = a[j1];
	      a[j1] = a[k1];
	      a[k1] = x;
	      j1 += m;
	      k1 -= m;
	      x = a[j1];
	      a[j1] = a[k1];
	      a[k1] = x;
	      j1 += m;
	      k1 += 2 * m;
	      x = a[j1];
	      a[j1] = a[k1];
	      a[k1] = x;
	      for (i = n >> 1; i > (k ^= i); i >>= 1);
	    }
	  j1 = j0 + k0 + m;
	  k1 = j1 + m;
	  x = a[j1];
	  a[j1] = a[k1];
	  a[k1] = x;
	  for (i = n >> 1; i > (j0 ^= i); i >>= 1);
	}
    }
  else
    {
      j0 = 0;
      for (k0 = 1; k0 < m; k0++)
	{
	  for (i = n >> 1; i > (j0 ^= i); i >>= 1);
	  k = k0;
	  for (j = j0; j < j0 + k0; j++)
	    {
	      x = a[j];
	      a[j] = a[k];
	      a[k] = x;
	      j1 = j + m;
	      k1 = k + m;
	      x = a[j1];
	      a[j1] = a[k1];
	      a[k1] = x;
	      for (i = n >> 1; i > (k ^= i); i >>= 1);
	    }
	}
    }
}


void
cftb1st (int n, double *a)
{
  int i, i0, j, j0, j1, j2, j3, m, mh;
  double ew, w1r, w1i, wk1r, wk1i, wk3r, wk3i,
    wd1r, wd1i, wd3r, wd3i, ss1, ss3;
  double x0r, x0i, x1r, x1i, x2r, x2i, x3r, x3i;

  mh = n >> 3;
  m = 2 * mh;
  j1 = m;
  j2 = j1 + m;
  j3 = j2 + m;
  x0r = a[0] + a[j2];
  x0i = -a[1] - a[j2 + 1];
  x1r = a[0] - a[j2];
  x1i = -a[1] + a[j2 + 1];
  x2r = a[j1] + a[j3];
  x2i = a[j1 + 1] + a[j3 + 1];
  x3r = a[j1] - a[j3];
  x3i = a[j1 + 1] - a[j3 + 1];
  a[0] = x0r + x2r;
  a[1] = x0i - x2i;
  a[j1] = x0r - x2r;
  a[j1 + 1] = x0i + x2i;
  a[j2] = x1r + x3i;
  a[j2 + 1] = x1i + x3r;
  a[j3] = x1r - x3i;
  a[j3 + 1] = x1i - x3r;
  wd1r = 1;
  wd1i = 0;
  wd3r = 1;
  wd3i = 0;
  ew = M_PI_2 / m;
  w1r = cos (2 * ew);
  w1i = sin (2 * ew);
  wk1r = w1r;
  wk1i = w1i;
  ss1 = 2 * w1i;
  wk3i = 2 * ss1 * wk1r;
  wk3r = wk1r - wk3i * wk1i;
  wk3i = wk1i - wk3i * wk1r;
  ss3 = 2 * wk3i;
  i = 0;
  for (;;)
    {
      i0 = i + 4 * CDFT_LOOP_DIV;
      if (i0 > mh - 4)
	{
	  i0 = mh - 4;
	}
      for (j = i + 2; j < i0; j += 4)
	{
	  wd1r -= ss1 * wk1i;
	  wd1i += ss1 * wk1r;
	  wd3r -= ss3 * wk3i;
	  wd3i += ss3 * wk3r;
	  j1 = j + m;
	  j2 = j1 + m;
	  j3 = j2 + m;
	  x0r = a[j] + a[j2];
	  x0i = -a[j + 1] - a[j2 + 1];
	  x1r = a[j] - a[j2];
	  x1i = -a[j + 1] + a[j2 + 1];
	  x2r = a[j1] + a[j3];
	  x2i = a[j1 + 1] + a[j3 + 1];
	  x3r = a[j1] - a[j3];
	  x3i = a[j1 + 1] - a[j3 + 1];
	  a[j] = x0r + x2r;
	  a[j + 1] = x0i - x2i;
	  a[j1] = x0r - x2r;
	  a[j1 + 1] = x0i + x2i;
	  x0r = x1r + x3i;
	  x0i = x1i + x3r;
	  a[j2] = wk1r * x0r - wk1i * x0i;
	  a[j2 + 1] = wk1r * x0i + wk1i * x0r;
	  x0r = x1r - x3i;
	  x0i = x1i - x3r;
	  a[j3] = wk3r * x0r + wk3i * x0i;
	  a[j3 + 1] = wk3r * x0i - wk3i * x0r;
	  x0r = a[j + 2] + a[j2 + 2];
	  x0i = -a[j + 3] - a[j2 + 3];
	  x1r = a[j + 2] - a[j2 + 2];
	  x1i = -a[j + 3] + a[j2 + 3];
	  x2r = a[j1 + 2] + a[j3 + 2];
	  x2i = a[j1 + 3] + a[j3 + 3];
	  x3r = a[j1 + 2] - a[j3 + 2];
	  x3i = a[j1 + 3] - a[j3 + 3];
	  a[j + 2] = x0r + x2r;
	  a[j + 3] = x0i - x2i;
	  a[j1 + 2] = x0r - x2r;
	  a[j1 + 3] = x0i + x2i;
	  x0r = x1r + x3i;
	  x0i = x1i + x3r;
	  a[j2 + 2] = wd1r * x0r - wd1i * x0i;
	  a[j2 + 3] = wd1r * x0i + wd1i * x0r;
	  x0r = x1r - x3i;
	  x0i = x1i - x3r;
	  a[j3 + 2] = wd3r * x0r + wd3i * x0i;
	  a[j3 + 3] = wd3r * x0i - wd3i * x0r;
	  j0 = m - j;
	  j1 = j0 + m;
	  j2 = j1 + m;
	  j3 = j2 + m;
	  x0r = a[j0] + a[j2];
	  x0i = -a[j0 + 1] - a[j2 + 1];
	  x1r = a[j0] - a[j2];
	  x1i = -a[j0 + 1] + a[j2 + 1];
	  x2r = a[j1] + a[j3];
	  x2i = a[j1 + 1] + a[j3 + 1];
	  x3r = a[j1] - a[j3];
	  x3i = a[j1 + 1] - a[j3 + 1];
	  a[j0] = x0r + x2r;
	  a[j0 + 1] = x0i - x2i;
	  a[j1] = x0r - x2r;
	  a[j1 + 1] = x0i + x2i;
	  x0r = x1r + x3i;
	  x0i = x1i + x3r;
	  a[j2] = wk1i * x0r - wk1r * x0i;
	  a[j2 + 1] = wk1i * x0i + wk1r * x0r;
	  x0r = x1r - x3i;
	  x0i = x1i - x3r;
	  a[j3] = wk3i * x0r + wk3r * x0i;
	  a[j3 + 1] = wk3i * x0i - wk3r * x0r;
	  x0r = a[j0 - 2] + a[j2 - 2];
	  x0i = -a[j0 - 1] - a[j2 - 1];
	  x1r = a[j0 - 2] - a[j2 - 2];
	  x1i = -a[j0 - 1] + a[j2 - 1];
	  x2r = a[j1 - 2] + a[j3 - 2];
	  x2i = a[j1 - 1] + a[j3 - 1];
	  x3r = a[j1 - 2] - a[j3 - 2];
	  x3i = a[j1 - 1] - a[j3 - 1];
	  a[j0 - 2] = x0r + x2r;
	  a[j0 - 1] = x0i - x2i;
	  a[j1 - 2] = x0r - x2r;
	  a[j1 - 1] = x0i + x2i;
	  x0r = x1r + x3i;
	  x0i = x1i + x3r;
	  a[j2 - 2] = wd1i * x0r - wd1r * x0i;
	  a[j2 - 1] = wd1i * x0i + wd1r * x0r;
	  x0r = x1r - x3i;
	  x0i = x1i - x3r;
	  a[j3 - 2] = wd3i * x0r + wd3r * x0i;
	  a[j3 - 1] = wd3i * x0i - wd3r * x0r;
	  wk1r -= ss1 * wd1i;
	  wk1i += ss1 * wd1r;
	  wk3r -= ss3 * wd3i;
	  wk3i += ss3 * wd3r;
	}
      if (i0 == mh - 4)
	{
	  break;
	}
      wd1r = cos (ew * i0);
      wd1i = sin (ew * i0);
      wd3i = 4 * wd1i * wd1r;
      wd3r = wd1r - wd3i * wd1i;
      wd3i = wd1i - wd3i * wd1r;
      wk1r = w1r * wd1r - w1i * wd1i;
      wk1i = w1r * wd1i + w1i * wd1r;
      wk3i = 4 * wk1i * wk1r;
      wk3r = wk1r - wk3i * wk1i;
      wk3i = wk1i - wk3i * wk1r;
      i = i0;
    }
  wd1r -= ss1 * wk1i;
  j0 = mh;
  j1 = j0 + m;
  j2 = j1 + m;
  j3 = j2 + m;
  x0r = a[j0 - 2] + a[j2 - 2];
  x0i = -a[j0 - 1] - a[j2 - 1];
  x1r = a[j0 - 2] - a[j2 - 2];
  x1i = -a[j0 - 1] + a[j2 - 1];
  x2r = a[j1 - 2] + a[j3 - 2];
  x2i = a[j1 - 1] + a[j3 - 1];
  x3r = a[j1 - 2] - a[j3 - 2];
  x3i = a[j1 - 1] - a[j3 - 1];
  a[j0 - 2] = x0r + x2r;
  a[j0 - 1] = x0i - x2i;
  a[j1 - 2] = x0r - x2r;
  a[j1 - 1] = x0i + x2i;
  x0r = x1r + x3i;
  x0i = x1i + x3r;
  a[j2 - 2] = wk1r * x0r - wk1i * x0i;
  a[j2 - 1] = wk1r * x0i + wk1i * x0r;
  x0r = x1r - x3i;
  x0i = x1i - x3r;
  a[j3 - 2] = wk3r * x0r + wk3i * x0i;
  a[j3 - 1] = wk3r * x0i - wk3i * x0r;
  x0r = a[j0] + a[j2];
  x0i = -a[j0 + 1] - a[j2 + 1];
  x1r = a[j0] - a[j2];
  x1i = -a[j0 + 1] + a[j2 + 1];
  x2r = a[j1] + a[j3];
  x2i = a[j1 + 1] + a[j3 + 1];
  x3r = a[j1] - a[j3];
  x3i = a[j1 + 1] - a[j3 + 1];
  a[j0] = x0r + x2r;
  a[j0 + 1] = x0i - x2i;
  a[j1] = x0r - x2r;
  a[j1 + 1] = x0i + x2i;
  x0r = x1r + x3i;
  x0i = x1i + x3r;
  a[j2] = wd1r * (x0r - x0i);
  a[j2 + 1] = wd1r * (x0i + x0r);
  x0r = x1r - x3i;
  x0i = x1i - x3r;
  a[j3] = -wd1r * (x0r + x0i);
  a[j3 + 1] = -wd1r * (x0i - x0r);
  x0r = a[j0 + 2] + a[j2 + 2];
  x0i = -a[j0 + 3] - a[j2 + 3];
  x1r = a[j0 + 2] - a[j2 + 2];
  x1i = -a[j0 + 3] + a[j2 + 3];
  x2r = a[j1 + 2] + a[j3 + 2];
  x2i = a[j1 + 3] + a[j3 + 3];
  x3r = a[j1 + 2] - a[j3 + 2];
  x3i = a[j1 + 3] - a[j3 + 3];
  a[j0 + 2] = x0r + x2r;
  a[j0 + 3] = x0i - x2i;
  a[j1 + 2] = x0r - x2r;
  a[j1 + 3] = x0i + x2i;
  x0r = x1r + x3i;
  x0i = x1i + x3r;
  a[j2 + 2] = wk1i * x0r - wk1r * x0i;
  a[j2 + 3] = wk1i * x0i + wk1r * x0r;
  x0r = x1r - x3i;
  x0i = x1i - x3r;
  a[j3 + 2] = wk3i * x0r + wk3r * x0i;
  a[j3 + 3] = wk3i * x0i - wk3r * x0r;
}


void
cftrec1 (int n, double *a)
{
  void cftrec1 (int n, double *a);
  void cftrec2 (int n, double *a);
  void cftmdl1 (int n, double *a);
  void cftexp1 (int n, double *a);
  int m;

  m = n >> 2;
  cftmdl1 (n, a);
  if (n > CDFT_RECURSIVE_N)
    {
      cftrec1 (m, a);
      cftrec2 (m, &a[m]);
      cftrec1 (m, &a[2 * m]);
      cftrec1 (m, &a[3 * m]);
    }
  else
    {
      cftexp1 (n, a);
    }
}


void
cftrec2 (int n, double *a)
{
  void cftrec1 (int n, double *a);
  void cftrec2 (int n, double *a);
  void cftmdl2 (int n, double *a);
  void cftexp2 (int n, double *a);
  int m;

  m = n >> 2;
  cftmdl2 (n, a);
  if (n > CDFT_RECURSIVE_N)
    {
      cftrec1 (m, a);
      cftrec2 (m, &a[m]);
      cftrec1 (m, &a[2 * m]);
      cftrec2 (m, &a[3 * m]);
    }
  else
    {
      cftexp2 (n, a);
    }
}


void
cftexp1 (int n, double *a)
{
  void cftmdl1 (int n, double *a);
  void cftmdl2 (int n, double *a);
  void cftfx41 (int n, double *a);
  void cftfx42 (int n, double *a);
  int j, k, l;

  l = n >> 2;
  while (l > 128)
    {
      for (k = l; k < n; k <<= 2)
	{
	  for (j = k - l; j < n; j += 4 * k)
	    {
	      cftmdl1 (l, &a[j]);
	      cftmdl2 (l, &a[k + j]);
	      cftmdl1 (l, &a[2 * k + j]);
	    }
	}
      cftmdl1 (l, &a[n - l]);
      l >>= 2;
    }
  for (k = l; k < n; k <<= 2)
    {
      for (j = k - l; j < n; j += 4 * k)
	{
	  cftmdl1 (l, &a[j]);
	  cftfx41 (l, &a[j]);
	  cftmdl2 (l, &a[k + j]);
	  cftfx42 (l, &a[k + j]);
	  cftmdl1 (l, &a[2 * k + j]);
	  cftfx41 (l, &a[2 * k + j]);
	}
    }
  cftmdl1 (l, &a[n - l]);
  cftfx41 (l, &a[n - l]);
}


void
cftexp2 (int n, double *a)
{
  void cftmdl1 (int n, double *a);
  void cftmdl2 (int n, double *a);
  void cftfx41 (int n, double *a);
  void cftfx42 (int n, double *a);
  int j, k, l, m;

  m = n >> 1;
  l = n >> 2;
  while (l > 128)
    {
      for (k = l; k < m; k <<= 2)
	{
	  for (j = k - l; j < m; j += 2 * k)
	    {
	      cftmdl1 (l, &a[j]);
	      cftmdl1 (l, &a[m + j]);
	    }
	  for (j = 2 * k - l; j < m; j += 4 * k)
	    {
	      cftmdl2 (l, &a[j]);
	      cftmdl2 (l, &a[m + j]);
	    }
	}
      l >>= 2;
    }
  for (k = l; k < m; k <<= 2)
    {
      for (j = k - l; j < m; j += 2 * k)
	{
	  cftmdl1 (l, &a[j]);
	  cftfx41 (l, &a[j]);
	  cftmdl1 (l, &a[m + j]);
	  cftfx41 (l, &a[m + j]);
	}
      for (j = 2 * k - l; j < m; j += 4 * k)
	{
	  cftmdl2 (l, &a[j]);
	  cftfx42 (l, &a[j]);
	  cftmdl2 (l, &a[m + j]);
	  cftfx42 (l, &a[m + j]);
	}
    }
}


void
cftmdl1 (int n, double *a)
{
  int i, i0, j, j0, j1, j2, j3, m, mh;
  double ew, w1r, w1i, wk1r, wk1i, wk3r, wk3i,
    wd1r, wd1i, wd3r, wd3i, ss1, ss3;
  double x0r, x0i, x1r, x1i, x2r, x2i, x3r, x3i;

  mh = n >> 3;
  m = 2 * mh;
  j1 = m;
  j2 = j1 + m;
  j3 = j2 + m;
  x0r = a[0] + a[j2];
  x0i = a[1] + a[j2 + 1];
  x1r = a[0] - a[j2];
  x1i = a[1] - a[j2 + 1];
  x2r = a[j1] + a[j3];
  x2i = a[j1 + 1] + a[j3 + 1];
  x3r = a[j1] - a[j3];
  x3i = a[j1 + 1] - a[j3 + 1];
  a[0] = x0r + x2r;
  a[1] = x0i + x2i;
  a[j1] = x0r - x2r;
  a[j1 + 1] = x0i - x2i;
  a[j2] = x1r - x3i;
  a[j2 + 1] = x1i + x3r;
  a[j3] = x1r + x3i;
  a[j3 + 1] = x1i - x3r;
  wd1r = 1;
  wd1i = 0;
  wd3r = 1;
  wd3i = 0;
  ew = M_PI_2 / m;
  w1r = cos (2 * ew);
  w1i = sin (2 * ew);
  wk1r = w1r;
  wk1i = w1i;
  ss1 = 2 * w1i;
  wk3i = 2 * ss1 * wk1r;
  wk3r = wk1r - wk3i * wk1i;
  wk3i = wk1i - wk3i * wk1r;
  ss3 = 2 * wk3i;
  i = 0;
  for (;;)
    {
      i0 = i + 4 * CDFT_LOOP_DIV;
      if (i0 > mh - 4)
	{
	  i0 = mh - 4;
	}
      for (j = i + 2; j < i0; j += 4)
	{
	  wd1r -= ss1 * wk1i;
	  wd1i += ss1 * wk1r;
	  wd3r -= ss3 * wk3i;
	  wd3i += ss3 * wk3r;
	  j1 = j + m;
	  j2 = j1 + m;
	  j3 = j2 + m;
	  x0r = a[j] + a[j2];
	  x0i = a[j + 1] + a[j2 + 1];
	  x1r = a[j] - a[j2];
	  x1i = a[j + 1] - a[j2 + 1];
	  x2r = a[j1] + a[j3];
	  x2i = a[j1 + 1] + a[j3 + 1];
	  x3r = a[j1] - a[j3];
	  x3i = a[j1 + 1] - a[j3 + 1];
	  a[j] = x0r + x2r;
	  a[j + 1] = x0i + x2i;
	  a[j1] = x0r - x2r;
	  a[j1 + 1] = x0i - x2i;
	  x0r = x1r - x3i;
	  x0i = x1i + x3r;
	  a[j2] = wk1r * x0r - wk1i * x0i;
	  a[j2 + 1] = wk1r * x0i + wk1i * x0r;
	  x0r = x1r + x3i;
	  x0i = x1i - x3r;
	  a[j3] = wk3r * x0r + wk3i * x0i;
	  a[j3 + 1] = wk3r * x0i - wk3i * x0r;
	  x0r = a[j + 2] + a[j2 + 2];
	  x0i = a[j + 3] + a[j2 + 3];
	  x1r = a[j + 2] - a[j2 + 2];
	  x1i = a[j + 3] - a[j2 + 3];
	  x2r = a[j1 + 2] + a[j3 + 2];
	  x2i = a[j1 + 3] + a[j3 + 3];
	  x3r = a[j1 + 2] - a[j3 + 2];
	  x3i = a[j1 + 3] - a[j3 + 3];
	  a[j + 2] = x0r + x2r;
	  a[j + 3] = x0i + x2i;
	  a[j1 + 2] = x0r - x2r;
	  a[j1 + 3] = x0i - x2i;
	  x0r = x1r - x3i;
	  x0i = x1i + x3r;
	  a[j2 + 2] = wd1r * x0r - wd1i * x0i;
	  a[j2 + 3] = wd1r * x0i + wd1i * x0r;
	  x0r = x1r + x3i;
	  x0i = x1i - x3r;
	  a[j3 + 2] = wd3r * x0r + wd3i * x0i;
	  a[j3 + 3] = wd3r * x0i - wd3i * x0r;
	  j0 = m - j;
	  j1 = j0 + m;
	  j2 = j1 + m;
	  j3 = j2 + m;
	  x0r = a[j0] + a[j2];
	  x0i = a[j0 + 1] + a[j2 + 1];
	  x1r = a[j0] - a[j2];
	  x1i = a[j0 + 1] - a[j2 + 1];
	  x2r = a[j1] + a[j3];
	  x2i = a[j1 + 1] + a[j3 + 1];
	  x3r = a[j1] - a[j3];
	  x3i = a[j1 + 1] - a[j3 + 1];
	  a[j0] = x0r + x2r;
	  a[j0 + 1] = x0i + x2i;
	  a[j1] = x0r - x2r;
	  a[j1 + 1] = x0i - x2i;
	  x0r = x1r - x3i;
	  x0i = x1i + x3r;
	  a[j2] = wk1i * x0r - wk1r * x0i;
	  a[j2 + 1] = wk1i * x0i + wk1r * x0r;
	  x0r = x1r + x3i;
	  x0i = x1i - x3r;
	  a[j3] = wk3i * x0r + wk3r * x0i;
	  a[j3 + 1] = wk3i * x0i - wk3r * x0r;
	  x0r = a[j0 - 2] + a[j2 - 2];
	  x0i = a[j0 - 1] + a[j2 - 1];
	  x1r = a[j0 - 2] - a[j2 - 2];
	  x1i = a[j0 - 1] - a[j2 - 1];
	  x2r = a[j1 - 2] + a[j3 - 2];
	  x2i = a[j1 - 1] + a[j3 - 1];
	  x3r = a[j1 - 2] - a[j3 - 2];
	  x3i = a[j1 - 1] - a[j3 - 1];
	  a[j0 - 2] = x0r + x2r;
	  a[j0 - 1] = x0i + x2i;
	  a[j1 - 2] = x0r - x2r;
	  a[j1 - 1] = x0i - x2i;
	  x0r = x1r - x3i;
	  x0i = x1i + x3r;
	  a[j2 - 2] = wd1i * x0r - wd1r * x0i;
	  a[j2 - 1] = wd1i * x0i + wd1r * x0r;
	  x0r = x1r + x3i;
	  x0i = x1i - x3r;
	  a[j3 - 2] = wd3i * x0r + wd3r * x0i;
	  a[j3 - 1] = wd3i * x0i - wd3r * x0r;
	  wk1r -= ss1 * wd1i;
	  wk1i += ss1 * wd1r;
	  wk3r -= ss3 * wd3i;
	  wk3i += ss3 * wd3r;
	}
      if (i0 == mh - 4)
	{
	  break;
	}
      wd1r = cos (ew * i0);
      wd1i = sin (ew * i0);
      wd3i = 4 * wd1i * wd1r;
      wd3r = wd1r - wd3i * wd1i;
      wd3i = wd1i - wd3i * wd1r;
      wk1r = w1r * wd1r - w1i * wd1i;
      wk1i = w1r * wd1i + w1i * wd1r;
      wk3i = 4 * wk1i * wk1r;
      wk3r = wk1r - wk3i * wk1i;
      wk3i = wk1i - wk3i * wk1r;
      i = i0;
    }
  wd1r -= ss1 * wk1i;
  j0 = mh;
  j1 = j0 + m;
  j2 = j1 + m;
  j3 = j2 + m;
  x0r = a[j0 - 2] + a[j2 - 2];
  x0i = a[j0 - 1] + a[j2 - 1];
  x1r = a[j0 - 2] - a[j2 - 2];
  x1i = a[j0 - 1] - a[j2 - 1];
  x2r = a[j1 - 2] + a[j3 - 2];
  x2i = a[j1 - 1] + a[j3 - 1];
  x3r = a[j1 - 2] - a[j3 - 2];
  x3i = a[j1 - 1] - a[j3 - 1];
  a[j0 - 2] = x0r + x2r;
  a[j0 - 1] = x0i + x2i;
  a[j1 - 2] = x0r - x2r;
  a[j1 - 1] = x0i - x2i;
  x0r = x1r - x3i;
  x0i = x1i + x3r;
  a[j2 - 2] = wk1r * x0r - wk1i * x0i;
  a[j2 - 1] = wk1r * x0i + wk1i * x0r;
  x0r = x1r + x3i;
  x0i = x1i - x3r;
  a[j3 - 2] = wk3r * x0r + wk3i * x0i;
  a[j3 - 1] = wk3r * x0i - wk3i * x0r;
  x0r = a[j0] + a[j2];
  x0i = a[j0 + 1] + a[j2 + 1];
  x1r = a[j0] - a[j2];
  x1i = a[j0 + 1] - a[j2 + 1];
  x2r = a[j1] + a[j3];
  x2i = a[j1 + 1] + a[j3 + 1];
  x3r = a[j1] - a[j3];
  x3i = a[j1 + 1] - a[j3 + 1];
  a[j0] = x0r + x2r;
  a[j0 + 1] = x0i + x2i;
  a[j1] = x0r - x2r;
  a[j1 + 1] = x0i - x2i;
  x0r = x1r - x3i;
  x0i = x1i + x3r;
  a[j2] = wd1r * (x0r - x0i);
  a[j2 + 1] = wd1r * (x0i + x0r);
  x0r = x1r + x3i;
  x0i = x1i - x3r;
  a[j3] = -wd1r * (x0r + x0i);
  a[j3 + 1] = -wd1r * (x0i - x0r);
  x0r = a[j0 + 2] + a[j2 + 2];
  x0i = a[j0 + 3] + a[j2 + 3];
  x1r = a[j0 + 2] - a[j2 + 2];
  x1i = a[j0 + 3] - a[j2 + 3];
  x2r = a[j1 + 2] + a[j3 + 2];
  x2i = a[j1 + 3] + a[j3 + 3];
  x3r = a[j1 + 2] - a[j3 + 2];
  x3i = a[j1 + 3] - a[j3 + 3];
  a[j0 + 2] = x0r + x2r;
  a[j0 + 3] = x0i + x2i;
  a[j1 + 2] = x0r - x2r;
  a[j1 + 3] = x0i - x2i;
  x0r = x1r - x3i;
  x0i = x1i + x3r;
  a[j2 + 2] = wk1i * x0r - wk1r * x0i;
  a[j2 + 3] = wk1i * x0i + wk1r * x0r;
  x0r = x1r + x3i;
  x0i = x1i - x3r;
  a[j3 + 2] = wk3i * x0r + wk3r * x0i;
  a[j3 + 3] = wk3i * x0i - wk3r * x0r;
}


void
cftmdl2 (int n, double *a)
{
  int i, i0, j, j0, j1, j2, j3, m, mh;
  double ew, w1r, w1i, wn4r, wk1r, wk1i, wk3r, wk3i,
    wl1r, wl1i, wl3r, wl3i, wd1r, wd1i, wd3r, wd3i,
    we1r, we1i, we3r, we3i, ss1, ss3;
  double x0r, x0i, x1r, x1i, x2r, x2i, x3r, x3i, y0r, y0i, y2r, y2i;

  mh = n >> 3;
  m = 2 * mh;
  wn4r = WR5000;
  j1 = m;
  j2 = j1 + m;
  j3 = j2 + m;
  x0r = a[0] - a[j2 + 1];
  x0i = a[1] + a[j2];
  x1r = a[0] + a[j2 + 1];
  x1i = a[1] - a[j2];
  x2r = a[j1] - a[j3 + 1];
  x2i = a[j1 + 1] + a[j3];
  x3r = a[j1] + a[j3 + 1];
  x3i = a[j1 + 1] - a[j3];
  y0r = wn4r * (x2r - x2i);
  y0i = wn4r * (x2i + x2r);
  a[0] = x0r + y0r;
  a[1] = x0i + y0i;
  a[j1] = x0r - y0r;
  a[j1 + 1] = x0i - y0i;
  y0r = wn4r * (x3r - x3i);
  y0i = wn4r * (x3i + x3r);
  a[j2] = x1r - y0i;
  a[j2 + 1] = x1i + y0r;
  a[j3] = x1r + y0i;
  a[j3 + 1] = x1i - y0r;
  wl1r = 1;
  wl1i = 0;
  wl3r = 1;
  wl3i = 0;
  we1r = wn4r;
  we1i = wn4r;
  we3r = -wn4r;
  we3i = -wn4r;
  ew = M_PI_2 / (2 * m);
  w1r = cos (2 * ew);
  w1i = sin (2 * ew);
  wk1r = w1r;
  wk1i = w1i;
  wd1r = wn4r * (w1r - w1i);
  wd1i = wn4r * (w1i + w1r);
  ss1 = 2 * w1i;
  wk3i = 2 * ss1 * wk1r;
  wk3r = wk1r - wk3i * wk1i;
  wk3i = wk1i - wk3i * wk1r;
  ss3 = 2 * wk3i;
  wd3r = -wn4r * (wk3r - wk3i);
  wd3i = -wn4r * (wk3i + wk3r);
  i = 0;
  for (;;)
    {
      i0 = i + 4 * CDFT_LOOP_DIV;
      if (i0 > mh - 4)
	{
	  i0 = mh - 4;
	}
      for (j = i + 2; j < i0; j += 4)
	{
	  wl1r -= ss1 * wk1i;
	  wl1i += ss1 * wk1r;
	  wl3r -= ss3 * wk3i;
	  wl3i += ss3 * wk3r;
	  we1r -= ss1 * wd1i;
	  we1i += ss1 * wd1r;
	  we3r -= ss3 * wd3i;
	  we3i += ss3 * wd3r;
	  j1 = j + m;
	  j2 = j1 + m;
	  j3 = j2 + m;
	  x0r = a[j] - a[j2 + 1];
	  x0i = a[j + 1] + a[j2];
	  x1r = a[j] + a[j2 + 1];
	  x1i = a[j + 1] - a[j2];
	  x2r = a[j1] - a[j3 + 1];
	  x2i = a[j1 + 1] + a[j3];
	  x3r = a[j1] + a[j3 + 1];
	  x3i = a[j1 + 1] - a[j3];
	  y0r = wk1r * x0r - wk1i * x0i;
	  y0i = wk1r * x0i + wk1i * x0r;
	  y2r = wd1r * x2r - wd1i * x2i;
	  y2i = wd1r * x2i + wd1i * x2r;
	  a[j] = y0r + y2r;
	  a[j + 1] = y0i + y2i;
	  a[j1] = y0r - y2r;
	  a[j1 + 1] = y0i - y2i;
	  y0r = wk3r * x1r + wk3i * x1i;
	  y0i = wk3r * x1i - wk3i * x1r;
	  y2r = wd3r * x3r + wd3i * x3i;
	  y2i = wd3r * x3i - wd3i * x3r;
	  a[j2] = y0r + y2r;
	  a[j2 + 1] = y0i + y2i;
	  a[j3] = y0r - y2r;
	  a[j3 + 1] = y0i - y2i;
	  x0r = a[j + 2] - a[j2 + 3];
	  x0i = a[j + 3] + a[j2 + 2];
	  x1r = a[j + 2] + a[j2 + 3];
	  x1i = a[j + 3] - a[j2 + 2];
	  x2r = a[j1 + 2] - a[j3 + 3];
	  x2i = a[j1 + 3] + a[j3 + 2];
	  x3r = a[j1 + 2] + a[j3 + 3];
	  x3i = a[j1 + 3] - a[j3 + 2];
	  y0r = wl1r * x0r - wl1i * x0i;
	  y0i = wl1r * x0i + wl1i * x0r;
	  y2r = we1r * x2r - we1i * x2i;
	  y2i = we1r * x2i + we1i * x2r;
	  a[j + 2] = y0r + y2r;
	  a[j + 3] = y0i + y2i;
	  a[j1 + 2] = y0r - y2r;
	  a[j1 + 3] = y0i - y2i;
	  y0r = wl3r * x1r + wl3i * x1i;
	  y0i = wl3r * x1i - wl3i * x1r;
	  y2r = we3r * x3r + we3i * x3i;
	  y2i = we3r * x3i - we3i * x3r;
	  a[j2 + 2] = y0r + y2r;
	  a[j2 + 3] = y0i + y2i;
	  a[j3 + 2] = y0r - y2r;
	  a[j3 + 3] = y0i - y2i;
	  j0 = m - j;
	  j1 = j0 + m;
	  j2 = j1 + m;
	  j3 = j2 + m;
	  x0r = a[j0] - a[j2 + 1];
	  x0i = a[j0 + 1] + a[j2];
	  x1r = a[j0] + a[j2 + 1];
	  x1i = a[j0 + 1] - a[j2];
	  x2r = a[j1] - a[j3 + 1];
	  x2i = a[j1 + 1] + a[j3];
	  x3r = a[j1] + a[j3 + 1];
	  x3i = a[j1 + 1] - a[j3];
	  y0r = wd1i * x0r - wd1r * x0i;
	  y0i = wd1i * x0i + wd1r * x0r;
	  y2r = wk1i * x2r - wk1r * x2i;
	  y2i = wk1i * x2i + wk1r * x2r;
	  a[j0] = y0r + y2r;
	  a[j0 + 1] = y0i + y2i;
	  a[j1] = y0r - y2r;
	  a[j1 + 1] = y0i - y2i;
	  y0r = wd3i * x1r + wd3r * x1i;
	  y0i = wd3i * x1i - wd3r * x1r;
	  y2r = wk3i * x3r + wk3r * x3i;
	  y2i = wk3i * x3i - wk3r * x3r;
	  a[j2] = y0r + y2r;
	  a[j2 + 1] = y0i + y2i;
	  a[j3] = y0r - y2r;
	  a[j3 + 1] = y0i - y2i;
	  x0r = a[j0 - 2] - a[j2 - 1];
	  x0i = a[j0 - 1] + a[j2 - 2];
	  x1r = a[j0 - 2] + a[j2 - 1];
	  x1i = a[j0 - 1] - a[j2 - 2];
	  x2r = a[j1 - 2] - a[j3 - 1];
	  x2i = a[j1 - 1] + a[j3 - 2];
	  x3r = a[j1 - 2] + a[j3 - 1];
	  x3i = a[j1 - 1] - a[j3 - 2];
	  y0r = we1i * x0r - we1r * x0i;
	  y0i = we1i * x0i + we1r * x0r;
	  y2r = wl1i * x2r - wl1r * x2i;
	  y2i = wl1i * x2i + wl1r * x2r;
	  a[j0 - 2] = y0r + y2r;
	  a[j0 - 1] = y0i + y2i;
	  a[j1 - 2] = y0r - y2r;
	  a[j1 - 1] = y0i - y2i;
	  y0r = we3i * x1r + we3r * x1i;
	  y0i = we3i * x1i - we3r * x1r;
	  y2r = wl3i * x3r + wl3r * x3i;
	  y2i = wl3i * x3i - wl3r * x3r;
	  a[j2 - 2] = y0r + y2r;
	  a[j2 - 1] = y0i + y2i;
	  a[j3 - 2] = y0r - y2r;
	  a[j3 - 1] = y0i - y2i;
	  wk1r -= ss1 * wl1i;
	  wk1i += ss1 * wl1r;
	  wk3r -= ss3 * wl3i;
	  wk3i += ss3 * wl3r;
	  wd1r -= ss1 * we1i;
	  wd1i += ss1 * we1r;
	  wd3r -= ss3 * we3i;
	  wd3i += ss3 * we3r;
	}
      if (i0 == mh - 4)
	{
	  break;
	}
      wl1r = cos (ew * i0);
      wl1i = sin (ew * i0);
      wl3i = 4 * wl1i * wl1r;
      wl3r = wl1r - wl3i * wl1i;
      wl3i = wl1i - wl3i * wl1r;
      we1r = wn4r * (wl1r - wl1i);
      we1i = wn4r * (wl1i + wl1r);
      we3r = -wn4r * (wl3r - wl3i);
      we3i = -wn4r * (wl3i + wl3r);
      wk1r = w1r * wl1r - w1i * wl1i;
      wk1i = w1r * wl1i + w1i * wl1r;
      wk3i = 4 * wk1i * wk1r;
      wk3r = wk1r - wk3i * wk1i;
      wk3i = wk1i - wk3i * wk1r;
      wd1r = wn4r * (wk1r - wk1i);
      wd1i = wn4r * (wk1i + wk1r);
      wd3r = -wn4r * (wk3r - wk3i);
      wd3i = -wn4r * (wk3i + wk3r);
      i = i0;
    }
  wl1r -= ss1 * wk1i;
  wl1i += ss1 * wk1r;
  j0 = mh;
  j1 = j0 + m;
  j2 = j1 + m;
  j3 = j2 + m;
  x0r = a[j0 - 2] - a[j2 - 1];
  x0i = a[j0 - 1] + a[j2 - 2];
  x1r = a[j0 - 2] + a[j2 - 1];
  x1i = a[j0 - 1] - a[j2 - 2];
  x2r = a[j1 - 2] - a[j3 - 1];
  x2i = a[j1 - 1] + a[j3 - 2];
  x3r = a[j1 - 2] + a[j3 - 1];
  x3i = a[j1 - 1] - a[j3 - 2];
  y0r = wk1r * x0r - wk1i * x0i;
  y0i = wk1r * x0i + wk1i * x0r;
  y2r = wd1r * x2r - wd1i * x2i;
  y2i = wd1r * x2i + wd1i * x2r;
  a[j0 - 2] = y0r + y2r;
  a[j0 - 1] = y0i + y2i;
  a[j1 - 2] = y0r - y2r;
  a[j1 - 1] = y0i - y2i;
  y0r = wk3r * x1r + wk3i * x1i;
  y0i = wk3r * x1i - wk3i * x1r;
  y2r = wd3r * x3r + wd3i * x3i;
  y2i = wd3r * x3i - wd3i * x3r;
  a[j2 - 2] = y0r + y2r;
  a[j2 - 1] = y0i + y2i;
  a[j3 - 2] = y0r - y2r;
  a[j3 - 1] = y0i - y2i;
  x0r = a[j0] - a[j2 + 1];
  x0i = a[j0 + 1] + a[j2];
  x1r = a[j0] + a[j2 + 1];
  x1i = a[j0 + 1] - a[j2];
  x2r = a[j1] - a[j3 + 1];
  x2i = a[j1 + 1] + a[j3];
  x3r = a[j1] + a[j3 + 1];
  x3i = a[j1 + 1] - a[j3];
  y0r = wl1r * x0r - wl1i * x0i;
  y0i = wl1r * x0i + wl1i * x0r;
  y2r = wl1i * x2r - wl1r * x2i;
  y2i = wl1i * x2i + wl1r * x2r;
  a[j0] = y0r + y2r;
  a[j0 + 1] = y0i + y2i;
  a[j1] = y0r - y2r;
  a[j1 + 1] = y0i - y2i;
  y0r = wl1i * x1r - wl1r * x1i;
  y0i = wl1i * x1i + wl1r * x1r;
  y2r = wl1r * x3r - wl1i * x3i;
  y2i = wl1r * x3i + wl1i * x3r;
  a[j2] = y0r - y2r;
  a[j2 + 1] = y0i - y2i;
  a[j3] = y0r + y2r;
  a[j3 + 1] = y0i + y2i;
  x0r = a[j0 + 2] - a[j2 + 3];
  x0i = a[j0 + 3] + a[j2 + 2];
  x1r = a[j0 + 2] + a[j2 + 3];
  x1i = a[j0 + 3] - a[j2 + 2];
  x2r = a[j1 + 2] - a[j3 + 3];
  x2i = a[j1 + 3] + a[j3 + 2];
  x3r = a[j1 + 2] + a[j3 + 3];
  x3i = a[j1 + 3] - a[j3 + 2];
  y0r = wd1i * x0r - wd1r * x0i;
  y0i = wd1i * x0i + wd1r * x0r;
  y2r = wk1i * x2r - wk1r * x2i;
  y2i = wk1i * x2i + wk1r * x2r;
  a[j0 + 2] = y0r + y2r;
  a[j0 + 3] = y0i + y2i;
  a[j1 + 2] = y0r - y2r;
  a[j1 + 3] = y0i - y2i;
  y0r = wd3i * x1r + wd3r * x1i;
  y0i = wd3i * x1i - wd3r * x1r;
  y2r = wk3i * x3r + wk3r * x3i;
  y2i = wk3i * x3i - wk3r * x3r;
  a[j2 + 2] = y0r + y2r;
  a[j2 + 3] = y0i + y2i;
  a[j3 + 2] = y0r - y2r;
  a[j3 + 3] = y0i - y2i;
}


void
cftfx41 (int n, double *a)
{
  void cftf161 (double *a);
  void cftf162 (double *a);
  void cftf081 (double *a);
  void cftf082 (double *a);

  if (n == 128)
    {
      cftf161 (a);
      cftf162 (&a[32]);
      cftf161 (&a[64]);
      cftf161 (&a[96]);
    }
  else
    {
      cftf081 (a);
      cftf082 (&a[16]);
      cftf081 (&a[32]);
      cftf081 (&a[48]);
    }
}


void
cftfx42 (int n, double *a)
{
  void cftf161 (double *a);
  void cftf162 (double *a);
  void cftf081 (double *a);
  void cftf082 (double *a);

  if (n == 128)
    {
      cftf161 (a);
      cftf162 (&a[32]);
      cftf161 (&a[64]);
      cftf162 (&a[96]);
    }
  else
    {
      cftf081 (a);
      cftf082 (&a[16]);
      cftf081 (&a[32]);
      cftf082 (&a[48]);
    }
}


void
cftf161 (double *a)
{
  double wn4r, wk1r, wk1i,
    x0r, x0i, x1r, x1i, x2r, x2i, x3r, x3i,
    y0r, y0i, y1r, y1i, y2r, y2i, y3r, y3i,
    y4r, y4i, y5r, y5i, y6r, y6i, y7r, y7i,
    y8r, y8i, y9r, y9i, y10r, y10i, y11r, y11i,
    y12r, y12i, y13r, y13i, y14r, y14i, y15r, y15i;

  wn4r = WR5000;
  wk1r = WR2500;
  wk1i = WI2500;
  x0r = a[0] + a[16];
  x0i = a[1] + a[17];
  x1r = a[0] - a[16];
  x1i = a[1] - a[17];
  x2r = a[8] + a[24];
  x2i = a[9] + a[25];
  x3r = a[8] - a[24];
  x3i = a[9] - a[25];
  y0r = x0r + x2r;
  y0i = x0i + x2i;
  y4r = x0r - x2r;
  y4i = x0i - x2i;
  y8r = x1r - x3i;
  y8i = x1i + x3r;
  y12r = x1r + x3i;
  y12i = x1i - x3r;
  x0r = a[2] + a[18];
  x0i = a[3] + a[19];
  x1r = a[2] - a[18];
  x1i = a[3] - a[19];
  x2r = a[10] + a[26];
  x2i = a[11] + a[27];
  x3r = a[10] - a[26];
  x3i = a[11] - a[27];
  y1r = x0r + x2r;
  y1i = x0i + x2i;
  y5r = x0r - x2r;
  y5i = x0i - x2i;
  x0r = x1r - x3i;
  x0i = x1i + x3r;
  y9r = wk1r * x0r - wk1i * x0i;
  y9i = wk1r * x0i + wk1i * x0r;
  x0r = x1r + x3i;
  x0i = x1i - x3r;
  y13r = wk1i * x0r - wk1r * x0i;
  y13i = wk1i * x0i + wk1r * x0r;
  x0r = a[4] + a[20];
  x0i = a[5] + a[21];
  x1r = a[4] - a[20];
  x1i = a[5] - a[21];
  x2r = a[12] + a[28];
  x2i = a[13] + a[29];
  x3r = a[12] - a[28];
  x3i = a[13] - a[29];
  y2r = x0r + x2r;
  y2i = x0i + x2i;
  y6r = x0r - x2r;
  y6i = x0i - x2i;
  x0r = x1r - x3i;
  x0i = x1i + x3r;
  y10r = wn4r * (x0r - x0i);
  y10i = wn4r * (x0i + x0r);
  x0r = x1r + x3i;
  x0i = x1i - x3r;
  y14r = wn4r * (x0r + x0i);
  y14i = wn4r * (x0i - x0r);
  x0r = a[6] + a[22];
  x0i = a[7] + a[23];
  x1r = a[6] - a[22];
  x1i = a[7] - a[23];
  x2r = a[14] + a[30];
  x2i = a[15] + a[31];
  x3r = a[14] - a[30];
  x3i = a[15] - a[31];
  y3r = x0r + x2r;
  y3i = x0i + x2i;
  y7r = x0r - x2r;
  y7i = x0i - x2i;
  x0r = x1r - x3i;
  x0i = x1i + x3r;
  y11r = wk1i * x0r - wk1r * x0i;
  y11i = wk1i * x0i + wk1r * x0r;
  x0r = x1r + x3i;
  x0i = x1i - x3r;
  y15r = wk1r * x0r - wk1i * x0i;
  y15i = wk1r * x0i + wk1i * x0r;
  x0r = y12r - y14r;
  x0i = y12i - y14i;
  x1r = y12r + y14r;
  x1i = y12i + y14i;
  x2r = y13r - y15r;
  x2i = y13i - y15i;
  x3r = y13r + y15r;
  x3i = y13i + y15i;
  a[24] = x0r + x2r;
  a[25] = x0i + x2i;
  a[26] = x0r - x2r;
  a[27] = x0i - x2i;
  a[28] = x1r - x3i;
  a[29] = x1i + x3r;
  a[30] = x1r + x3i;
  a[31] = x1i - x3r;
  x0r = y8r + y10r;
  x0i = y8i + y10i;
  x1r = y8r - y10r;
  x1i = y8i - y10i;
  x2r = y9r + y11r;
  x2i = y9i + y11i;
  x3r = y9r - y11r;
  x3i = y9i - y11i;
  a[16] = x0r + x2r;
  a[17] = x0i + x2i;
  a[18] = x0r - x2r;
  a[19] = x0i - x2i;
  a[20] = x1r - x3i;
  a[21] = x1i + x3r;
  a[22] = x1r + x3i;
  a[23] = x1i - x3r;
  x0r = y5r - y7i;
  x0i = y5i + y7r;
  x2r = wn4r * (x0r - x0i);
  x2i = wn4r * (x0i + x0r);
  x0r = y5r + y7i;
  x0i = y5i - y7r;
  x3r = wn4r * (x0r - x0i);
  x3i = wn4r * (x0i + x0r);
  x0r = y4r - y6i;
  x0i = y4i + y6r;
  x1r = y4r + y6i;
  x1i = y4i - y6r;
  a[8] = x0r + x2r;
  a[9] = x0i + x2i;
  a[10] = x0r - x2r;
  a[11] = x0i - x2i;
  a[12] = x1r - x3i;
  a[13] = x1i + x3r;
  a[14] = x1r + x3i;
  a[15] = x1i - x3r;
  x0r = y0r + y2r;
  x0i = y0i + y2i;
  x1r = y0r - y2r;
  x1i = y0i - y2i;
  x2r = y1r + y3r;
  x2i = y1i + y3i;
  x3r = y1r - y3r;
  x3i = y1i - y3i;
  a[0] = x0r + x2r;
  a[1] = x0i + x2i;
  a[2] = x0r - x2r;
  a[3] = x0i - x2i;
  a[4] = x1r - x3i;
  a[5] = x1i + x3r;
  a[6] = x1r + x3i;
  a[7] = x1i - x3r;
}


void
cftf162 (double *a)
{
  double wn4r, wk1r, wk1i, wk2r, wk2i, wk3r, wk3i,
    x0r, x0i, x1r, x1i, x2r, x2i,
    y0r, y0i, y1r, y1i, y2r, y2i, y3r, y3i,
    y4r, y4i, y5r, y5i, y6r, y6i, y7r, y7i,
    y8r, y8i, y9r, y9i, y10r, y10i, y11r, y11i,
    y12r, y12i, y13r, y13i, y14r, y14i, y15r, y15i;

  wn4r = WR5000;
  wk1r = WR1250;
  wk1i = WI1250;
  wk2r = WR2500;
  wk2i = WI2500;
  wk3r = WR3750;
  wk3i = WI3750;
  x1r = a[0] - a[17];
  x1i = a[1] + a[16];
  x0r = a[8] - a[25];
  x0i = a[9] + a[24];
  x2r = wn4r * (x0r - x0i);
  x2i = wn4r * (x0i + x0r);
  y0r = x1r + x2r;
  y0i = x1i + x2i;
  y4r = x1r - x2r;
  y4i = x1i - x2i;
  x1r = a[0] + a[17];
  x1i = a[1] - a[16];
  x0r = a[8] + a[25];
  x0i = a[9] - a[24];
  x2r = wn4r * (x0r - x0i);
  x2i = wn4r * (x0i + x0r);
  y8r = x1r - x2i;
  y8i = x1i + x2r;
  y12r = x1r + x2i;
  y12i = x1i - x2r;
  x0r = a[2] - a[19];
  x0i = a[3] + a[18];
  x1r = wk1r * x0r - wk1i * x0i;
  x1i = wk1r * x0i + wk1i * x0r;
  x0r = a[10] - a[27];
  x0i = a[11] + a[26];
  x2r = wk3i * x0r - wk3r * x0i;
  x2i = wk3i * x0i + wk3r * x0r;
  y1r = x1r + x2r;
  y1i = x1i + x2i;
  y5r = x1r - x2r;
  y5i = x1i - x2i;
  x0r = a[2] + a[19];
  x0i = a[3] - a[18];
  x1r = wk3r * x0r - wk3i * x0i;
  x1i = wk3r * x0i + wk3i * x0r;
  x0r = a[10] + a[27];
  x0i = a[11] - a[26];
  x2r = wk1r * x0r + wk1i * x0i;
  x2i = wk1r * x0i - wk1i * x0r;
  y9r = x1r - x2r;
  y9i = x1i - x2i;
  y13r = x1r + x2r;
  y13i = x1i + x2i;
  x0r = a[4] - a[21];
  x0i = a[5] + a[20];
  x1r = wk2r * x0r - wk2i * x0i;
  x1i = wk2r * x0i + wk2i * x0r;
  x0r = a[12] - a[29];
  x0i = a[13] + a[28];
  x2r = wk2i * x0r - wk2r * x0i;
  x2i = wk2i * x0i + wk2r * x0r;
  y2r = x1r + x2r;
  y2i = x1i + x2i;
  y6r = x1r - x2r;
  y6i = x1i - x2i;
  x0r = a[4] + a[21];
  x0i = a[5] - a[20];
  x1r = wk2i * x0r - wk2r * x0i;
  x1i = wk2i * x0i + wk2r * x0r;
  x0r = a[12] + a[29];
  x0i = a[13] - a[28];
  x2r = wk2r * x0r - wk2i * x0i;
  x2i = wk2r * x0i + wk2i * x0r;
  y10r = x1r - x2r;
  y10i = x1i - x2i;
  y14r = x1r + x2r;
  y14i = x1i + x2i;
  x0r = a[6] - a[23];
  x0i = a[7] + a[22];
  x1r = wk3r * x0r - wk3i * x0i;
  x1i = wk3r * x0i + wk3i * x0r;
  x0r = a[14] - a[31];
  x0i = a[15] + a[30];
  x2r = wk1i * x0r - wk1r * x0i;
  x2i = wk1i * x0i + wk1r * x0r;
  y3r = x1r + x2r;
  y3i = x1i + x2i;
  y7r = x1r - x2r;
  y7i = x1i - x2i;
  x0r = a[6] + a[23];
  x0i = a[7] - a[22];
  x1r = wk1i * x0r + wk1r * x0i;
  x1i = wk1i * x0i - wk1r * x0r;
  x0r = a[14] + a[31];
  x0i = a[15] - a[30];
  x2r = wk3i * x0r - wk3r * x0i;
  x2i = wk3i * x0i + wk3r * x0r;
  y11r = x1r + x2r;
  y11i = x1i + x2i;
  y15r = x1r - x2r;
  y15i = x1i - x2i;
  x1r = y0r + y2r;
  x1i = y0i + y2i;
  x2r = y1r + y3r;
  x2i = y1i + y3i;
  a[0] = x1r + x2r;
  a[1] = x1i + x2i;
  a[2] = x1r - x2r;
  a[3] = x1i - x2i;
  x1r = y0r - y2r;
  x1i = y0i - y2i;
  x2r = y1r - y3r;
  x2i = y1i - y3i;
  a[4] = x1r - x2i;
  a[5] = x1i + x2r;
  a[6] = x1r + x2i;
  a[7] = x1i - x2r;
  x1r = y4r - y6i;
  x1i = y4i + y6r;
  x0r = y5r - y7i;
  x0i = y5i + y7r;
  x2r = wn4r * (x0r - x0i);
  x2i = wn4r * (x0i + x0r);
  a[8] = x1r + x2r;
  a[9] = x1i + x2i;
  a[10] = x1r - x2r;
  a[11] = x1i - x2i;
  x1r = y4r + y6i;
  x1i = y4i - y6r;
  x0r = y5r + y7i;
  x0i = y5i - y7r;
  x2r = wn4r * (x0r - x0i);
  x2i = wn4r * (x0i + x0r);
  a[12] = x1r - x2i;
  a[13] = x1i + x2r;
  a[14] = x1r + x2i;
  a[15] = x1i - x2r;
  x1r = y8r + y10r;
  x1i = y8i + y10i;
  x2r = y9r - y11r;
  x2i = y9i - y11i;
  a[16] = x1r + x2r;
  a[17] = x1i + x2i;
  a[18] = x1r - x2r;
  a[19] = x1i - x2i;
  x1r = y8r - y10r;
  x1i = y8i - y10i;
  x2r = y9r + y11r;
  x2i = y9i + y11i;
  a[20] = x1r - x2i;
  a[21] = x1i + x2r;
  a[22] = x1r + x2i;
  a[23] = x1i - x2r;
  x1r = y12r - y14i;
  x1i = y12i + y14r;
  x0r = y13r + y15i;
  x0i = y13i - y15r;
  x2r = wn4r * (x0r - x0i);
  x2i = wn4r * (x0i + x0r);
  a[24] = x1r + x2r;
  a[25] = x1i + x2i;
  a[26] = x1r - x2r;
  a[27] = x1i - x2i;
  x1r = y12r + y14i;
  x1i = y12i - y14r;
  x0r = y13r - y15i;
  x0i = y13i + y15r;
  x2r = wn4r * (x0r - x0i);
  x2i = wn4r * (x0i + x0r);
  a[28] = x1r - x2i;
  a[29] = x1i + x2r;
  a[30] = x1r + x2i;
  a[31] = x1i - x2r;
}


void
cftf081 (double *a)
{
  double wn4r, x0r, x0i, x1r, x1i, x2r, x2i, x3r, x3i,
    y0r, y0i, y1r, y1i, y2r, y2i, y3r, y3i,
    y4r, y4i, y5r, y5i, y6r, y6i, y7r, y7i;

  wn4r = WR5000;
  x0r = a[0] + a[8];
  x0i = a[1] + a[9];
  x1r = a[0] - a[8];
  x1i = a[1] - a[9];
  x2r = a[4] + a[12];
  x2i = a[5] + a[13];
  x3r = a[4] - a[12];
  x3i = a[5] - a[13];
  y0r = x0r + x2r;
  y0i = x0i + x2i;
  y2r = x0r - x2r;
  y2i = x0i - x2i;
  y1r = x1r - x3i;
  y1i = x1i + x3r;
  y3r = x1r + x3i;
  y3i = x1i - x3r;
  x0r = a[2] + a[10];
  x0i = a[3] + a[11];
  x1r = a[2] - a[10];
  x1i = a[3] - a[11];
  x2r = a[6] + a[14];
  x2i = a[7] + a[15];
  x3r = a[6] - a[14];
  x3i = a[7] - a[15];
  y4r = x0r + x2r;
  y4i = x0i + x2i;
  y6r = x0r - x2r;
  y6i = x0i - x2i;
  x0r = x1r - x3i;
  x0i = x1i + x3r;
  x2r = x1r + x3i;
  x2i = x1i - x3r;
  y5r = wn4r * (x0r - x0i);
  y5i = wn4r * (x0r + x0i);
  y7r = wn4r * (x2r - x2i);
  y7i = wn4r * (x2r + x2i);
  a[8] = y1r + y5r;
  a[9] = y1i + y5i;
  a[10] = y1r - y5r;
  a[11] = y1i - y5i;
  a[12] = y3r - y7i;
  a[13] = y3i + y7r;
  a[14] = y3r + y7i;
  a[15] = y3i - y7r;
  a[0] = y0r + y4r;
  a[1] = y0i + y4i;
  a[2] = y0r - y4r;
  a[3] = y0i - y4i;
  a[4] = y2r - y6i;
  a[5] = y2i + y6r;
  a[6] = y2r + y6i;
  a[7] = y2i - y6r;
}


void
cftf082 (double *a)
{
  double wn4r, wk1r, wk1i, x0r, x0i, x1r, x1i,
    y0r, y0i, y1r, y1i, y2r, y2i, y3r, y3i,
    y4r, y4i, y5r, y5i, y6r, y6i, y7r, y7i;

  wn4r = WR5000;
  wk1r = WR2500;
  wk1i = WI2500;
  y0r = a[0] - a[9];
  y0i = a[1] + a[8];
  y1r = a[0] + a[9];
  y1i = a[1] - a[8];
  x0r = a[4] - a[13];
  x0i = a[5] + a[12];
  y2r = wn4r * (x0r - x0i);
  y2i = wn4r * (x0i + x0r);
  x0r = a[4] + a[13];
  x0i = a[5] - a[12];
  y3r = wn4r * (x0r - x0i);
  y3i = wn4r * (x0i + x0r);
  x0r = a[2] - a[11];
  x0i = a[3] + a[10];
  y4r = wk1r * x0r - wk1i * x0i;
  y4i = wk1r * x0i + wk1i * x0r;
  x0r = a[2] + a[11];
  x0i = a[3] - a[10];
  y5r = wk1i * x0r - wk1r * x0i;
  y5i = wk1i * x0i + wk1r * x0r;
  x0r = a[6] - a[15];
  x0i = a[7] + a[14];
  y6r = wk1i * x0r - wk1r * x0i;
  y6i = wk1i * x0i + wk1r * x0r;
  x0r = a[6] + a[15];
  x0i = a[7] - a[14];
  y7r = wk1r * x0r - wk1i * x0i;
  y7i = wk1r * x0i + wk1i * x0r;
  x0r = y0r + y2r;
  x0i = y0i + y2i;
  x1r = y4r + y6r;
  x1i = y4i + y6i;
  a[0] = x0r + x1r;
  a[1] = x0i + x1i;
  a[2] = x0r - x1r;
  a[3] = x0i - x1i;
  x0r = y0r - y2r;
  x0i = y0i - y2i;
  x1r = y4r - y6r;
  x1i = y4i - y6i;
  a[4] = x0r - x1i;
  a[5] = x0i + x1r;
  a[6] = x0r + x1i;
  a[7] = x0i - x1r;
  x0r = y1r - y3i;
  x0i = y1i + y3r;
  x1r = y5r - y7r;
  x1i = y5i - y7i;
  a[8] = x0r + x1r;
  a[9] = x0i + x1i;
  a[10] = x0r - x1r;
  a[11] = x0i - x1i;
  x0r = y1r + y3i;
  x0i = y1i - y3r;
  x1r = y5r + y7r;
  x1i = y5i + y7i;
  a[12] = x0r - x1i;
  a[13] = x0i + x1r;
  a[14] = x0r + x1i;
  a[15] = x0i - x1r;
}


void
cftf040 (double *a)
{
  double x0r, x0i, x1r, x1i, x2r, x2i, x3r, x3i;

  x0r = a[0] + a[4];
  x0i = a[1] + a[5];
  x1r = a[0] - a[4];
  x1i = a[1] - a[5];
  x2r = a[2] + a[6];
  x2i = a[3] + a[7];
  x3r = a[2] - a[6];
  x3i = a[3] - a[7];
  a[0] = x0r + x2r;
  a[1] = x0i + x2i;
  a[4] = x0r - x2r;
  a[5] = x0i - x2i;
  a[2] = x1r - x3i;
  a[3] = x1i + x3r;
  a[6] = x1r + x3i;
  a[7] = x1i - x3r;
}


void
cftb040 (double *a)
{
  double x0r, x0i, x1r, x1i, x2r, x2i, x3r, x3i;

  x0r = a[0] + a[4];
  x0i = a[1] + a[5];
  x1r = a[0] - a[4];
  x1i = a[1] - a[5];
  x2r = a[2] + a[6];
  x2i = a[3] + a[7];
  x3r = a[2] - a[6];
  x3i = a[3] - a[7];
  a[0] = x0r + x2r;
  a[1] = x0i + x2i;
  a[4] = x0r - x2r;
  a[5] = x0i - x2i;
  a[2] = x1r + x3i;
  a[3] = x1i - x3r;
  a[6] = x1r - x3i;
  a[7] = x1i + x3r;
}


void
cftx020 (double *a)
{
  double x0r, x0i;

  x0r = a[0] - a[2];
  x0i = a[1] - a[3];
  a[0] += a[2];
  a[1] += a[3];
  a[2] = x0r;
  a[3] = x0i;
}


void
rftfsub (int n, double *a)
{
  int i, i0, j, k;
  double ec, w1r, w1i, wkr, wki, wdr, wdi, ss, xr, xi, yr, yi;

  ec = 2 * M_PI_2 / n;
  wkr = 0;
  wki = 0;
  wdi = cos (ec);
  wdr = sin (ec);
  wdi *= wdr;
  wdr *= wdr;
  w1r = 1 - 2 * wdr;
  w1i = 2 * wdi;
  ss = 2 * w1i;
  i = n >> 1;
  for (;;)
    {
      i0 = i - 4 * RDFT_LOOP_DIV;
      if (i0 < 4)
	{
	  i0 = 4;
	}
      for (j = i - 4; j >= i0; j -= 4)
	{
	  k = n - j;
	  xr = a[j + 2] - a[k - 2];
	  xi = a[j + 3] + a[k - 1];
	  yr = wdr * xr - wdi * xi;
	  yi = wdr * xi + wdi * xr;
	  a[j + 2] -= yr;
	  a[j + 3] -= yi;
	  a[k - 2] += yr;
	  a[k - 1] -= yi;
	  wkr += ss * wdi;
	  wki += ss * (0.5 - wdr);
	  xr = a[j] - a[k];
	  xi = a[j + 1] + a[k + 1];
	  yr = wkr * xr - wki * xi;
	  yi = wkr * xi + wki * xr;
	  a[j] -= yr;
	  a[j + 1] -= yi;
	  a[k] += yr;
	  a[k + 1] -= yi;
	  wdr += ss * wki;
	  wdi += ss * (0.5 - wkr);
	}
      if (i0 == 4)
	{
	  break;
	}
      wkr = 0.5 * sin (ec * i0);
      wki = 0.5 * cos (ec * i0);
      wdr = 0.5 - (wkr * w1r - wki * w1i);
      wdi = wkr * w1i + wki * w1r;
      wkr = 0.5 - wkr;
      i = i0;
    }
  xr = a[2] - a[n - 2];
  xi = a[3] + a[n - 1];
  yr = wdr * xr - wdi * xi;
  yi = wdr * xi + wdi * xr;
  a[2] -= yr;
  a[3] -= yi;
  a[n - 2] += yr;
  a[n - 1] -= yi;
}


void
rftbsub (int n, double *a)
{
  int i, i0, j, k;
  double ec, w1r, w1i, wkr, wki, wdr, wdi, ss, xr, xi, yr, yi;

  ec = 2 * M_PI_2 / n;
  wkr = 0;
  wki = 0;
  wdi = cos (ec);
  wdr = sin (ec);
  wdi *= wdr;
  wdr *= wdr;
  w1r = 1 - 2 * wdr;
  w1i = 2 * wdi;
  ss = 2 * w1i;
  i = n >> 1;
  for (;;)
    {
      i0 = i - 4 * RDFT_LOOP_DIV;
      if (i0 < 4)
	{
	  i0 = 4;
	}
      for (j = i - 4; j >= i0; j -= 4)
	{
	  k = n - j;
	  xr = a[j + 2] - a[k - 2];
	  xi = a[j + 3] + a[k - 1];
	  yr = wdr * xr + wdi * xi;
	  yi = wdr * xi - wdi * xr;
	  a[j + 2] -= yr;
	  a[j + 3] -= yi;
	  a[k - 2] += yr;
	  a[k - 1] -= yi;
	  wkr += ss * wdi;
	  wki += ss * (0.5 - wdr);
	  xr = a[j] - a[k];
	  xi = a[j + 1] + a[k + 1];
	  yr = wkr * xr + wki * xi;
	  yi = wkr * xi - wki * xr;
	  a[j] -= yr;
	  a[j + 1] -= yi;
	  a[k] += yr;
	  a[k + 1] -= yi;
	  wdr += ss * wki;
	  wdi += ss * (0.5 - wkr);
	}
      if (i0 == 4)
	{
	  break;
	}
      wkr = 0.5 * sin (ec * i0);
      wki = 0.5 * cos (ec * i0);
      wdr = 0.5 - (wkr * w1r - wki * w1i);
      wdi = wkr * w1i + wki * w1r;
      wkr = 0.5 - wkr;
      i = i0;
    }
  xr = a[2] - a[n - 2];
  xi = a[3] + a[n - 1];
  yr = wdr * xr + wdi * xi;
  yi = wdr * xi - wdi * xr;
  a[2] -= yr;
  a[3] -= yi;
  a[n - 2] += yr;
  a[n - 1] -= yi;
}


void
dctsub (int n, double *a)
{
  int i, i0, j, k, m;
  double ec, w1r, w1i, wkr, wki, wdr, wdi, ss, xr, xi, yr, yi;

  ec = M_PI_2 / n;
  wkr = 0.5;
  wki = 0.5;
  w1r = cos (ec);
  w1i = sin (ec);
  wdr = 0.5 * (w1r - w1i);
  wdi = 0.5 * (w1r + w1i);
  ss = 2 * w1i;
  m = n >> 1;
  i = 0;
  for (;;)
    {
      i0 = i + 2 * DCST_LOOP_DIV;
      if (i0 > m - 2)
	{
	  i0 = m - 2;
	}
      for (j = i + 2; j <= i0; j += 2)
	{
	  k = n - j;
	  xr = wdi * a[j - 1] - wdr * a[k + 1];
	  xi = wdr * a[j - 1] + wdi * a[k + 1];
	  wkr -= ss * wdi;
	  wki += ss * wdr;
	  yr = wki * a[j] - wkr * a[k];
	  yi = wkr * a[j] + wki * a[k];
	  wdr -= ss * wki;
	  wdi += ss * wkr;
	  a[k + 1] = xr;
	  a[k] = yr;
	  a[j - 1] = xi;
	  a[j] = yi;
	}
      if (i0 == m - 2)
	{
	  break;
	}
      wdr = cos (ec * i0);
      wdi = sin (ec * i0);
      wkr = 0.5 * (wdr - wdi);
      wki = 0.5 * (wdr + wdi);
      wdr = wkr * w1r - wki * w1i;
      wdi = wkr * w1i + wki * w1r;
      i = i0;
    }
  xr = wdi * a[m - 1] - wdr * a[m + 1];
  a[m - 1] = wdr * a[m - 1] + wdi * a[m + 1];
  a[m + 1] = xr;
  a[m] *= wki + ss * wdr;
}


void
dstsub (int n, double *a)
{
  int i, i0, j, k, m;
  double ec, w1r, w1i, wkr, wki, wdr, wdi, ss, xr, xi, yr, yi;

  ec = M_PI_2 / n;
  wkr = 0.5;
  wki = 0.5;
  w1r = cos (ec);
  w1i = sin (ec);
  wdr = 0.5 * (w1r - w1i);
  wdi = 0.5 * (w1r + w1i);
  ss = 2 * w1i;
  m = n >> 1;
  i = 0;
  for (;;)
    {
      i0 = i + 2 * DCST_LOOP_DIV;
      if (i0 > m - 2)
	{
	  i0 = m - 2;
	}
      for (j = i + 2; j <= i0; j += 2)
	{
	  k = n - j;
	  xr = wdi * a[k + 1] - wdr * a[j - 1];
	  xi = wdr * a[k + 1] + wdi * a[j - 1];
	  wkr -= ss * wdi;
	  wki += ss * wdr;
	  yr = wki * a[k] - wkr * a[j];
	  yi = wkr * a[k] + wki * a[j];
	  wdr -= ss * wki;
	  wdi += ss * wkr;
	  a[j - 1] = xr;
	  a[j] = yr;
	  a[k + 1] = xi;
	  a[k] = yi;
	}
      if (i0 == m - 2)
	{
	  break;
	}
      wdr = cos (ec * i0);
      wdi = sin (ec * i0);
      wkr = 0.5 * (wdr - wdi);
      wki = 0.5 * (wdr + wdi);
      wdr = wkr * w1r - wki * w1i;
      wdi = wkr * w1i + wki * w1r;
      i = i0;
    }
  xr = wdi * a[m + 1] - wdr * a[m - 1];
  a[m + 1] = wdr * a[m + 1] + wdi * a[m - 1];
  a[m - 1] = xr;
  a[m] *= wki + ss * wdr;
}


void
dctsub4 (int n, double *a)
{
  int m;
  double wki, wdr, wdi, xr;

  wki = WR5000;
  m = n >> 1;
  if (m == 2)
    {
      wdr = wki * WI2500;
      wdi = wki * WR2500;
      xr = wdi * a[1] - wdr * a[3];
      a[1] = wdr * a[1] + wdi * a[3];
      a[3] = xr;
    }
  a[m] *= wki;
}


void
dstsub4 (int n, double *a)
{
  int m;
  double wki, wdr, wdi, xr;

  wki = WR5000;
  m = n >> 1;
  if (m == 2)
    {
      wdr = wki * WI2500;
      wdi = wki * WR2500;
      xr = wdi * a[3] - wdr * a[1];
      a[3] = wdr * a[3] + wdi * a[1];
      a[1] = xr;
    }
  a[m] *= wki;
}
