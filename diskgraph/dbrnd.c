/*
 * dbrnd.c - random read test по времени, человекочитаемый stdout
 *           (Mac OS X 10.2..10.5, PowerPC, парк)
 *
 * -T-секундный прогон random 4K-чтений по raw-девайсу: xorshift
 * позиция uniform по ВСЕЙ ёмкости, per-op латентность. Только
 * ЧТЕНИЕ - данные не пишутся никогда.
 *
 * Честность (канон парка, тот же набор, что в dbsweep):
 *   - только raw-нода /dev/rdiskN (блочная /dev/diskN - warning);
 *   - F_NOCACHE + readahead off на device-fd;
 *   - XOR-чексум по каждому опу (анти-оптимизация);
 *   - mach_absolute_time (суб-µs на PPC);
 *   - офсеты выровнены на 512 (сектор);
 *   - прогрев спин-апа: -w опов читаются, в данные НЕ идут.
 *
 * stdout - единственный выходной канал, человекочитаемый:
 *   шапка, строка на каждый оп, строки ошибок, финальная сводка "---"
 *   (ops, медиана, p99, среднее, eio).
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
#include <sys/disk.h>
#include <mach/mach_time.h>

#define DEFAULT_DEV   "/dev/rdisk1"
#define DEFAULT_RND_BLK 4096ULL            /* 4 KiB            */
#define DEFAULT_SECS  10                   /* секунд           */
#define DEFAULT_WARMUP 16
#define SECT          512ULL

/* ------------------------------------------------------------ clock */
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

/* ------------------------------------------------------------ util */
static const char *g_dev = DEFAULT_DEV;

static void die(const char *what, const char *path)
{
    fprintf(stderr, "dbrnd: %s (%s): %s\n",
            what, path ? path : "-", strerror(errno));
    exit(1);
}

static uint64_t parse_size(const char *s)
{
    char *end;
    unsigned long long val;

    errno = 0;
    val = strtoull(s, &end, 0);
    if (end == s || errno != 0) {
        fprintf(stderr, "dbrnd: bad size '%s'\n", s);
        exit(1);
    }
    if (*end) {
        switch (tolower((unsigned char)*end)) {
        case 'k': val *= 1024ULL;        end++; break;
        case 'm': val *= 1024ULL * 1024; end++; break;
        case 'g': val *= 1024ULL * 1048576; end++; break;
        default:
            fprintf(stderr, "dbrnd: bad size suffix in '%s' "
                            "(use K/M/G or plain bytes)\n", s);
            exit(1);
        }
    }
    if (*end) {
        fprintf(stderr, "dbrnd: trailing garbage in size '%s'\n", s);
        exit(1);
    }
    return (uint64_t)val;
}

static void fmt_gb(uint64_t off, char *out, size_t outsz)
{   /* человекочитаемый офсет в двоичных GB, ширина как в dblin */
    snprintf(out, outsz, "%7.2f", (double)off / 1073741824.0);
}

static void fmt_ms(uint64_t us, char *out, size_t outsz)
{
    if (us >= 1000000)
        snprintf(out, outsz, "%.2f s", (double)us / 1e6);
    else if (us >= 10000)
        snprintf(out, outsz, "%.1f ms", (double)us / 1e3);
    else if (us >= 1000)
        snprintf(out, outsz, "%.2f ms", (double)us / 1e3);
    else
        snprintf(out, outsz, "%.0f us", (double)us);
}

static void set_nocache(int fd, const char *path)
{
    if (fcntl(fd, F_NOCACHE, 1) < 0)
        die("fcntl(F_NOCACHE)", path);
    if (fcntl(fd, F_RDAHEAD, 0) < 0)
        die("fcntl(F_RDAHEAD)", path);
}

/* ------------------------------------------------------------ random */
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

/* ------------------------------------------------------------ signals */
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

static void usage(void)
{
    printf("dbrnd 1.0 - timed random read test (Mac OS X 10.2..10.5 PPC)\n\n"
           "usage: sudo dbrnd -d /dev/rdiskN [options]\n"
           "  -B SIZE   random block       default 4K\n"
           "  -T N      run seconds        default 10\n"
           "  -w N      warmup ops         default 16 (not recorded)\n"
           "  -s N      rng seed           default clock\n"
           "  -h        help\n");
}

int main(int argc, char **argv)
{
    uint64_t rblk   = DEFAULT_RND_BLK;
    uint64_t seed   = (uint64_t)time(NULL) ^ (uint64_t)getpid();
    int      secs   = DEFAULT_SECS;
    int      warmup = DEFAULT_WARMUP;
    int      fd, ch;
    uint64_t cap, nslots, off;
    uint32_t bs = 512;
    uint64_t ops = 0, eio = 0, t_io = 0;
    char    *buf;
    double  *lat   = NULL;
    size_t   lcap  = 1 << 12, ln = 0;
    time_t   t0wall;
    struct sigaction sa;

    while ((ch = getopt(argc, argv, "d:B:T:w:s:h")) != -1) {
        switch (ch) {
        case 'd': g_dev   = optarg;   break;
        case 'B': rblk   = parse_size(optarg); break;
        case 'T': secs   = atoi(optarg);   break;
        case 'w': warmup = atoi(optarg);   break;
        case 's': seed   = parse_size(optarg); break;
        case 'h': usage(); return 0;
        default:  usage(); return 1;
        }
    }

    rblk = rblk - (rblk % SECT);       /* сектор-выравнивание блока */
    if (rblk == 0) rblk = SECT;
    if (secs < 1) {
        fprintf(stderr, "dbrnd: run seconds must be at least 1\n");
        return 1;
    }

    clock_init();

    /* геометрия девайса */
    fd = open(g_dev, O_RDONLY);
    if (fd < 0) {
        if (errno == EACCES || errno == EPERM)
            fprintf(stderr, "dbrnd: cannot open %s: %s\n"
                            "  (raw nodes are root:operator; "
                            "run under sudo; use /dev/rdiskN)\n",
                    g_dev, strerror(errno));
        die("open", g_dev);
    }
    if (strncmp(g_dev, "/dev/rdisk", 10) != 0)
        fprintf(stderr, "dbrnd: WARNING: %s is not a raw node - "
                        "use /dev/rdiskN for honest latency\n", g_dev);
    if (ioctl(fd, DKIOCGETBLOCKSIZE, &bs) < 0) bs = 512;
    if (ioctl(fd, DKIOCGETBLOCKCOUNT, &off) < 0)
        die("ioctl(DKIOCGETBLOCKCOUNT)", g_dev);
    cap = off * (uint64_t)bs;
    if (cap == 0) {
        fprintf(stderr, "dbrnd: zero capacity reported by %s\n", g_dev);
        return 1;
    }
    set_nocache(fd, g_dev);

    if (rblk > cap) {
        fprintf(stderr, "dbrnd: block bigger than the device\n");
        return 1;
    }
    nslots = (cap - rblk) / SECT + 1;

    {
        char gb[24], b1[24];
        snprintf(gb, sizeof gb, "%.2f G", (double)cap / 1073741824.0);
        snprintf(b1, sizeof b1, "%.2f K", (double)rblk / 1024.0);
        printf("dbrnd 1.0  dev %s  capacity %s\n", g_dev, gb);
        printf("  block %s  %d s  warmup %d  seed %" PRIu64 "\n",
               b1, secs, warmup, seed);
    }
    fflush(stdout);

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sig;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    rng_seed(seed);
    t0wall = time(NULL);

    buf = (char *)malloc((size_t)rblk);
    if (!buf) die("malloc", g_dev);
    lat = (double *)malloc(lcap * sizeof(double));
    if (!lat) die("malloc(stats)", g_dev);

    /* ---- warmup: считается, но в данные не идёт ---- */
    {
        uint64_t i;
        for (i = 0; i < (uint64_t)warmup && !g_stop; i++) {
            uint64_t wo = (rng_next() % nslots) * SECT;
            if (pread(fd, buf, (size_t)rblk, (off_t)wo) < 0)
                eio++;
        }
    }

    /* ---- сам прогон по времени ---- */
    while (!g_stop) {
        uint64_t t0, dt;
        ssize_t  r;
        uint32_t i;
        uint8_t  cs = 0;
        char     gb[24], ms[24];

        if (time(NULL) - t0wall >= (time_t)secs)
            break;

        off = (rng_next() % nslots) * SECT;

        t0 = ticks_us();
        r = pread(fd, buf, (size_t)rblk, (off_t)off);
        dt = ticks_us() - t0;
        t_io += dt;

        if (r < 0) {
            fmt_gb(off, gb, sizeof gb);
            fmt_ms(dt, ms, sizeof ms);
            printf("E %s G %8s  (errno %ld)\n", gb, ms, (long)errno);
            fflush(stdout);
            eio++;
            continue;
        }
        if (r == 0)
            break;                       /* край носителя */

        for (i = 0; i < (uint32_t)r; i++)          /* анти-оптимизатор */
            cs ^= buf[i];
        (void)cs;

        fmt_gb(off, gb, sizeof gb);
        fmt_ms(dt, ms, sizeof ms);
        printf("R %s G %8s\n", gb, ms);
        fflush(stdout);

        if (ln == lcap) {
            lcap <<= 1;
            lat = (double *)realloc(lat, lcap * sizeof(double));
            if (!lat) die("realloc(stats)", g_dev);
        }
        lat[ln++] = (double)dt;
        ops++;
    }

    free(buf);
    close(fd);

    qsort(lat, ln, sizeof(double), dcmp);
    {
        double avg = ln > 0 ? (double)t_io / (double)ln : 0.0;
        char m1[24], m2[24], m3[24];
        int  mins = (int)(time(NULL) - t0wall) / 60;
        int  s     = (int)(time(NULL) - t0wall) % 60;

        fmt_ms((uint64_t)(pct(lat, ln, 0.5) + 0.5), m1, sizeof m1);
        fmt_ms((uint64_t)(pct(lat, ln, 0.99) + 0.5), m2, sizeof m2);
        fmt_ms((uint64_t)(avg + 0.5), m3, sizeof m3);
        printf("---  ops %" PRIu64 "  med %s  p99 %s  avg %s  eio %" PRIu64
               "%s  time %d:%02d\n",
               ops, m1, m2, m3, eio, g_stop ? "  [stopped]" : "",
               mins, s);
        fflush(stdout);
    }
    free(lat);
    return 0;
}
