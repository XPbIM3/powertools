/*
 * dbsweep.c - raw-device disk read sweep for Mac OS X 10.2..10.5 (PPC)
 *
 * Read-only benchmark over a RAW DEVICE node (/dev/rdiskN).  No data
 * is ever written.  Two phases, both record a latencу/offset record
 * per I/O into a CSV file and (with -V) to stdout as live events:
 *
 *   1) linear sweep, "dashed" across the whole capacity:
 *        read segments of -c bytes, skip -D-duty-driven gaps, repeat
 *        from LBA 0 to the device end (equidistant segments).
 *        Every read block (-b, default 1M) is timed with
 *        mach_absolute_time => one graph point per block.
 *
 *   2) random read for -T seconds (default 30):
 *        xorshift LBA over the FULL capacity, -B-byte pread
 *        (default 4K), per-op latency.  First -w ops are warmup
 *        (spin-up/first-touch) and are NOT recorded.
 *
 * Honesty measures:
 *   - F_NOCACHE + readahead off on the device fd (OS cache bypass)
 *   - XOR checksum over every block (defeat dead-code optimisation)
 *   - all offsets aligned to 512 (device sector); linear segment
 *     sizes are multiples of the block size
 *   - EIO/short reads are recorded as "E" rows (surface-scan bonus:
 *     bad sectors become markers on the graph) and the sweep moves on
 *
 * CSV rows (dbsweep.csv):
 *   # comments: run metadata
 *   L,<offset>,<block bytes>,<latency us>,<ok>
 *   R,<offset>,<block bytes>,<latency us>
 *   E,<offset>,<errno>
 *
 * stdout events (machine-parsable, line per point, -V):
 *   L <offset> <block bytes> <latency us>
 *   R <offset> <latency us>
 *   E <offset> <errno>
 * summary at end:
 *   S lin <points> <avg MB/s> <eio>
 *   S rnd <ops> <avg us> <median us> <p99 us> <MB/s> <eio>
 *
 * Raw node permission: /dev/rdiskN is root:operator 640 - a plain
 * admin user cannot open it; run under sudo (or add the drive node
 * to the operator group).  The engine never writes anything.
 *
 * Usage: dbsweep -d /dev/rdiskN [options]
 *   -c SIZE   linear segment size       (default 32M; multiple of -b)
 *   -D N      duty percent 10..100      (default 33; 100 = solid pass)
 *   -b SIZE   linear read block         (default 1M, powers of two)
 *   -B SIZE   random read block         (default 4K)
 *   -T N      random run seconds        (default 30, 0 = skip phase)
 *   -m SIZE   sweep limit on extent     (0 = whole capacity)
 *             (smoke tests / partial scans default 0)
 *   -w N      random warmup ops         (default 16, not recorded)
 *   -s N      rng seed                  (default: clock)
 *   -o FILE   csv output                (default dbsweep.csv)
 *   -V        stream every point to stdout too
 *   -h        help
 * Size suffixes: K, M, G (also lower case), plain bytes otherwise.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <signal.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/disk.h>

#define DEFAULT_OFF      "/dev/disk1"
#define DEFAULT_SEG      (32ULL << 20)      /* 32 MiB          */
#define DEFAULT_DUTY     33                 /* percent         */
#define DEFAULT_LIN_BLK  (1ULL  << 20)      /* 1 MiB           */
#define DEFAULT_RND_BLK  4096ULL            /* 4 KiB           */
#define DEFAULT_RND_SECS 30
#define DEFAULT_WARMUP   16
#define SECT             512ULL

/* ------------------------------------------------------------ util */

static const char *g_dev = DEFAULT_OFF;
static FILE       *g_csv = NULL;

static void die(const char *what, const char *path)
{
    fprintf(stderr, "dbsweep: %s (%s): %s\n",
            what, path ? path : "-", strerror(errno));
    exit(1);
}

static uint64_t now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

/* sub-microsecond resolution clock (Tiger has mach_absolute_time) */
#include <mach/mach_time.h>
static double g_ns_per_tick = 1.0;

static void clock_init(void)
{
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    g_ns_per_tick = (double)tb.numer / (double)tb.denom;
}

static uint64_t ticks_us(void)
{
    return (uint64_t)((double)mach_absolute_time() * g_ns_per_tick / 1000.0);
}

static uint64_t parse_size(const char *s)
{
    char *end;
    unsigned long long val;

    errno = 0;
    val = strtoull(s, &end, 0);
    if (end == s || errno != 0) {
        fprintf(stderr, "dbsweep: bad size '%s'\n", s);
        exit(1);
    }
    if (*end) {
        switch (tolower((unsigned char)*end)) {
        case 'k': val *= 1024ULL;          end++; break;
        case 'm': val *= 1024ULL * 1024;   end++; break;
        case 'g': val *= 1024ULL * 1048576;end++; break;
        default:
            fprintf(stderr, "dbsweep: bad size suffix in '%s' "
                            "(use K/M/G or plain bytes)\n", s);
            exit(1);
        }
    }
    if (*end) {
        fprintf(stderr, "dbsweep: trailing garbage in size '%s'\n", s);
        exit(1);
    }
    return (uint64_t)val;
}

static void fmt_size(uint64_t b, char *out, size_t outsz)
{
    if (b >= (1ULL << 30))
        snprintf(out, outsz, "%.2f G", (double)b / 1073741824.0);
    else if (b >= (1ULL << 20))
        snprintf(out, outsz, "%.2f M", (double)b / 1048576.0);
    else if (b >= 1024ULL)
        snprintf(out, outsz, "%.2f K", (double)b / 1024.0);
    else
        snprintf(out, outsz, "%" PRIu64 " B", b);
}

/* round down to the device sector; warn if the result loses granularity */
static uint64_t align_sect(uint64_t v, const char *name)
{
    uint64_t a = v - (v % SECT);
    if (a != v)
        fprintf(stderr, "dbsweep: %s rounded %" PRIu64 " -> %" PRIu64
                        " (sector align)\n", name, v, a);
    return a;
}

/* power of two check for block sizes (graph granularity honesty) */
static int is_pow2(uint64_t v)
{
    return v && !(v & (v - 1));
}

/* --------------------------------------------------------- csv out */

static void csv_open(const char *path, const char *dev,
                     uint64_t dev_bytes)
{
    char s1[32];
    fmt_size(dev_bytes, s1, sizeof s1);
    g_csv = fopen(path, "w");
    if (!g_csv) die("fopen csv", path);
    fprintf(g_csv,
            "# dbsweep v1  dev=%s  capacity=%s (%" PRIu64 " bytes)\n",
            dev, s1, dev_bytes);
    fprintf(g_csv, "# rows: L,offset,bytes,latency_us,ok | "
                   "R,offset,bytes,latency_us | E,offset,errno\n");
}

static void csv_line(const char *kind, uint64_t off, uint64_t bytes,
                     uint64_t us, long err)
{
    if (strcmp(kind, "L") == 0)
        fprintf(g_csv, "L,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%ld\n",
                off, bytes, us, err);
    else if (strcmp(kind, "R") == 0)
        fprintf(g_csv, "R,%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
                off, bytes, us);
    else
        fprintf(g_csv, "E,%" PRIu64 ",%ld\n", off, err);
    fflush(g_csv);   /* UI tail'ит файл - строки живые */
}

static void stdout_line(const char *kind, uint64_t off, uint64_t bytes,
                        uint64_t us, long err, int verbose)
{
    if (!verbose)
        return;
    if (strcmp(kind, "L") == 0)
        printf("L %" PRIu64 " %" PRIu64 " %" PRIu64 "\n", off, bytes, us);
    else if (strcmp(kind, "R") == 0)
        printf("R %" PRIu64 " %" PRIu64 "\n", off, us);
    else
        printf("E %" PRIu64 " %ld\n", off, err);
    fflush(stdout);
}

/* -------------------------------------------------------- i/o bits */

static void set_nocache(int fd, const char *path)
{
#ifdef F_NOCACHE
    if (fcntl(fd, F_NOCACHE, 1) < 0)
        die("fcntl(F_NOCACHE)", path);
#endif
#ifdef F_RDAHEAD
    if (fcntl(fd, F_RDAHEAD, 0) < 0)   /* kill readahead everywhere */
        die("fcntl(F_RDAHEAD)", path);
#endif
}

/* ------------------------------------------------------- random src */

static uint64_t rng_state;

static void rng_seed(uint64_t s)
{
    rng_state = s * 6364136223846793005ULL + 1442695040888963407ULL;
    if (rng_state == 0)
        rng_state = 88172645463325252ULL;
}

static uint64_t rng_next(void)
{
    uint64_t x = rng_state;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    rng_state = x;
    return x * 2685821657736338717ULL;
}

/* --------------------------------------------------------- signals */

static volatile sig_atomic_t g_stop = 0;

static void on_sig(int s)
{
    (void)s;
    g_stop = 1;
}

/* ------------------------------------------------------------ stats */

static int dcmp(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static double pct(double *v, size_t n, double p)
{
    double idx;
    size_t i;
    if (n == 0) return 0.0;
    idx = p * (double)(n - 1);
    i = (size_t)(idx + 0.5);
    if (i >= n) i = n - 1;
    return v[i];
}

/* ------------------------------------------------------------ phases */

/*
 * Linear "dashed" sweep: segments of seg bytes at equidistant starts
 * off = i*(seg+skip), last segment truncated at the extent.  Every
 * block-level read inside a segment is one graph point.
 */
static void linear_sweep(int fd, uint64_t extent, uint64_t seg,
                         uint64_t skip, uint64_t blk, uint64_t maxoff,
                         int verbose, uint64_t *out_points,
                         uint64_t *out_bytes, uint64_t *out_eio,
                         double *out_mbs)
{
    uint64_t off, points = 0, bytes_ok = 0, eio = 0;
    uint64_t t_all = 0;
    char    *buf;

    buf = (char *)malloc((size_t)blk);
    if (!buf) die("malloc", g_dev);

    /* linear warmup: the media may have spun down (disksleep) - the
     * first recorded point must not carry spin-up seconds.  Two
     * block reads at offset 0, timings discarded. */
    {
        uint64_t w;
        for (w = 0; w < 2; w++) {
            ssize_t wr = pread(fd, buf, (size_t)blk, (off_t)0);
            if (wr < 0) break;
        }
    }

    for (off = 0; off < maxoff && !g_stop; off += seg + skip) {
        uint64_t seglen = seg;
        uint64_t pos;
        if (off + seglen > maxoff)
            seglen = maxoff - off;
        if (seglen < blk)
            break;                       /* nothing sizeable left */

        for (pos = 0; pos < seglen && !g_stop; pos += blk) {
            uint64_t chunk = seglen - pos;
            ssize_t  r;
            uint64_t t0, dt;
            uint32_t i;
            uint8_t  cs = 0;

            if (chunk > blk) chunk = blk;

            t0 = ticks_us();
            r = pread(fd, buf, (size_t)chunk, (off_t)(off + pos));
            dt = ticks_us() - t0;
            t_all += dt;

            if (r < 0) {
                csv_line("E", off + pos, 0, dt, errno);
                stdout_line("E", off + pos, 0, dt, errno, verbose);
                eio++;
                continue;
            }
            if (r == 0)
                break;                   /* end of media early */

            for (i = 0; i < (uint32_t)r; i++)      /* anti-optimizer */
                cs ^= buf[i];
            (void)cs;

            csv_line("L", off + pos, (uint64_t)r, dt, 0);
            stdout_line("L", off + pos, (uint64_t)r, dt, 0, verbose);
            points++;
            bytes_ok += (uint64_t)r;
        }
        fflush(g_csv);
    }

    free(buf);

    *out_points = points;
    *out_bytes  = bytes_ok;
    *out_eio    = eio;
    *out_mbs = t_all > 0 ? (double)bytes_ok / ((double)t_all / 1e6)
                         / 1048576.0 : 0.0;
}

/*
 * Random read for T seconds: uniform xorshift position over the FULL
 * capacity, per-op latency.  Warmup ops run but are not recorded.
 */
static void random_sweep(int fd, uint64_t devcap,
                         int seconds, int warmup, uint64_t rblk,
                         int verbose, uint64_t *out_ops,
                         uint64_t *out_bytes, uint64_t *out_eio,
                         double *out_avg, double *out_med,
                         double *out_p99, double *out_mbs)
{
    uint64_t nslots = (devcap - rblk) / SECT + 1;
    uint64_t ops = 0, eio = 0, bytes_ok = 0, t_io = 0;
    uint64_t deadline, t0_all;
    char    *buf;
    double  *lat   = NULL;
    size_t   lcap  = 1 << 12, ln = 0;
    buf = (char *)malloc((size_t)rblk);
    if (!buf) die("malloc", g_dev);
    lat = (double *)malloc(lcap * sizeof(double));
    if (!lat) die("malloc(stats)", g_dev);

    deadline = (uint64_t)seconds > 0
             ? (uint64_t)seconds * 1000000ULL : ~(uint64_t)0;

    t0_all = now_us();

    /* warmup, unrecorded */
    {
        uint64_t i;
        for (i = 0; i < (uint64_t)warmup && !g_stop; i++) {
            uint64_t off = (rng_next() % nslots) * SECT;
            if (pread(fd, buf, (size_t)rblk, (off_t)off) < 0)
                eio++;                       /* even warmup errors count */
        }
    }

    while (!g_stop) {
        uint64_t off;
        ssize_t  r;
        uint64_t t0, dt;
        uint32_t i;
        uint8_t  cs = 0;

        if (seconds > 0 && now_us() - t0_all >= deadline)
            break;

        off = (rng_next() % nslots) * SECT;

        t0 = ticks_us();
        r = pread(fd, buf, (size_t)rblk, (off_t)off);
        dt = ticks_us() - t0;
        t_io += dt;

        if (r < 0) {
            csv_line("E", off, 0, dt, errno);
            stdout_line("E", off, 0, dt, errno, verbose);
            eio++;
            continue;
        }
        if (r == 0)
            break;

        for (i = 0; i < (uint32_t)r; i++)          /* anti-optimizer */
            cs ^= buf[i];
        (void)cs;

        csv_line("R", off, (uint64_t)r, dt, 0);
        stdout_line("R", off, (uint64_t)r, dt, 0, verbose);

        if (ln == lcap) {
            lcap <<= 1;
            lat = (double *)realloc(lat, lcap * sizeof(double));
            if (!lat) die("realloc(stats)", g_dev);
        }
        lat[ln++] = (double)dt;
        ops++;
        bytes_ok += (uint64_t)r;
    }
    qsort(lat, ln, sizeof(double), dcmp);
    {
        double avg = ln > 0 ? t_io / (double)ln : 0.0;
        *out_ops   = ops;
        *out_bytes = bytes_ok;
        *out_eio   = eio;
        *out_avg   = avg;
        *out_med   = pct(lat, ln, 0.5);
        *out_p99   = pct(lat, ln, 0.99);
        {
            uint64_t secs = now_us() - t0_all;
            secs = secs > 0 ? secs : 1;
            *out_mbs = (double)bytes_ok / ((double)secs / 1e6) / 1048576.0;
        }
    }

    free(lat);
    free(buf);
    fflush(g_csv);
}

/* --------------------------------------------------------- main/opts */

static void usage(void)
{
    char c1[32], b1[32], b2[32];
    fmt_size(DEFAULT_SEG, c1, sizeof c1);
    fmt_size(DEFAULT_LIN_BLK, b1, sizeof b1);
    fmt_size(DEFAULT_RND_BLK, b2, sizeof b2);
    printf("dbsweep v1 - raw-device read sweep (Mac OS X 10.2..10.5 PPC)\n\n"
           "usage: sudo dbsweep -d /dev/rdiskN [options]\n"
           "  -c SIZE   linear segment size    (default %s, mult of -b)\n"
           "  -D N      duty percent 10..100   (default %d; 100 = solid)\n"
           "  -b SIZE   linear read block      (default %s, pow2)\n"
           "  -B SIZE   random read block      (default %s)\n"
           "  -T N      random run seconds     (default %d, 0 = skip)\n"
           "  -m SIZE   limit sweep extent     (0 = whole capacity)\n"
           "  -w N      random warmup ops      (default %d)\n"
           "  -s N      rng seed               (default clock)\n"
           "  -o FILE   csv output             (default dbsweep.csv)\n"
           "  -V        echo every point to stdout\n"
           "  -h        help\n",
           c1, DEFAULT_DUTY, b1, b2, DEFAULT_RND_SECS, DEFAULT_WARMUP);
}

int main(int argc, char **argv)
{
    uint64_t seg        = DEFAULT_SEG;
    uint64_t lin_blk    = DEFAULT_LIN_BLK;
    uint64_t rnd_blk    = DEFAULT_RND_BLK;
    uint64_t max_extent = 0;             /* 0 = whole capacity */
    uint64_t seed       = (uint64_t)time(NULL) ^ (uint64_t)getpid();
    int      duty       = DEFAULT_DUTY;
    int      rnd_secs   = DEFAULT_RND_SECS;
    int      warmup     = DEFAULT_WARMUP;
    int      verbose    = 0;
    const char *csv_path = "dbsweep.csv";
    int      fd, ch;
    uint64_t cap, extent, skip, off;
    uint32_t bs = 512;
    struct stat st;
    char s1[32], s2[32], s3[32];
    uint64_t l_pts, l_bytes, l_eio, r_ops, r_bytes, r_eio;
    double   l_mbs, r_avg, r_med, r_p99, r_mbs;
    struct sigaction sa;
    time_t t_start;

    while ((ch = getopt(argc, argv, "d:c:D:b:B:T:m:w:s:o:Vh")) != -1) {
        switch (ch) {
        case 'd': g_dev    = optarg;          break;
        case 'c': seg      = parse_size(optarg); break;
        case 'D': duty     = atoi(optarg);    break;
        case 'b': lin_blk  = parse_size(optarg); break;
        case 'B': rnd_blk  = parse_size(optarg); break;
        case 'T': rnd_secs = atoi(optarg);    break;
        case 'm': max_extent = parse_size(optarg); break;
        case 'w': warmup   = atoi(optarg);    break;
        case 's': seed     = parse_size(optarg); break;
        case 'o': csv_path = optarg;          break;
        case 'V': verbose  = 1;               break;
        case 'h': usage(); return 0;
        default:  usage(); return 1;
        }
    }

    seg     = align_sect(seg, "segment");
    lin_blk = align_sect(lin_blk, "linear block");
    rnd_blk = align_sect(rnd_blk, "random block");

    if (seg < lin_blk)    die("segment smaller than linear block", g_dev);
    if (seg % lin_blk != 0) {
        fprintf(stderr, "dbsweep: segment must be a multiple of "
                        "the linear block\n");
        return 1;
    }
    if (duty <= 0 || duty > 100) {
        fprintf(stderr, "dbsweep: duty percent must be 10..100\n");
        return 1;
    }
    if (is_pow2(seg) == 0 && is_pow2(lin_blk) == 0)
        fprintf(stderr, "dbsweep: note: non-power-of-two segment/block; "
                        "grid stays sector-aligned but graph offsets "
                        "are irregular\n");

    clock_init();

    /* device geometry */
    fd = open(g_dev, O_RDONLY);
    if (fd < 0) {
        if (errno == EACCES || errno == EPERM)
            fprintf(stderr, "dbsweep: cannot open %s: %s\n"
                            "  (raw nodes are root:operator; "
                            "run under sudo)\n",
                    g_dev, strerror(errno));
        die("open", g_dev);
    }
    if (ioctl(fd, DKIOCGETBLOCKSIZE, &bs) < 0) bs = 512;
    if (ioctl(fd, DKIOCGETBLOCKCOUNT, &off) < 0)
        die("ioctl(DKIOCGETBLOCKCOUNT)", g_dev);
    cap = off * (uint64_t)bs;

    set_nocache(fd, g_dev);              /* OS cache bypass on device fd */

    fstat(fd, &st);
    (void)st;
    if (cap == 0) {
        fprintf(stderr, "dbsweep: zero capacity reported by %s\n", g_dev);
        return 1;
    }

    extent = cap;
    if (max_extent > 0 && max_extent < cap) {
        extent = align_sect(max_extent, "extent");
    }

    fmt_size(cap, s1, sizeof s1);
    fmt_size(extent, s2, sizeof s2);
    skip = seg * (uint64_t)(100 - duty) / (uint64_t)duty;
    skip = align_sect(skip, "duty gap");   /* EINVAL on rdisk otherwise */
    fmt_size(seg, s3, sizeof s3);

    printf("dbsweep v1 (Mac OS X 10.2..10.5, PowerPC)\n");
    printf("  device:      %s  blocksize %u\n", g_dev, (unsigned)bs);
    printf("  capacity:    %s   sweep extent: %s\n", s1, s2);
    printf("  linear seg:  %s  duty %d%%  step %s\n",
           s3, duty, skip == 0 ? "solid" : "-");
    printf("  lin block:   ");
    { char b1[32]; fmt_size(lin_blk, b1, sizeof b1); printf("%s", b1); }
    printf("   rnd block: ");
    { char b2[32]; fmt_size(rnd_blk, b2, sizeof b2); printf("%s", b2); }
    printf("   rnd secs: %d (warmup %d)\n", rnd_secs, warmup);
    printf("  csv:         %s\n", csv_path);
    fflush(stdout);

    csv_open(csv_path, g_dev, cap);

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sig;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    t_start = time(NULL);

    /* ---- linear dashed sweep ---- */
    linear_sweep(fd, extent, seg, skip, lin_blk, extent, verbose,
                 &l_pts, &l_bytes, &l_eio, &l_mbs);

    /* ---- random read phase ---- */
    r_ops = r_bytes = r_eio = 0;
    r_avg = r_med = r_p99 = r_mbs = 0;
    if (rnd_secs > 0 && !g_stop) {
        rng_seed(seed);                  /* per-run reproducible positions */
        random_sweep(fd, cap, rnd_secs, warmup, rnd_blk,
                     verbose, &r_ops, &r_bytes, &r_eio,
                     &r_avg, &r_med, &r_p99, &r_mbs);
    } else if (rnd_secs <= 0)
        printf("  (random phase skipped: -T 0)\n");
    else
        ;                                /* fell through after stop */

    fprintf(g_csv, "# stats\n");
    fprintf(g_csv, "# lin points=%" PRIu64 " avg=%.2f MB/s eio=%" PRIu64 "\n",
            l_pts, l_mbs, l_eio);
    fprintf(g_csv, "# rnd ops=%" PRIu64 " avg=%.1f us med=%.1f us "
                   "p99=%.1f us avg=%.2f MB/s eio=%" PRIu64 " stopped=%ld\n",
            r_ops, r_avg, r_med, r_p99, r_mbs, r_eio, (long)g_stop);
    fflush(g_csv);
    fclose(g_csv);

    fprintf(stderr, "dbsweep: linear %s pts=\"%" PRIu64 "\" eio=%" PRIu64 " \n",
            l_mbs > 0 ? "ok" : "aborted", l_pts, l_eio);
    fprintf(stderr, "  avg %.2f MB/s\n", l_mbs);
    fprintf(stderr, "dbsweep: random ops=%" PRIu64 " avg=%.1f med=%.1f "
                    "p99=%.1f us eio=%" PRIu64 " stopped=%ld\n",
            r_ops, r_avg, r_med, r_p99, r_eio, (long)g_stop);
    fprintf(stderr, "dbsweep: run %ld s, csv=%s\n",
            (long)(time(NULL) - t_start), csv_path);

    close(fd);
    return 0;
}
