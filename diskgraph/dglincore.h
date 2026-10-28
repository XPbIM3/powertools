/*
 * dglincore.h - линейный пунктирный проход по диску как ФУНКЦИЯ
 *
 * Один и тот же код замера для консольного dblin и для UI:
 *   - dblin.c   : sink печатает человекочитаемые строки в stdout;
 *   - diskgraph : sink кладёт точки в массивы графика.
 * Никакого stdout внутри ядра нет - только калбеки.
 *
 * Честность-канон (F_NOCACHE, readahead off, XOR, mach-время,
 * 512-выравнивание, duty-квантование, спин-ап прогрев) живёт здесь.
 *
 * Чтение только. Запись в устройство не выполняется никогда.
 */
#ifndef DGLINCORE_H
#define DGLINCORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *dev;     /* "/dev/rdiskN" (raw)                 */
    uint64_t    seg;     /* чанк (сегмент), байты               */
    uint64_t    blk;     /* блок чтения, байты                  */
    int         duty;    /* 1..100 %, 100 = сплошной проход     */
    uint64_t    extent;  /* 0 = вся ёмкость                     */
} dglincfg;

typedef struct {
    uint64_t capacity;   /* ёмкость устройства, байты           */
    uint64_t extent;     /* сколько реально пройдём, байты      */
    uint64_t skip;       /* средний пропуск между чанками, байты */
    uint64_t chunks;     /* сколько сегментов будет в проходе   */
} dglininfo;

typedef struct {
    uint64_t index;      /* номер сегмента, с 0                 */
    uint64_t off;        /* старт сегмента, байты               */
    uint64_t len;        /* длина сегмента по плану, байты      */
    uint64_t bytes;      /* прочитано успешно, байты            */
    uint64_t time_us;    /* время сегмента (сумма блоков), µs   */
    uint64_t blocks;     /* успешных блоков                     */
    uint64_t eio;        /* ошибок в сегменте                   */
} dglinseg;

typedef struct {
    uint64_t off;        /* где была ошибка                     */
    long     err;        /* errno                               */
} dglinerr;

typedef struct {
    uint64_t chunks;
    uint64_t blocks;
    uint64_t bytes;
    uint64_t eio;
    uint64_t time_us;
    int      stopped;    /* 1 = прервано cancel-флагом          */
    int      oserr;      /* errno фатальной ошибки (0 - нет)    */
} dglinresult;

/* on_begin зовётся один раз после опроса геометрии, до прогрева */
typedef void (*dglin_begin_fn)(void *ctx, const dglininfo *info);
typedef void (*dglin_seg_fn)(void *ctx, const dglinseg *seg);
typedef void (*dglin_err_fn)(void *ctx, const dglinerr *err);

/*
 * Возврат: 0 - проход выполнен (в т.ч. прерванный), -1 - ошибка
 * (текст в errbuf). Калбеки вызываются из потока-вызывателя.
 */
int dglincore(const dglincfg *cfg,
              dglin_begin_fn on_begin,
              dglin_seg_fn on_seg,
              dglin_err_fn on_err,
              void *ctx,
              volatile int *cancel,
              dglinresult *res,
              char *errbuf, size_t errlen);

#ifdef __cplusplus
}
#endif

#endif /* DGLINCORE_H */
