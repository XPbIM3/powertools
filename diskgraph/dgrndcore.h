/*
 * dgrndcore.h - random-чтение по времени как ФУНКЦИЯ
 *
 * Один и тот же код замера для консольного dbrnd и для UI:
 * sink кладёт лаг каждого опа в массив графика; dbrnd печатает строки.
 * Никакого stdout внутри ядра.
 *
 * Чтение только. Запись не выполняется никогда.
 */
#ifndef DGRNDCORE_H
#define DGRNDCORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *dev;     /* "/dev/rdiskN"                  */
    uint64_t    blk;     /* размер опа, байты (обычно 4K)  */
    int         secs;    /* длительность прогона, секунды  */
    int         warmup;  /* опов прогрева (не в данные)    */
    uint64_t    seed;    /* 0 = взять из времени/pid       */
} dgrndcfg;

typedef struct {
    uint64_t capacity;   /* ёмкость устройства, байты      */
    uint64_t slots;      /* число позиций (слот 512 б)     */
    uint64_t seed;       /* фактическое зерно rng          */
} dgrndinfo;

typedef struct {
    uint64_t index;      /* номер успешного опа, с 0       */
    uint64_t off;        /* позиция чтения, байты          */
    uint64_t us;         /* латентность опа, µs            */
} dgrndop;

typedef struct {
    uint64_t off;
    uint64_t us;
    long     err;
} dgrnderr;

typedef struct {
    uint64_t ops;
    uint64_t eio;
    uint64_t time_us;    /* сумма всех латентностей        */
    double   med_us;
    double   p99_us;
    double   avg_us;
    int      stopped;
    int      oserr;      /* errno фатальной ошибки (0 - нет)    */
} dgrndresult;

typedef void (*dgrnd_begin_fn)(void *ctx, const dgrndinfo *info);
typedef void (*dgrnd_op_fn)(void *ctx, const dgrndop *op);
typedef void (*dgrnd_err_fn)(void *ctx, const dgrnderr *err);

/* Возврат: 0 - прогон выполнен, -1 - ошибка (текст в errbuf) */
int dgrndcore(const dgrndcfg *cfg,
              dgrnd_begin_fn on_begin,
              dgrnd_op_fn on_op,
              dgrnd_err_fn on_err,
              void *ctx,
              volatile int *cancel,
              dgrndresult *res,
              char *errbuf, size_t errlen);

#ifdef __cplusplus
}
#endif

#endif /* DGRNDCORE_H */
