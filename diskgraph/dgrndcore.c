/*
 * dgrndcore.c - random-чтение по времени как ФУНКЦИЯ
 *
 * Вынуто из dbrnd.c без изменения логики: xorshift-позиция uniform по
 * всей ёмкости, лаг каждого опа по mach-времени, XOR-чексум, прогрев
 * в данные не идёт, медиана/p99 считаются по накопленным лагам.
 */
#include "dgrndcore.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/disk.h>
#include <mach/mach_time.h>

#define SECT 512ULL

/* ------------------------------------------------------------ clock */
static double g_ns_per_tick = 0.0;

static void clock_init(void)
{
    mach_timebase_info_data_t tb;

    if (g_ns_per_tick > 0.0)
        return;
    mach_timebase_info(&tb);
    g_ns_per_tick = (double)tb.numer / (double)tb.denom;
}

static uint64_t ticks_us(void)
{
    return (uint64_t)((double)mach_absolute_time() * g_ns_per_tick / 1000.0);
}

/* ------------------------------------------------------------ rng */
static uint64_t g_rng;

static void rng_seed(uint64_t s)
{
    g_rng = s * 6364136223846793005ULL + 1442695040888963407ULL;
    if (g_rng == 0)
        g_rng = 88172645463325252ULL;
}

static uint64_t rng_next(void)
{
    uint64_t x = g_rng;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    g_rng = x;
    return x * 2685821657736338717ULL;
}

/* ------------------------------------------------------------ stats */
static int dcmp(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static double pct(const double *v, size_t n, double p)
{
    double idx;
    size_t i;

    if (n == 0)
        return 0.0;
    idx = p * (double)(n - 1);
    i = (size_t)(idx + 0.5);
    if (i >= n)
        i = n - 1;
    return v[i];
}

/* ------------------------------------------------------------ core */

int dgrndcore(const dgrndcfg *cfg,
              dgrnd_begin_fn on_begin,
              dgrnd_op_fn on_op,
              dgrnd_err_fn on_err,
              void *ctx,
              volatile int *cancel,
              dgrndresult *res,
              char *errbuf, size_t errlen)
{
    int      fd = -1;
    uint64_t rblk, cap, nslots, off, seed;
    uint32_t bs = 512;
    char    *buf = NULL;
    double  *lat = NULL;
    size_t   lcap = 1 << 12, ln = 0;
    uint64_t ops = 0, eio = 0, t_io = 0;
    time_t   t0wall;
    int      rc = -1;
    dgrndinfo info;

    if (errbuf != NULL && errlen > 0)
        errbuf[0] = 0;
    if (cfg == NULL || res == NULL) {
        if (errbuf != NULL && errlen > 0)
            snprintf(errbuf, errlen, "null argument");
        return -1;
    }
    memset(res, 0, sizeof *res);
    memset(&info, 0, sizeof info);

    rblk = cfg->blk - (cfg->blk % SECT);
    if (rblk == 0)
        rblk = SECT;
    if (cfg->secs < 1) {
        snprintf(errbuf, errlen, "run seconds must be at least 1");
        return -1;
    }

    seed = cfg->seed;
    if (seed == 0)
        seed = (uint64_t)time(NULL) ^ (uint64_t)getpid();

    clock_init();

    fd = open(cfg->dev, O_RDONLY);
    if (fd < 0) {
        if (errno == EACCES || errno == EPERM)
            snprintf(errbuf, errlen,
                     "cannot open %s: %s (raw nodes are root:operator; "
                     "run under sudo; use /dev/rdiskN)",
                     cfg->dev, strerror(errno));
        else
            snprintf(errbuf, errlen, "cannot open %s: %s",
                     cfg->dev, strerror(errno));
        return -1;
    }

    if (ioctl(fd, DKIOCGETBLOCKSIZE, &bs) < 0)
        bs = 512;
    if (ioctl(fd, DKIOCGETBLOCKCOUNT, &off) < 0) {
        snprintf(errbuf, errlen, "ioctl(DKIOCGETBLOCKCOUNT) on %s: %s",
                 cfg->dev, strerror(errno));
        goto out;
    }

    cap = off * (uint64_t)bs;
    if (cap == 0) {
        snprintf(errbuf, errlen, "zero capacity reported by %s", cfg->dev);
        goto out;
    }

    if (fcntl(fd, F_NOCACHE, 1) < 0) {
        snprintf(errbuf, errlen, "fcntl(F_NOCACHE) on %s: %s",
                 cfg->dev, strerror(errno));
        goto out;
    }
    if (fcntl(fd, F_RDAHEAD, 0) < 0) {
        snprintf(errbuf, errlen, "fcntl(F_RDAHEAD) on %s: %s",
                 cfg->dev, strerror(errno));
        goto out;
    }

    if (rblk > cap) {
        snprintf(errbuf, errlen, "block bigger than the device");
        goto out;
    }
    nslots = (cap - rblk) / SECT + 1;

    info.capacity = cap;
    info.slots    = nslots;
    info.seed     = seed;
    if (on_begin != NULL)
        on_begin(ctx, &info);

    rng_seed(seed);
    t0wall = time(NULL);

    buf = (char *)malloc((size_t)rblk);
    if (buf == NULL) {
        snprintf(errbuf, errlen, "malloc(%llu) failed",
                 (unsigned long long)rblk);
        goto out;
    }
    lat = (double *)malloc(lcap * sizeof(double));
    if (lat == NULL) {
        snprintf(errbuf, errlen, "malloc(stats) failed");
        goto out;
    }

    /* прогрев: считается в eio, но в данные не идёт */
    {
        uint64_t i;
        for (i = 0; i < (uint64_t)cfg->warmup; i++) {
            uint64_t wo;
            if (cancel != NULL && *cancel)
                break;
            wo = (rng_next() % nslots) * SECT;
            if (pread(fd, buf, (size_t)rblk, (off_t)wo) < 0)
                eio++;
        }
    }

    while (cancel == NULL || !*cancel) {
        uint64_t t0, dt;
        ssize_t  r;
        uint32_t i;
        uint8_t  cs = 0;

        if (time(NULL) - t0wall >= (time_t)cfg->secs)
            break;

        off = (rng_next() % nslots) * SECT;

        t0 = ticks_us();
        r = pread(fd, buf, (size_t)rblk, (off_t)off);
        dt = ticks_us() - t0;
        t_io += dt;

        if (r < 0) {
            dgrnderr e;
            e.off = off;
            e.us  = dt;
            e.err = (long)errno;
            if (on_err != NULL)
                on_err(ctx, &e);
            eio++;
            continue;
        }
        if (r == 0)
            break;                    /* край носителя */

        for (i = 0; i < (uint32_t)r; i++)       /* анти-оптимизатор */
            cs ^= buf[i];
        (void)cs;

        if (on_op != NULL) {
            dgrndop op;
            op.index = ops;
            op.off   = off;
            op.us    = dt;
            on_op(ctx, &op);
        }

        if (ln == lcap) {
            double *nl = (double *)realloc(lat, (lcap << 1) * sizeof(double));
            if (nl == NULL) {
                snprintf(errbuf, errlen, "realloc(stats) failed");
                goto out;
            }
            lat = nl;
            lcap <<= 1;
        }
        lat[ln++] = (double)dt;
        ops++;
    }

    qsort(lat, ln, sizeof(double), dcmp);

    res->ops     = ops;
    res->eio     = eio;
    res->time_us = t_io;
    res->med_us  = pct(lat, ln, 0.50);
    res->p99_us  = pct(lat, ln, 0.99);
    res->avg_us  = (ln > 0) ? (double)t_io / (double)ln : 0.0;
    res->stopped = (cancel != NULL && *cancel) ? 1 : 0;

    rc = 0;

out:
    if (lat != NULL)
        free(lat);
    if (buf != NULL)
        free(buf);
    if (fd >= 0)
        close(fd);
    return rc;
}
