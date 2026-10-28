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
#include "dgdev.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#define SECT 512ULL

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
    uint64_t seg, blk, cap, maxoff, skip, off;
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
    if (cfg->duty < 1 || cfg->duty > 100) {
        snprintf(errbuf, errlen, "duty percent must be 1..100");
        return -1;
    }

    dg_clock_init();

    fd = dg_open_raw(cfg->dev, &cap, &res->oserr, errbuf, errlen);
    if (fd < 0)
        return -1;

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

            t0 = dg_now_us();
            r = pread(fd, buf, (size_t)chunk, (off_t)(off + pos));
            dt = dg_now_us() - t0;
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
