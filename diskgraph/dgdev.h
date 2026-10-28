/*
 * dgdev.h - общее для ядер замеров и UI: часы, открытие raw-ноды,
 *           ёмкость, перцентили
 *
 * Раньше это жило копиями в dglincore.c, dgrndcore.c и diskgraph.m.
 * Тексты ошибок нейтральные: подсказку про права дописывает тот,
 * кто знает контекст (консоль - "sudo", UI - "пароль").
 */
#ifndef DGDEV_H
#define DGDEV_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* mach-время в µs; dg_clock_init звать до первого dg_now_us */
void     dg_clock_init(void);
uint64_t dg_now_us(void);

/*
 * Открыть устройство на чтение для честного замера: ёмкость через
 * DKIOCGETBLOCKSIZE/COUNT, F_NOCACHE, readahead off.
 * Возврат fd или -1 (текст в errbuf, errno в *oserr, если не NULL).
 */
int dg_open_raw(const char *dev, uint64_t *cap, int *oserr,
                char *errbuf, size_t errlen);

/* только ёмкость (для оси X до Старта); 0 - не удалось */
double dg_probe_capacity(const char *dev);

/* 1 - errno означает "нет прав на ноду" */
int dg_is_perm_err(int oserr);

/* сравнение double для qsort и перцентиль по отсортированному массиву */
int    dg_dcmp(const void *a, const void *b);
double dg_pct(const double *sorted, size_t n, double p);

#ifdef __cplusplus
}
#endif

#endif /* DGDEV_H */
