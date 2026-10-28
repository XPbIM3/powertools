/*
 * dbrnd.c - консольная обёртка над dgrndcore (random по времени)
 *
 * Вся логика замера - в dgrndcore.c (её же зовёт UI). Здесь только
 * разбор аргументов, человекочитаемый stdout и сводка.
 *
 *   dbrnd 1.0  dev /dev/rdisk0  capacity 55.90 G
 *     block 4.00 K  10 s  warmup 16  seed 12345
 *   R    0.12 G  14.32 ms
 *   ...
 *   ---  ops 700  med 14.30 ms  p99 32.10 ms  avg 15.20 ms  eio 0  time 0:10
 *
 * Только ЧТЕНИЕ. Запись не выполняется никогда.
 */
#include "dgrndcore.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <signal.h>
#include <time.h>

#define DEFAULT_DEV     "/dev/rdisk1"
#define DEFAULT_RND_BLK 4096ULL
#define DEFAULT_SECS    10
#define DEFAULT_WARMUP  16

static volatile int g_stop = 0;
static time_t       g_t0   = 0;

static void on_sig(int s)
{
    (void)s;
    g_stop = 1;
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
        case 'k': val *= 1024ULL;              end++; break;
        case 'm': val *= 1024ULL * 1024;       end++; break;
        case 'g': val *= 1024ULL * 1048576;    end++; break;
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
{
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

/* ------------------------------------------------------- stdout-sink */

typedef struct {
    const char *dev;
    uint64_t    blk;
    int         secs;
    int         warmup;
} outctx;

static void out_begin(void *ctx, const dgrndinfo *info)
{
    outctx *c = (outctx *)ctx;

    g_t0 = time(NULL);
    printf("dbrnd 1.0  dev %s  capacity %.2f G\n",
           c->dev, (double)info->capacity / 1073741824.0);
    printf("  block %.2f K  %d s  warmup %d  seed %" PRIu64 "\n",
           (double)c->blk / 1024.0, c->secs, c->warmup, info->seed);
    fflush(stdout);
}

static void out_op(void *ctx, const dgrndop *op)
{
    char gb[24], ms[24];

    (void)ctx;
    fmt_gb(op->off, gb, sizeof gb);
    fmt_ms(op->us, ms, sizeof ms);
    printf("R %s G %8s\n", gb, ms);
    fflush(stdout);
}

static void out_err(void *ctx, const dgrnderr *e)
{
    char gb[24], ms[24];

    (void)ctx;
    fmt_gb(e->off, gb, sizeof gb);
    fmt_ms(e->us, ms, sizeof ms);
    printf("E %s G %8s  (errno %ld)\n", gb, ms, e->err);
    fflush(stdout);
}

/* --------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    dgrndcfg    cfg;
    dgrndresult res;
    outctx      out;
    uint64_t    rblk   = DEFAULT_RND_BLK;
    uint64_t    seed   = (uint64_t)time(NULL) ^ (uint64_t)getpid();
    int         secs   = DEFAULT_SECS;
    int         warmup = DEFAULT_WARMUP;
    int         ch, rc;
    char        err[256];
    const char *dev = DEFAULT_DEV;
    struct sigaction sa;

    while ((ch = getopt(argc, argv, "d:B:T:w:s:h")) != -1) {
        switch (ch) {
        case 'd': dev    = optarg;   break;
        case 'B': rblk   = parse_size(optarg); break;
        case 'T': secs   = atoi(optarg);   break;
        case 'w': warmup = atoi(optarg);   break;
        case 's': seed   = parse_size(optarg); break;
        case 'h':
            printf("dbrnd 1.0 - timed random read test "
                   "(Mac OS X 10.2..10.5 PPC)\n\n"
                   "usage: sudo dbrnd -d /dev/rdiskN [options]\n"
                   "  -B SIZE   random block       default 4K\n"
                   "  -T N      run seconds        default 10\n"
                   "  -w N      warmup ops         default 16 "
                   "(not recorded)\n"
                   "  -s N      rng seed           default clock\n"
                   "  -h        help\n");
            return 0;
        default:
            return 1;
        }
    }

    rblk = rblk - (rblk % 512ULL);
    if (rblk == 0)
        rblk = 512ULL;
    if (secs < 1) {
        fprintf(stderr, "dbrnd: run seconds must be at least 1\n");
        return 1;
    }
    if (strncmp(dev, "/dev/rdisk", 10) != 0)
        fprintf(stderr, "dbrnd: WARNING: %s is not a raw node - "
                        "use /dev/rdiskN for honest latency\n", dev);

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sig;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    memset(&cfg, 0, sizeof cfg);
    cfg.dev    = dev;
    cfg.blk    = rblk;
    cfg.secs   = secs;
    cfg.warmup = warmup;
    cfg.seed   = seed;

    out.dev    = dev;
    out.blk    = rblk;
    out.secs   = secs;
    out.warmup = warmup;

    rc = dgrndcore(&cfg, out_begin, out_op, out_err, &out,
                   &g_stop, &res, err, sizeof err);
    if (rc != 0) {
        fprintf(stderr, "dbrnd: %s\n", err);
        return 1;
    }

    {
        char m1[24], m2[24], m3[24];
        int  mins = (int)(time(NULL) - g_t0) / 60;
        int  s    = (int)(time(NULL) - g_t0) % 60;

        fmt_ms((uint64_t)(res.med_us + 0.5), m1, sizeof m1);
        fmt_ms((uint64_t)(res.p99_us + 0.5), m2, sizeof m2);
        fmt_ms((uint64_t)(res.avg_us + 0.5), m3, sizeof m3);
        printf("---  ops %" PRIu64 "  med %s  p99 %s  avg %s  eio %"
               PRIu64 "%s  time %d:%02d\n",
               res.ops, m1, m2, m3, res.eio,
               res.stopped ? "  [stopped]" : "", mins, s);
        fflush(stdout);
    }
    return 0;
}
