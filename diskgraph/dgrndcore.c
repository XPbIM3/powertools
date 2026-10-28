/*
 * dgrndcore.c - random-чтение по времени как ФУНКЦИЯ
 *
 * Вынуто из dbrnd.c без изменения логики: xorshift-позиция uniform по
 * всей ёмкости, лаг каждого опа по mach-времени, XOR-чексум, прогрев
 * в данные не идёт, медиана/p99 считаются по накопленным лагам.
 */
#include "dgrndcore.h"
#include "dgdev.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <math.h>

#define SECT 512ULL

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

/* равномерно [0,1) из 53 старших бит */
static double rng_unit(void)
{
    return (double)(rng_next() >> 11) * (1.0 / 9007199254740992.0);
}

/*
 * Следующая позиция цепочки хопов: d log-равномерно [dmin, dmax],
 * направление случайное; не влезает в сторону - в другую; не влезает
 * никуда - хоп до дальнего края. Всё в слотах по 512 байт.
 */
static uint64_t seek_next(uint64_t cur, uint64_t nslots, uint64_t dmin)
{
    uint64_t last = nslots - 1;
    uint64_t dmax = last;
    uint64_t d;
    int      fwd;

    if (dmax <= dmin)
        return (cur == 0) ? last : 0;
    d = (uint64_t)((double)dmin * pow((double)dmax / (double)dmin,
                                      rng_unit()));
    if (d < dmin) d = dmin;
    if (d > dmax) d = dmax;

    fwd = (int)(rng_next() >> 63);
    if (fwd && d > last - cur)
        fwd = 0;
    else if (!fwd && d > cur)
        fwd = 1;
    if (fwd ? d > last - cur : d > cur)   /* не влез ни туда, ни туда */
        return (cur > last - cur) ? 0 : last;
    return fwd ? cur + d : cur - d;
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
    uint64_t rblk, cap, nslots, off, seed, prev = 0, dmin;
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

    dg_clock_init();

    fd = dg_open_raw(cfg->dev, &cap, &res->oserr, errbuf, errlen);
    if (fd < 0)
        return -1;

    if (rblk > cap) {
        snprintf(errbuf, errlen, "block bigger than the device");
        goto out;
    }
    nslots = (cap - rblk) / SECT + 1;
    /* короче 8K (и не короче двух блоков) хоп - почти линейное чтение */
    dmin = (rblk * 2 > 8192 ? rblk * 2 : 8192) / SECT;

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
            prev = wo;
        }
    }

    while (cancel == NULL || !*cancel) {
        uint64_t t0, dt;
        ssize_t  r;
        uint32_t i;
        uint8_t  cs = 0;

        if (time(NULL) - t0wall >= (time_t)cfg->secs)
            break;

        if (cfg->mode == DGRND_SEEK)
            off = seek_next(prev / SECT, nslots, dmin) * SECT;
        else
            off = (rng_next() % nslots) * SECT;

        t0 = dg_now_us();
        r = pread(fd, buf, (size_t)rblk, (off_t)off);
        dt = dg_now_us() - t0;
        t_io += dt;

        if (r < 0) {
            dgrnderr e;
            e.off = off;
            e.us  = dt;
            e.err = (long)errno;
            if (on_err != NULL)
                on_err(ctx, &e);
            eio++;
            prev = off;
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
            op.dist  = (off >= prev) ? off - prev : prev - off;
            op.dir   = (off >= prev) ? 1 : -1;
            on_op(ctx, &op);
        }
        prev = off;

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

    qsort(lat, ln, sizeof(double), dg_dcmp);

    res->ops     = ops;
    res->eio     = eio;
    res->time_us = t_io;
    res->med_us  = dg_pct(lat, ln, 0.50);
    res->p99_us  = dg_pct(lat, ln, 0.99);
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
