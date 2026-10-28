/*
 * dglincore.c - линейный пунктирный проход по диску как ФУНКЦИЯ
 *
 * Вынуто из dblin.c без изменения логики: сегменты стартуют
 * эквидистантно от LBA0, шаг = seg*(100-duty)/duty, внутри сегмента
 * блоки таймятся по отдельности и XOR-ятся (анти-оптимизатор).
 * Наружу - калбеки: begin (геометрия/план), seg (итог сегмента),
 * err (ошибка блока). Никакого stdout.
 */
#include "dglincore.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/disk.h>
#include <mach/mach_time.h>

#define SECT 512ULL

/* ------------------------------------------------------------ clock */
static double g_ns_per_tick = 0.0;

static void clock_init(void)
{
    mach_timebase_info_data_t tb;

    if (g_ns_per_tick > 0.0)
        return;                       /* гонка безобидна: число то же */
    mach_timebase_info(&tb);
    g_ns_per_tick = (double)tb.numer / (double)tb.denom;
}

static uint64_t ticks_us(void)
{
    return (uint64_t)((double)mach_absolute_time() * g_ns_per_tick / 1000.0);
}

/* ------------------------------------------------------------ core */

int dglincore(const dglincfg *cfg,
              dglin_begin_fn on_begin,
              dglin_seg_fn on_seg,
              dglin_err_fn on_err,
              void *ctx,
              volatile int *cancel,
              dglinresult *res,
              char *errbuf, size_t errlen)
{
    int      fd = -1;
    uint64_t seg, blk, cap, maxoff, skip, off, t;
    uint32_t bs = 512;
    char    *buf = NULL;
    int      rc = -1;
    dglininfo info;

    if (errbuf != NULL && errlen > 0)
        errbuf[0] = 0;
    if (cfg == NULL || res == NULL) {
        if (errbuf != NULL && errlen > 0)
            snprintf(errbuf, errlen, "null argument");
        return -1;
    }
    memset(res, 0, sizeof *res);
    memset(&info, 0, sizeof info);

    seg = cfg->seg - (cfg->seg % SECT);
    blk = cfg->blk - (cfg->blk % SECT);

    if (blk == 0 || seg < blk || seg % blk != 0) {
        snprintf(errbuf, errlen,
                 "bad geometry: chunk %llu, block %llu "
                 "(chunk must be a multiple of block)",
                 (unsigned long long)seg, (unsigned long long)blk);
        return -1;
    }
    if (cfg->duty < 10 || cfg->duty > 100) {
        snprintf(errbuf, errlen, "duty percent must be 10..100");
        return -1;
    }

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
    if (ioctl(fd, DKIOCGETBLOCKCOUNT, &t) < 0) {
        snprintf(errbuf, errlen, "ioctl(DKIOCGETBLOCKCOUNT) on %s: %s",
                 cfg->dev, strerror(errno));
        goto out;
    }

    cap = t * (uint64_t)bs;
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

    maxoff = (cfg->extent == 0 || cfg->extent > cap) ? cap : cfg->extent;
    maxoff -= maxoff % SECT;

    skip = seg * (uint64_t)(100 - cfg->duty) / (uint64_t)cfg->duty;
    skip -= skip % SECT;              /* EINVAL on rdisk otherwise */

    info.capacity = cap;
    info.extent   = maxoff;
    info.skip     = skip;

    {
        uint64_t o, n = 0;
        for (o = 0; o < maxoff; o += seg + skip) {
            uint64_t seglen = seg;
            if (o + seglen > maxoff)
                seglen = maxoff - o;
            if (seglen < blk)
                break;
            n++;
        }
        info.chunks = n;
    }

    if (on_begin != NULL)
        on_begin(ctx, &info);

    buf = (char *)malloc((size_t)blk);
    if (buf == NULL) {
        snprintf(errbuf, errlen, "malloc(%llu) failed",
                 (unsigned long long)blk);
        goto out;
    }

    /* прогрев спин-апа: два блока, тайминги выбрасываются */
    {
        uint64_t w;
        for (w = 0; w < 2; w++)
            if (pread(fd, buf, (size_t)blk, (off_t)0) < 0)
                break;
    }

    for (off = 0; off < maxoff; off += seg + skip) {
        uint64_t seglen = seg;
        uint64_t pos;
        uint64_t c_bytes = 0, c_time = 0, c_blocks = 0, c_eio = 0;
        int      media_end = 0;

        if (cancel != NULL && *cancel)
            break;

        if (off + seglen > maxoff)
            seglen = maxoff - off;
        if (seglen < blk)
            break;                    /* хвост < блока - нечего мерить */

        for (pos = 0; pos < seglen; pos += blk) {
            uint64_t chunk = seglen - pos;
            ssize_t  r;
            uint64_t t0, dt;
            uint32_t i;
            uint8_t  cs = 0;

            if (cancel != NULL && *cancel)
                break;
            if (chunk > blk)
                chunk = blk;

            t0 = ticks_us();
            r = pread(fd, buf, (size_t)chunk, (off_t)(off + pos));
            dt = ticks_us() - t0;
            res->time_us += dt;

            if (r < 0) {
                dglinerr e;
                e.off = off + pos;
                e.err = (long)errno;
                if (on_err != NULL)
                    on_err(ctx, &e);
                res->eio++;
                c_eio++;
                continue;
            }
            if (r == 0) {
                media_end = 1;        /* край носителя */
                break;
            }

            for (i = 0; i < (uint32_t)r; i++)   /* анти-оптимизатор */
                cs ^= buf[i];
            (void)cs;

            c_time += dt;
            c_bytes += (uint64_t)r;
            c_blocks++;
        }

        if (on_seg != NULL) {
            dglinseg s;
            s.index   = res->chunks;
            s.off     = off;
            s.bytes   = c_bytes;
            s.time_us = c_time;
            s.blocks  = c_blocks;
            s.eio     = c_eio;
            on_seg(ctx, &s);
        }

        res->chunks++;
        res->blocks += c_blocks;
        res->bytes  += c_bytes;

        if (media_end)
            break;
    }

    if (cancel != NULL && *cancel)
        res->stopped = 1;

    rc = 0;

out:
    if (buf != NULL)
        free(buf);
    if (fd >= 0)
        close(fd);
    return rc;
}
