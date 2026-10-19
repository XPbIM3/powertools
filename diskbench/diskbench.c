/*
 * diskbench.c - disk linear/random read+write benchmark for Mac OS X 10.4/10.5 (PowerPC)
 *
 * Tests:
 *   1) linear write  (size, then fsync)
 *   2) linear read   (of the data just written)
 *   3) random write  (pwrite at random offsets, then fsync)
 *   4) random read   (pread at random offsets)
 *
 * Chunk files are created next to the working directory by default and
 * removed afterwards (unless -k).
 *
 * Usage: diskbench [options]
 *   -s SIZE   linear test chunk size   (default 512M)
 *   -r SIZE   random test chunk size   (default 16M)
 *   -p PATH   chunk file base path     (default ./diskbench_chunk.bin)
 *   -b SIZE   linear I/O block size    (default 4M)
 *   -B SIZE   random I/O block size    (default 4K)
 *   -c        use OS cache (F_NOCACHE off; less honest, faster numbers)
 *   -k        keep chunk files after the run
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
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/sysctl.h>

#define DEFAULT_PATH    "./diskbench_chunk.bin"
#define DEFAULT_LIN     (512ULL << 20)   /* 512 MB */
#define DEFAULT_RND     (16ULL  << 20)   /* 16 MB  */
#define DEFAULT_LIN_BLK (4ULL   << 20)   /* 4 MB   */
#define DEFAULT_RND_BLK 4096ULL          /* 4 KiB  */

/* ------------------------------------------------------------------ util */

static void die(const char *what, const char *path)
{
    fprintf(stderr, "diskbench: %s (%s): %s\n",
            what, path ? path : "-", strerror(errno));
    exit(1);
}

static double now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000000.0 + (double)tv.tv_usec;
}

static uint64_t parse_size(const char *s)
{
    char *end;
    unsigned long long val;

    errno = 0;
    val = strtoull(s, &end, 0);
    if (end == s || errno != 0) {
        fprintf(stderr, "diskbench: bad size '%s'\n", s);
        exit(1);
    }
    if (*end) {
        switch (tolower((unsigned char)*end)) {
        case 'k': val *= 1024ULL;          end++; break;
        case 'm': val *= 1024ULL * 1024;   end++; break;
        case 'g': val *= 1024ULL * 1048576;end++; break;
        default:
            fprintf(stderr, "diskbench: bad size suffix in '%s' "
                            "(use K/M/G or plain bytes)\n", s);
            exit(1);
        }
    }
    if (*end) {
        fprintf(stderr, "diskbench: trailing garbage in size '%s'\n", s);
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

/* --------------------------------------------------------- random engine */

static uint64_t rng_state;

static void rng_seed(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    rng_state = ((uint64_t)tv.tv_sec << 32) ^
                (uint64_t)tv.tv_usec ^
                ((uint64_t)getpid() << 16) ^ 0x9E3779B97F4A7C15ULL;
    if (rng_state == 0)
        rng_state = 88172645463325252ULL;
}

static uint64_t rng_next(void)
{
    uint64_t x = rng_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    rng_state = x;
    return x * 2685821657736338717ULL;
}

/* ------------------------------------------------------------- I/O bits */

static void set_nocache(int fd, const char *path, int for_write)
{
#ifdef F_NOCACHE
    if (fcntl(fd, F_NOCACHE, 1) < 0)
        die("fcntl(F_NOCACHE)", path);
#endif
#ifdef F_RDAHEAD
    if (!for_write) {
        if (fcntl(fd, F_RDAHEAD, 0) < 0)   /* kill readahead for random reads */
            die("fcntl(F_RDAHEAD)", path);
    }
#endif
}

static void full_sync(int fd, const char *path)
{
#ifdef F_FULLFSYNC
    if (fcntl(fd, F_FULLFSYNC) == 0)
        return;
#endif
    if (fsync(fd) < 0)
        die("fsync", path);
}

static int write_all(int fd, const void *buf, size_t n, const char *path)
{
    const char *p = (const char *)buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            die("write", path);
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

/* ---------------------------------------------------------------- phases */

static void report(const char *name, uint64_t bytes, double el_us, uint64_t ops)
{
    double secs = el_us / 1000000.0;
    double mbs  = (double)bytes / secs / 1048576.0;

    if (ops > 0)
        printf("%-14s %10.3f s  %8.1f MB/s  %10.0f ops/s  avg %7.1f us/op\n",
               name, secs, mbs, (double)ops / secs, el_us / (double)ops);
    else
        printf("%-14s %10.3f s  %8.1f MB/s\n", name, secs, mbs);
}

static void linear_write(const char *path, uint64_t size, uint64_t blk,
                         int nocache)
{
    uint64_t pos = 0;
    char    *buf;
    int      fd;
    double   t0;

    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) die("open", path);
    if (ftruncate(fd, (off_t)size) < 0) die("ftruncate", path);
    if (nocache) set_nocache(fd, path, 1);

    buf = (char *)malloc((size_t)blk);
    if (!buf) die("malloc", path);
    memset(buf, 0xA5, (size_t)blk);

    t0 = now_us();
    while (pos < size) {
        uint64_t chunk = size - pos;
        if (chunk > blk) chunk = blk;
        write_all(fd, buf, (size_t)chunk, path);
        pos += chunk;
    }
    full_sync(fd, path);
    report("linear write", size, now_us() - t0, 0);

    free(buf);
    close(fd);
}

static void linear_read(const char *path, uint64_t size, uint64_t blk,
                        int nocache)
{
    uint64_t got = 0;
    char    *buf;
    int      fd;
    double   t0;
    uint8_t  cs = 0;

    fd = open(path, O_RDONLY);
    if (fd < 0) die("open", path);
    if (nocache) set_nocache(fd, path, 0);

    buf = (char *)malloc((size_t)blk);
    if (!buf) die("malloc", path);

    t0 = now_us();
    for (;;) {
        uint64_t chunk = size - got;
        if (chunk > blk) chunk = blk;
        if (chunk == 0) break;
        {
            ssize_t r = read(fd, buf, (size_t)chunk);
            if (r < 0) {
                if (errno == EINTR) continue;
                die("read", path);
            }
            if (r == 0) break;
            {
                ssize_t i;
                for (i = 0; i < r; i++) cs ^= (uint8_t)buf[i];
            }
            got += (uint64_t)r;
        }
    }
    report("linear read", got, now_us() - t0, 0);
    if (got != size)
        printf("      note: expected %" PRIu64 " bytes, got %" PRIu64 "\n",
               size, got);
    printf("      (checksum, discardable: 0x%02x)\n", cs);

    free(buf);
    close(fd);
}

static void random_write(const char *path, uint64_t size, uint64_t blk,
                         int nocache)
{
    uint64_t nblk  = size / blk;
    uint64_t i;
    char    *buf;
    int      fd;
    double   t0;

    if (nblk == 0) {
        fprintf(stderr, "diskbench: random block larger than chunk\n");
        exit(1);
    }
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) die("open", path);
    if (ftruncate(fd, (off_t)size) < 0) die("ftruncate", path);
    if (nocache) set_nocache(fd, path, 1);

    buf = (char *)malloc((size_t)blk);
    if (!buf) die("malloc", path);
    memset(buf, 0x5A, (size_t)blk);

    rng_seed();
    t0 = now_us();
    for (i = 0; i < nblk; i++) {
        uint64_t off = (rng_next() % nblk) * blk;
        {
            ssize_t w = pwrite(fd, buf, (size_t)blk, (off_t)off);
            if (w < 0) die("pwrite", path);
        }
    }
    full_sync(fd, path);
    report("random write", nblk * blk, now_us() - t0, nblk);

    free(buf);
    close(fd);
}

static void random_read(const char *path, uint64_t size, uint64_t blk,
                        int nocache)
{
    uint64_t nblk = size / blk;
    uint64_t i;
    char    *buf;
    int      fd;
    double   t0;

    if (nblk == 0) {
        fprintf(stderr, "diskbench: random block larger than chunk\n");
        exit(1);
    }
    fd = open(path, O_RDONLY);
    if (fd < 0) die("open", path);
    if (nocache) set_nocache(fd, path, 0);

    buf = (char *)malloc((size_t)blk);
    if (!buf) die("malloc", path);

    rng_seed();   /* same sequence as the write phase */
    t0 = now_us();
    for (i = 0; i < nblk; i++) {
        uint64_t off = (rng_next() % nblk) * blk;
        {
            ssize_t r = pread(fd, buf, (size_t)blk, (off_t)off);
            if (r < 0) die("pread", path);
            if (r == 0) break;
        }
    }
    report("random read", (nblk - i <= nblk) ? i * blk : nblk * blk,
           now_us() - t0, i);

    free(buf);
    close(fd);
}

/* ------------------------------------------------------------- main/opts */

static void usage(void)
{
    char s1[32], s2[32], s3[32], s4[32];
    fmt_size(DEFAULT_LIN, s1, sizeof s1);
    fmt_size(DEFAULT_RND, s2, sizeof s2);
    fmt_size(DEFAULT_LIN_BLK, s3, sizeof s3);
    fmt_size(DEFAULT_RND_BLK, s4, sizeof s4);
    printf("diskbench - disk I/O benchmark for Mac OS X 10.4/10.5 (PPC)\n\n"
           "usage: diskbench [options]\n"
           "  -s SIZE   linear chunk size     (default %s)\n"
           "  -r SIZE   random  chunk size    (default %s)\n"
           "  -p PATH   chunk file base path  (default %s)\n"
           "  -b SIZE   linear I/O block   (default %s)\n"
           "  -B SIZE   random I/O block   (default %s)\n"
           "  -c        use OS cache (F_NOCACHE off; less honest, faster numbers)\n"
           "  -k        keep chunk files after the run\n"
           "  -h        this help\n"
           "Size suffixes: K, M, G, or plain bytes.\n"
           "Default mode: no OS cache (F_NOCACHE); pass -c to allow caching.\n",
           s1, s2, DEFAULT_PATH, s3, s4);
}

int main(int argc, char **argv)
{
    uint64_t  lin_size = DEFAULT_LIN;
    uint64_t  rnd_size = DEFAULT_RND;
    uint64_t  lin_blk  = DEFAULT_LIN_BLK;
    uint64_t  rnd_blk  = DEFAULT_RND_BLK;
    int       nocache  = 1;   /* default: bypass OS cache */
    int       keep     = 0;
    const    char *base = DEFAULT_PATH;
    char      lin_path[1024], rnd_path[1024], szs1[32], szs2[32], szs3[32], szs4[32];
    int       ch;

    while ((ch = getopt(argc, argv, "p:s:r:b:B:ckh")) != -1) {
        switch (ch) {
        case 'p': base    = optarg;      break;
        case 's': lin_size = parse_size(optarg); break;
        case 'r': rnd_size = parse_size(optarg); break;
        case 'b': lin_blk  = parse_size(optarg); break;
        case 'B': rnd_blk  = parse_size(optarg); break;
        case 'c': nocache = 0; break;
        case 'k': keep    = 1; break;
        case 'h': usage(); return 0;
        default:  usage(); return 1;
        }
    }

    snprintf(lin_path, sizeof lin_path, "%s", base);
    snprintf(rnd_path, sizeof rnd_path, "%s.rand", base);

    fmt_size(lin_size, szs1, sizeof szs1);
    fmt_size(rnd_size, szs2, sizeof szs2);
    fmt_size(lin_blk,  szs3, sizeof szs3);
    fmt_size(rnd_blk,  szs4, sizeof szs4);

    /* free space sanity check */
    {
        char dir[1024];
        struct statfs st;
        const char *slash = strrchr(base, '/');
        snprintf(dir, sizeof dir, "%.*s",
                 slash ? (int)(slash - base) : 1, slash ? base : ".");
        if (statfs(dir, &st) == 0) {
            double freemb = (double)st.f_bavail * (double)st.f_bsize / 1048576.0;
            double needmb = (double)(lin_size + rnd_size) / 1048576.0;
            printf("diskbench (Mac OS X 10.4/10.5, PowerPC)\n");
            printf("  linear chunk: %s  random chunk: %s\n", szs1, szs2);
            printf("  linear block: %s  random block: %s\n", szs3, szs4);
            printf("  chunk path:   %s (+.rand)\n", lin_path);
            printf("  free space:   %.0f MB (need ~%.0f MB) [%s]\n",
                   freemb, needmb, nocache ? "nocache" : "os cache");
            if (freemb < needmb) {
                fprintf(stderr, "diskbench: not enough free space\n");
                return 1;
            }
        }
    }

    linear_write(lin_path, lin_size, lin_blk, nocache);
    linear_read(lin_path, lin_size, lin_blk, nocache);
    random_write(rnd_path, rnd_size, rnd_blk, nocache);
    random_read(rnd_path, rnd_size, rnd_blk, nocache);

    if (!keep) {
        unlink(lin_path);
        unlink(rnd_path);
        printf("  (chunk files removed; use -k to keep them)\n");
    }
    return 0;
}
