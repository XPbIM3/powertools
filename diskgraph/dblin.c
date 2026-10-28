/*
 * dblin.c - консольная обёртка над dglincore (линейный пунктир)
 *
 * Вся логика замера - в dglincore.c (её же зовёт UI). Здесь только
 * разбор аргументов, человекочитаемый stdout и сводка.
 *
 *   dblin 1.0  dev /dev/rdisk0  capacity 55.90 G
 *     chunk 32.00 M  block 1.00 M  duty 10%  dashed
 *   C    0.00 G  32.00 M   32.7 MB/s  blk 32
 *   ...
 *   ---  chunks 7  blocks 224  avg 32.8 MB/s  eio 0  time 0:07
 *
 * Только ЧТЕНИЕ. Запись не выполняется никогда.
 */
#include "dglincore.h"
#include "dgdev.h"

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

#define DEFAULT_DEV   "/dev/rdisk1"
#define DEFAULT_SEG   (32ULL << 20)
#define DEFAULT_BLK   (1ULL  << 20)
#define DEFAULT_DUTY  10
#define SECT          512ULL

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
        fprintf(stderr, "dblin: bad size '%s'\n", s);
        exit(1);
    }
    if (*end) {
        switch (tolower((unsigned char)*end)) {
        case 'k': val *= 1024ULL;              end++; break;
        case 'm': val *= 1024ULL * 1024;       end++; break;
        case 'g': val *= 1024ULL * 1048576;    end++; break;
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
{
    snprintf(out, outsz, "%7.2f", (double)off / 1073741824.0);
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

/* ------------------------------------------------------- stdout-sink */

typedef struct {
    const char *dev;
    uint64_t    seg;
    uint64_t    blk;
    int         duty;
} outctx;

static void out_begin(void *ctx, const dglininfo *info)
{
    outctx *c = (outctx *)ctx;

    g_t0 = time(NULL);
    printf("dblin 1.0  dev %s  capacity %.2f G\n",
           c->dev, (double)info->capacity / 1073741824.0);
    printf("  chunk %.2f M  block %.2f M  duty %d%%  %s\n",
           (double)c->seg / 1048576.0,
           (double)c->blk / 1048576.0,
           c->duty, info->skip == 0 ? "solid" : "dashed");
    fflush(stdout);
}

static void out_seg(void *ctx, const dglinseg *s)
{
    char gb[24], mb[24];

    (void)ctx;
    fmt_gb(s->off, gb, sizeof gb);
    snprintf(mb, sizeof mb, "%6.2f", (double)s->bytes / 1048576.0);

    printf("C %s G %s M %6.1f MB/s  blk %" PRIu64,
           gb, mb,
           s->time_us > 0
               ? (double)s->bytes / ((double)s->time_us / 1e6) / 1048576.0
               : 0.0,
           s->blocks);
    if (s->eio > 0)
        printf("  eio %" PRIu64, s->eio);
    printf("\n");
    fflush(stdout);
}

static void out_err(void *ctx, const dglinerr *e)
{
    char gb[24];

    (void)ctx;
    fmt_gb(e->off, gb, sizeof gb);
    printf("E %s G          (errno %ld)\n", gb, e->err);
    fflush(stdout);
}

/* --------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    dglincfg    cfg;
    dglinresult res;
    outctx      out;
    uint64_t    seg = DEFAULT_SEG;
    uint64_t    blk = DEFAULT_BLK;
    uint64_t    maxoff = 0;
    int         duty = DEFAULT_DUTY;
    int         ch, rc;
    char        err[256];
    const char *dev = DEFAULT_DEV;
    struct sigaction sa;

    while ((ch = getopt(argc, argv, "d:c:b:D:m:h")) != -1) {
        switch (ch) {
        case 'd': dev    = optarg; break;
        case 'c': seg    = parse_size(optarg); break;
        case 'b': blk    = parse_size(optarg); break;
        case 'D': duty   = atoi(optarg);   break;
        case 'm': maxoff = parse_size(optarg); break;
        case 'h':
            printf("dblin 1.0 - linear dashed read sweep "
                   "(Mac OS X 10.2..10.5 PPC)\n\n"
                   "usage: sudo dblin -d /dev/rdiskN [options]\n"
                   "  -c SIZE   chunk (segment) size  default 32M, "
                   "multiple of -b\n"
                   "  -b SIZE   read block            default 1M, "
                   "power of two\n"
                   "  -D N      duty percent 10..100  default 10 "
                   "(100 = solid)\n"
                   "  -m SIZE   sweep limit           default 0 = "
                   "whole capacity\n"
                   "  -h        help\n");
            return 0;
        default:
            return 1;
        }
    }

    seg = align_sect(seg, "chunk");
    blk = align_sect(blk, "block");
    if (seg < blk) {
        fprintf(stderr, "dblin: chunk smaller than block\n");
        return 1;
    }
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

    if (strncmp(dev, "/dev/rdisk", 10) != 0)
        fprintf(stderr, "dblin: WARNING: %s is not a raw node - "
                        "use /dev/rdiskN for honest latency\n", dev);

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sig;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    memset(&cfg, 0, sizeof cfg);
    cfg.dev    = dev;
    cfg.seg    = seg;
    cfg.blk    = blk;
    cfg.duty   = duty;
    cfg.extent = maxoff;

    out.dev  = dev;
    out.seg  = seg;
    out.blk  = blk;
    out.duty = duty;

    rc = dglincore(&cfg, out_begin, out_seg, out_err, &out,
                   &g_stop, &res, err, sizeof err);
    if (rc != 0) {
        fprintf(stderr, "dblin: %s%s\n", err,
                dg_is_perm_err(res.oserr)
                    ? " (raw nodes are root:operator; run under sudo; "
                      "use /dev/rdiskN)"
                    : "");
        return 1;
    }

    {
        double mbs = res.time_us > 0
            ? (double)res.bytes / ((double)res.time_us / 1e6) / 1048576.0
            : 0.0;
        int mins = (int)(time(NULL) - g_t0) / 60;
        int secs = (int)(time(NULL) - g_t0) % 60;

        printf("---  chunks %" PRIu64 "  blocks %" PRIu64
               "  avg %.1f MB/s  eio %" PRIu64 "%s  time %d:%02d\n",
               res.chunks, res.blocks, mbs, res.eio,
               res.stopped ? "  [stopped]" : "", mins, secs);
        fflush(stdout);
    }
    return 0;
}
