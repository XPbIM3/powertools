/*
 * dblin.c - линейный пунктирный тест чтения, человекочитаемый stdout
 *           (Mac OS X 10.2..10.5, PowerPC, парк)
 *
 * Проход по raw-девайсу в стиле HD Tune: сегменты (чанки) стартуют
 * эквидистантно от LBA0, шаг = chunk*(100-duty)/duty (duty 100 =
 * сплошной проход). Каждый -b блок внутри чанка таймится отдельно.
 * Только ЧТЕНИЕ - данные не пишутся никогда.
 *
 * Честность (канон парка, тот же набор, что в dbsweep):
 *   - только raw-нода /dev/rdiskN (блочная /dev/diskN - warning:
 *     буферный слой врёт в лагах);
 *   - F_NOCACHE + readahead off на device-fd;
 *   - XOR-чексум по каждому блоку (анти-оптимизация);
 *   - mach_absolute_time (суб-µs на PPC);
 *   - всё выравнивание на 512, duty-шаг квантуется по сектору
 *     (не кратный 512 ловит EINVAL на rdisk);
 *   - прогрев спин-апа: 2 блока в момент старта, в данные НЕ идут.
 *
 * stdout - единственный выходной канал, человекочитаемый:
 *   шапка, строка на каждый I/O, строки ошибок, финальная сводка "---".
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
#define DEFAULT_SEG   (32ULL << 20)      /* 32 MiB чанк      */
#define DEFAULT_BLK   (1ULL  << 20)      /* 1 MiB блок       */
#define DEFAULT_DUTY  10                 /* percent          */
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
    fprintf(stderr, "dblin: %s (%s): %s\n",
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
        fprintf(stderr, "dblin: bad size '%s'\n", s);
        exit(1);
    }
    if (*end) {
        switch (tolower((unsigned char)*end)) {
        case 'k': val *= 1024ULL;        end++; break;
        case 'm': val *= 1024ULL * 1024; end++; break;
        case 'g': val *= 1024ULL * 1048576; end++; break;
        default:
            fprintf(stderr, "dblin: bad size suffix in '%s' "
                            "(use K/M/G or plain bytes)\n", s);
            exit(1);
        }
    }
    if (*end) {
        fprintf(stderr, "dblin: trailing garbage in size '%s'\n", s);
        exit(1);
    }
    return (uint64_t)val;
}

static void fmt_gb(uint64_t off, char *out, size_t outsz)
{   /* человекочитаемый офсет в двоичных GB */
    snprintf(out, outsz, "%7.2f", (double)off / 1073741824.0);
}

static void fmt_ms(uint64_t us, char *out, size_t outsz)
{   /* человеческий лаг: s / ms / us */
    if (us >= 1000000)
        snprintf(out, outsz, "%.2f s", (double)us / 1e6);
    else if (us >= 10000)
        snprintf(out, outsz, "%.1f ms", (double)us / 1e3);
    else if (us >= 1000)
        snprintf(out, outsz, "%.2f ms", (double)us / 1e3);
    else
        snprintf(out, outsz, "%.0f us", (double)us);
}

static uint64_t align_sect(uint64_t v, const char *name)
{
    uint64_t a = v - (v % SECT);
    if (a != v)
        fprintf(stderr, "dblin: %s rounded %" PRIu64 " -> %" PRIu64
                        " (sector align)\n", name, v, a);
    return a;
}

static int is_pow2(uint64_t v)
{
    return v && !(v & (v - 1));
}

static void set_nocache(int fd, const char *path)
{
    if (fcntl(fd, F_NOCACHE, 1) < 0)
        die("fcntl(F_NOCACHE)", path);
    if (fcntl(fd, F_RDAHEAD, 0) < 0)   /* readahead везде выкл */
        die("fcntl(F_RDAHEAD)", path);
}

/* ------------------------------------------------------------ signals */
static volatile sig_atomic_t g_stop = 0;

static void on_sig(int s)
{
    (void)s;
    g_stop = 1;
}

static void usage(void)
{
    printf("dblin 1.0 - linear dashed read sweep (Mac OS X 10.2..10.5 PPC)\n\n"
           "usage: sudo dblin -d /dev/rdiskN [options]\n"
           "  -c SIZE   chunk (segment) size  default 32M, multiple of -b\n"
           "  -b SIZE   read block            default 1M, power of two\n"
           "  -D N      duty percent 10..100  default 10 (100 = solid)\n"
           "  -m SIZE   sweep limit           default 0 = whole capacity\n"
           "  -h        help\n");
}

int main(int argc, char **argv)
{
    uint64_t seg     = DEFAULT_SEG;
    uint64_t blk     = DEFAULT_BLK;
    uint64_t maxoff  = 0;              /* 0 = whole capacity */
    int      duty    = DEFAULT_DUTY;
    int      fd, ch;
    uint64_t cap, skip, off, t;
    uint32_t bs = 512;
    uint64_t points = 0, bytes_ok = 0, eio = 0, t_all = 0;
    char    *buf;
    time_t   t0wall;
    struct sigaction sa;

    while ((ch = getopt(argc, argv, "d:c:b:D:m:h")) != -1) {
        switch (ch) {
        case 'd': g_dev   = optarg; break;
        case 'c': seg     = parse_size(optarg); break;
        case 'b': blk     = parse_size(optarg); break;
        case 'D': duty    = atoi(optarg);   break;
        case 'm': maxoff  = parse_size(optarg); break;
        case 'h': usage(); return 0;
        default:  usage(); return 1;
        }
    }

    seg = align_sect(seg, "chunk");
    blk = align_sect(blk, "block");
    if (seg < blk)        die("chunk smaller than block", g_dev);
    if (seg % blk != 0) {
        fprintf(stderr, "dblin: chunk must be a multiple of the block\n");
        return 1;
    }
    if (duty < 10 || duty > 100) {
        fprintf(stderr, "dblin: duty percent must be 10..100\n");
        return 1;
    }
    if (is_pow2(blk) == 0)
        fprintf(stderr, "dblin: note: non-power-of-two block; "
                        "grid stays sector-aligned\n");

    clock_init();

    /* геометрия девайса */
    fd = open(g_dev, O_RDONLY);
    if (fd < 0) {
        if (errno == EACCES || errno == EPERM)
            fprintf(stderr, "dblin: cannot open %s: %s\n"
                            "  (raw nodes are root:operator; "
                            "run under sudo; use /dev/rdiskN)\n",
                    g_dev, strerror(errno));
        die("open", g_dev);
    }
    if (strncmp(g_dev, "/dev/rdisk", 10) != 0)
        fprintf(stderr, "dblin: WARNING: %s is not a raw node - "
                        "use /dev/rdiskN for honest latency\n", g_dev);
    if (ioctl(fd, DKIOCGETBLOCKSIZE, &bs) < 0) bs = 512;
    if (ioctl(fd, DKIOCGETBLOCKCOUNT, &t) < 0)
        die("ioctl(DKIOCGETBLOCKCOUNT)", g_dev);
    cap = t * (uint64_t)bs;
    if (cap == 0) {
        fprintf(stderr, "dblin: zero capacity reported by %s\n", g_dev);
        return 1;
    }
    set_nocache(fd, g_dev);

    if (maxoff == 0 || maxoff > cap)
        maxoff = cap;
    maxoff = align_sect(maxoff, "extent");

    skip = seg * (uint64_t)(100 - duty) / (uint64_t)duty;
    skip = align_sect(skip, "duty gap");   /* EINVAL on rdisk otherwise */

    {
        char gb[24], c1[24], b1[24];
        snprintf(gb, sizeof gb, "%.2f G", (double)cap / 1073741824.0);
        snprintf(c1, sizeof c1, "%.2f M", (double)seg / 1048576.0);
        snprintf(b1, sizeof b1, "%.2f M", (double)blk / 1048576.0);
        printf("dblin 1.0  dev %s  capacity %s\n", g_dev, gb);
        printf("  chunk %s  block %s  duty %d%%  %s\n",
               c1, b1, duty, skip == 0 ? "solid" : "dashed");
    }
    fflush(stdout);

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sig;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    buf = (char *)malloc((size_t)blk);
    if (!buf) die("malloc", g_dev);

    /* линейный прогрев: пластина могла уснуть (disksleep) - два блока
     * читаются, тайминги выбрасываются, в данные не идут */
    {
        uint64_t w;
        for (w = 0; w < 2; w++)
            if (pread(fd, buf, (size_t)blk, (off_t)0) < 0)
                break;
    }

    t0wall = time(NULL);

    /* ---- сам проход: чанки эквидистантно, блоки внутри чанков ---- */
    for (off = 0; off < maxoff && !g_stop; off += seg + skip) {
        uint64_t seglen = seg;
        uint64_t pos;
        if (off + seglen > maxoff)
            seglen = maxoff - off;
        if (seglen < blk)
            break;                       /* хвост < блока - нечего мерить */

        for (pos = 0; pos < seglen && !g_stop; pos += blk) {
            uint64_t chunk = seglen - pos;
            ssize_t  r;
            uint64_t t0, dt;
            uint32_t i;
            uint8_t  cs = 0;
            char     gb[24], ms[24];

            if (chunk > blk) chunk = blk;

            t0 = ticks_us();
            r = pread(fd, buf, (size_t)chunk, (off_t)(off + pos));
            dt = ticks_us() - t0;
            t_all += dt;

            if (r < 0) {
                fmt_gb(off + pos, gb, sizeof gb);
                printf("E %s G          (errno %ld)\n", gb, (long)errno);
                fflush(stdout);
                eio++;
                continue;
            }
            if (r == 0)
                break;                   /* край носителя раньше данных */

            for (i = 0; i < (uint32_t)r; i++)      /* анти-оптимизатор */
                cs ^= buf[i];
            (void)cs;

            fmt_gb(off + pos, gb, sizeof gb);
            fmt_ms(dt, ms, sizeof ms);
            printf("L %s G %5.2f M %8s %6.1f MB/s\n",
                   gb, (double)r / 1048576.0, ms,
                   (double)r / ((double)dt / 1e6) / 1048576.0);
            fflush(stdout);              /* живой человекочитаемый поток */

            points++;
            bytes_ok += (uint64_t)r;
        }
    }

    free(buf);
    close(fd);

    {
        double mbs = t_all > 0
            ? (double)bytes_ok / ((double)t_all / 1e6) / 1048576.0 : 0.0;
        int mins = (int)(time(NULL) - t0wall) / 60;
        int secs = (int)(time(NULL) - t0wall) % 60;
        printf("---  points %" PRIu64 "  avg %.1f MB/s  eio %" PRIu64
               "%s  time %d:%02d\n",
               points, mbs, eio, g_stop ? "  [stopped]" : "", mins, secs);
        fflush(stdout);
    }
    return 0;
}
