/*
 * dglincore.c - линейный пунктирный проход по диску как ФУНКЦИЯ
 *
 * Раскладка сегментов: duty 100 - сплошной проход; иначе N сегментов
 * равномерно от LBA0 до САМОГО КОНЦА диска (первый с 0, последний
 * кончается на ёмкости), N = round(ёмкость * duty / seg). Раньше шаг
 * seg*100/duty шёл от LBA0 и хвост короче шага не читался - при duty 1%
 * до 3 GB белого в конце графика. Внутри сегмента блоки таймятся по
 * отдельности и XOR-ятся (анти-оптимизатор).
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

/* ------------------------------------------------------------ plan */

typedef struct {
    uint64_t n;          /* сегментов              */
    uint64_t seg;        /* длина сегмента         */
    uint64_t span;       /* maxoff - seg (dashed)  */
    uint64_t maxoff;
    int      solid;
} linplan;

static void plan_make(linplan *p, uint64_t maxoff, uint64_t seg, int duty)
{
    uint64_t nmax;

    memset(p, 0, sizeof *p);
    p->seg    = seg;
    p->maxoff = maxoff;

    if (duty >= 100 || maxoff <= seg) {
        p->solid = 1;
        p->n = (maxoff + seg - 1) / seg;      /* последний - хвост */
        return;
    }
    p->n = (maxoff * (uint64_t)duty / 100 + seg / 2) / seg;
    if (p->n < 2)
        p->n = 2;
    nmax = maxoff / seg;                      /* больше - сегменты слипнутся */
    if (p->n > nmax)
        p->n = nmax;
    p->span = maxoff - seg;
}

/* старт сегмента k: равномерно от 0 до maxoff - seg, по сектору */
static uint64_t plan_off(const linplan *p, uint64_t k)
{
    uint64_t o;

    if (p->solid)
        return k * p->seg;
    if (p->n < 2)
        return 0;
    o = p->span * k / (p->n - 1);
    return o - (o % SECT);                    /* EINVAL on rdisk otherwise */
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
    uint64_t seg, blk, cap, maxoff, off, k;
    linplan  plan;
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

    plan_make(&plan, maxoff, seg, cfg->duty);

    info.capacity = cap;
    info.extent   = maxoff;
    info.skip     = (!plan.solid && plan.n > 1)
                    ? plan.span / (plan.n - 1) - seg : 0;
    info.chunks   = plan.n;
    /* сплошной проход: хвост короче блока не мерим */
    if (plan.solid && plan.n > 0 &&
        maxoff - plan_off(&plan, plan.n - 1) < blk)
        info.chunks--;

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

    for (k = 0; k < info.chunks; k++) {
        uint64_t seglen = seg;
        uint64_t pos;
        uint64_t c_bytes = 0, c_time = 0, c_blocks = 0, c_eio = 0;
        int      media_end = 0;

        if (cancel != NULL && *cancel)
            break;

        off = plan_off(&plan, k);
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
            s.len     = seglen;
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
