/*
 * diskgraph.m - UI DiskGraph: две вкладки (Linear / Random) на РЕАЛЬНЫХ
 *               замерах через dglincore/dgrndcore
 *
 * Фронт и бэк скрещены: вкладки зовут те же ядра, что и консольные
 * dblin/dbrnd, но данные приходят не в stdout, а в sink-калбеки:
 *   - linear: sink сегмента -> точка (offset, MB/s) на графике;
 *   - random: sink опа     -> точка (offset, лаг ms) на scatter.
 *
 * Поток: UI-поток (AppKit) + один рабочий поток на запуск (pthread).
 * Данные лежат в общих массивах под мьютексом; таймер 0.25 c
 * перерисовывает и обновляет статус. Никаких stdout-обменов.
 *
 * Права: чтение /dev/rdiskN требует root, поэтому приложение надо
 * запускать через sudo (подъём прав при старте - следующий шаг).
 *
 * Только ЧТЕНИЕ. Запись не выполняется никогда.
 *
 * ObjC строго objc1 (Jaguar..Leopard runtime): без @"..."-литералов,
 * без свойств и fast enumeration.
 */
#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/disk.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <math.h>

#include "dglincore.h"
#include "dgrndcore.h"

/* ---- параметры приборов (как в консольных тулзах) ---- */
#define LIN_SEG (32ULL << 20)      /* 32 MiB чанк  */
#define LIN_BLK (1ULL  << 20)      /* 1 MiB блок   */
#define RND_BLK 4096ULL            /* 4 KiB        */
#define RND_WARMUP 16
#define TICK_SEC 0.25

#define WIN_W 1000.0f
#define WIN_H  720.0f

typedef struct {
    double off;   /* позиция по ёмкости, байты */
    double val;   /* MB/s (linear) / µs (random) */
} dpt;

static dpt *g_lin = NULL;  static int g_lin_n = 0;
static dpt *g_rnd = NULL;  static int g_rnd_n = 0;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---- состояние прогонов ---- */
static int  g_run = 0;                  /* 0 idle, 1 linear, 2 random */
static volatile int g_lin_cancel = 0;
static volatile int g_rnd_cancel = 0;
static int  g_lin_done = 0, g_rnd_done = 0;
static int  g_lin_failed = 0, g_rnd_failed = 0;
static char g_lin_err[256] = "";
static char g_rnd_err[256] = "";
static dglinresult g_lin_res;
static dgrndresult g_rnd_res;
static uint64_t g_lin_total = 0;
static uint64_t g_lin_eio = 0;
static int      g_lin_idx_done = 0;     /* сегментов завершено */
static int      g_lin_elapsed = 0;      /* длительность прогона, с */
static int      g_rnd_elapsed_done = 0;
static time_t   g_run_t0 = 0;

/* снимок параметров на момент Старт */
static char g_lin_path[64] = "";
static int  g_lin_duty = 10;
static char g_rnd_path[64] = "";
static int  g_rnd_secs = 30;

/* значения слайдеров */
static int g_duty = 10;

/* ---- UI ---- */
static NSTextField   *g_lin_status = NULL;
static NSTextField   *g_rnd_status = NULL;
static NSTextField   *g_duty_val  = NULL;
static NSTextField   *g_secs_val  = NULL;
static NSButton      *g_lin_start = NULL;
static NSButton      *g_rnd_start = NULL;
static NSSlider      *g_lin_slider = NULL;
static NSSlider      *g_rnd_slider = NULL;
static NSView        *g_lin_view = NULL;
static NSView        *g_rnd_view = NULL;
static NSPopUpButton *g_lin_dev = NULL;
static NSPopUpButton *g_rnd_dev = NULL;
static NSTabView     *g_tabs = NULL;
static NSView        *g_root = NULL;

/* выбранное устройство (общее для вкладок) */
static char g_dev[64] = "/dev/rdisk0";

/* ёмкость того устройства, что реально читали (для оси X) */
static double g_cap = 0.0;

/* dev-хук -cycle */
static int g_cycle = 0;
static int g_cycle_ticks = 0;

/* dev-хук -stop N: через N секунд после старта нажать "Стоп" */
static time_t g_stop_at = 0;

/* dev-хук -again: после завершения прогона сразу стартовать снова */
static int g_again = 0;

/* ------------------------------------------------------------ данные */

/* вызывать под g_lock */
static int pts_add(dpt **arr, int *npt, double off, double val)
{
    dpt *p = (dpt *)realloc(*arr, (size_t)(*npt + 1) * sizeof(dpt));
    if (p == NULL)
        return 0;
    *arr = p;
    (*arr)[*npt].off = off;
    (*arr)[*npt].val = val;
    (*npt)++;
    return 1;
}

static double avg_of(const dpt *a, int n)
{
    double s = 0.0;
    int i;
    if (n <= 0) return 0.0;
    for (i = 0; i < n; i++) s += a[i].val;
    return s / (double)n;
}

static int dcmp(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static double pctl_of(const dpt *a, int n, double p)
{
    double *v, r;
    int i;
    if (n <= 0) return 0.0;
    v = (double *)malloc((size_t)n * sizeof(double));
    if (v == NULL) return 0.0;
    for (i = 0; i < n; i++) v[i] = a[i].val;
    qsort(v, (size_t)n, sizeof(double), dcmp);
    i = (int)(p * (double)(n - 1) + 0.5);
    if (i < 0) i = 0;
    if (i >= n) i = n - 1;
    r = v[i];
    free(v);
    return r;
}

/* ------------------------------------------------ список устройств */

static int intcmp(const void *a, const void *b)
{
    return *(const int *)a - *(const int *)b;
}

/* /dev/rdiskN (raw) и /dev/diskN (buffered) - только целые диски;
 * разбиения (sN) не нужны, пары идут рядом для сравнения */
static NSArray *scan_disks(void)
{
    NSMutableArray *a = [NSMutableArray array];
    DIR            *d = opendir("/dev");
    struct dirent  *e;
    int             nums[64];
    int             n = 0, i, j, dup;

    if (d != NULL) {
        while ((e = readdir(d)) != NULL) {
            const char *nm = e->d_name;
            const char *p;
            int         val = 0, ok = 1;

            if (strncmp(nm, "rdisk", 5) == 0) {
                p = nm + 5;
            } else if (strncmp(nm, "disk", 4) == 0) {
                p = nm + 4;
            } else {
                continue;
            }
            if (*p == 0)
                continue;
            while (*p) {
                if (*p < '0' || *p > '9') { ok = 0; break; }
                val = val * 10 + (*p - '0');
                p++;
            }
            if (!ok)
                continue;

            dup = 0;
            for (j = 0; j < n; j++)
                if (nums[j] == val)
                    dup = 1;
            if (dup)
                continue;
            if (n < 64)
                nums[n++] = val;
        }
        closedir(d);
    }

    qsort(nums, (size_t)n, sizeof(int), intcmp);

    for (i = 0; i < n; i++) {
        char pa[64], pb[64];

        snprintf(pa, sizeof pa, "/dev/rdisk%d", nums[i]);
        snprintf(pb, sizeof pb, "/dev/disk%d",  nums[i]);
        if (access(pa, F_OK) == 0)
            [a addObject:[NSString stringWithUTF8String:pa]];
        if (access(pb, F_OK) == 0)
            [a addObject:[NSString stringWithUTF8String:pb]];
    }
    return a;
}

static void fill_dev_popup(NSPopUpButton *pop)
{
    NSArray *disks;
    int      i, sel = -1;

    if (pop == NULL)
        return;

    disks = scan_disks();
    [pop removeAllItems];

    for (i = 0; i < (int)[disks count]; i++) {
        NSString *t = [disks objectAtIndex:i];
        [pop addItemWithTitle:t];
        if ([t isEqualToString:[NSString stringWithUTF8String:g_dev]])
            sel = i;
    }

    if ([disks count] == 0) {
        [pop addItemWithTitle:[NSString stringWithUTF8String:"(нет дисков)"]];
        sel = 0;
    } else if (sel < 0) {
        sel = 0;
        strncpy(g_dev, [[disks objectAtIndex:0] UTF8String], sizeof g_dev - 1);
        g_dev[sizeof g_dev - 1] = 0;
    }
    [pop selectItemAtIndex:sel];
}

static void refresh_devs(void)
{
    if (g_lin_dev != NULL) fill_dev_popup(g_lin_dev);
    if (g_rnd_dev != NULL) fill_dev_popup(g_rnd_dev);
}

/* ёмкость для оси X ещё до Старта (нужен root; иначе 0 = нет тиков) */
static double probe_capacity(const char *path)
{
    int      fd;
    uint32_t bs = 512;
    uint64_t n = 0;

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0.0;
    if (ioctl(fd, DKIOCGETBLOCKSIZE, &bs) < 0)
        bs = 512;
    if (ioctl(fd, DKIOCGETBLOCKCOUNT, &n) < 0) {
        close(fd);
        return 0.0;
    }
    close(fd);
    return (double)n * (double)bs;
}

/* ------------------------------------------------------- статус-строки */

/* вызывать без g_lock */
static void status_lin(void)
{
    char s[320];
    int  n, idx, failed, canc;
    uint64_t total, eio;
    double avg;
    dglinresult res;

    pthread_mutex_lock(&g_lock);
    n      = g_lin_n;
    idx    = g_lin_idx_done;
    total  = g_lin_total;
    eio    = g_lin_eio;
    failed = g_lin_failed;
    canc   = g_lin_cancel;
    avg    = avg_of(g_lin, g_lin_n);
    res    = g_lin_res;
    s[0] = 0;
    pthread_mutex_unlock(&g_lock);

    if (g_run == 1) {
        if (canc) {
            snprintf(s, sizeof s,
                     "linear: останавливаюсь после текущего блока...");
        } else if (total > 0) {
            snprintf(s, sizeof s,
                     "linear: сегмент %d / %llu   avg %.1f MB/s   "
                     "duty %d%%",
                     idx, (unsigned long long)total, avg, g_lin_duty);
        } else {
            snprintf(s, sizeof s, "linear: открываю %s ...", g_lin_path);
        }
    } else if (failed) {
        pthread_mutex_lock(&g_lock);
        snprintf(s, sizeof s, "linear: ошибка: %s", g_lin_err);
        pthread_mutex_unlock(&g_lock);
    } else if (n > 0) {
        double mbs = res.time_us > 0
            ? (double)res.bytes / ((double)res.time_us / 1e6) / 1048576.0
            : 0.0;
        snprintf(s, sizeof s,
                 "linear готово: %llu сегментов   avg %.1f MB/s   "
                 "eio %llu   время %d:%02d%s",
                 (unsigned long long)res.chunks, mbs,
                 (unsigned long long)res.eio,
                 g_lin_elapsed / 60, g_lin_elapsed % 60,
                 res.stopped ? "   [остановлено]" : "");
    } else {
        snprintf(s, sizeof s, "linear: нет данных - нажмите Старт");
    }

    [g_lin_status setStringValue:[NSString stringWithUTF8String:s]];
}

/* вызывать без g_lock */
static void status_rnd(void)
{
    char s[320];
    int  n, failed, canc, secs, elapsed;
    double med, p99;
    dgrndresult res;

    pthread_mutex_lock(&g_lock);
    n       = g_rnd_n;
    failed  = g_rnd_failed;
    canc    = g_rnd_cancel;
    res     = g_rnd_res;
    med     = pctl_of(g_rnd, g_rnd_n, 0.50) / 1000.0;
    p99     = pctl_of(g_rnd, g_rnd_n, 0.99) / 1000.0;
    secs    = g_rnd_secs;
    s[0] = 0;
    pthread_mutex_unlock(&g_lock);

    elapsed = (int)(time(NULL) - g_run_t0);
    if (elapsed < 0) elapsed = 0;
    if (elapsed > secs) elapsed = secs;

    if (g_run == 2) {
        if (canc) {
            snprintf(s, sizeof s, "random: останавливаюсь...");
        } else {
            snprintf(s, sizeof s,
                     "random: ops %d   %d / %d s   med %.1f ms   "
                     "p99 %.1f ms",
                     n, elapsed, secs, med, p99);
        }
    } else if (failed) {
        pthread_mutex_lock(&g_lock);
        snprintf(s, sizeof s, "random: ошибка: %s", g_rnd_err);
        pthread_mutex_unlock(&g_lock);
    } else if (n > 0) {
        double m = res.med_us / 1000.0;
        double p = res.p99_us / 1000.0;
        snprintf(s, sizeof s,
                 "random готово: %llu ops   med %.1f ms   p99 %.1f ms   "
                 "eio %llu   время %d:%02d%s",
                 (unsigned long long)res.ops, m, p,
                 (unsigned long long)res.eio,
                 g_rnd_elapsed_done / 60, g_rnd_elapsed_done % 60,
                 res.stopped ? "   [остановлено]" : "");
    } else {
        snprintf(s, sizeof s, "random: нет данных - нажмите Старт");
    }

    [g_rnd_status setStringValue:[NSString stringWithUTF8String:s]];
}

/* ------------------------------------------------------- рабочие потоки */

static void lin_begin(void *ctx, const dglininfo *info)
{
    (void)ctx;
    pthread_mutex_lock(&g_lock);
    g_lin_total = info->chunks;
    g_cap = (double)info->capacity;
    pthread_mutex_unlock(&g_lock);
}

static void lin_seg(void *ctx, const dglinseg *seg)
{
    double mbs;
    (void)ctx;

    mbs = seg->time_us > 0
        ? (double)seg->bytes / ((double)seg->time_us / 1e6) / 1048576.0
        : 0.0;

    pthread_mutex_lock(&g_lock);
    pts_add(&g_lin, &g_lin_n, (double)seg->off, mbs);
    g_lin_eio += seg->eio;
    g_lin_idx_done = (int)(seg->index + 1);
    pthread_mutex_unlock(&g_lock);
}

static void lin_err(void *ctx, const dglinerr *e)
{
    (void)ctx;
    (void)e;
}

static void *lin_thread(void *arg)
{
    dglincfg    cfg;
    dglinresult res;
    char        err[256];
    int         rc;

    (void)arg;
    memset(&cfg, 0, sizeof cfg);
    cfg.dev    = g_lin_path;
    cfg.seg    = LIN_SEG;
    cfg.blk    = LIN_BLK;
    cfg.duty   = g_lin_duty;
    cfg.extent = 0;

    rc = dglincore(&cfg, lin_begin, lin_seg, lin_err, NULL,
                   &g_lin_cancel, &res, err, sizeof err);

    pthread_mutex_lock(&g_lock);
    if (rc != 0) {
        snprintf(g_lin_err, sizeof g_lin_err, "%s", err);
        g_lin_failed = 1;
        memset(&res, 0, sizeof res);
    } else {
        g_lin_failed = 0;
    }
    g_lin_res = res;
    g_lin_elapsed = (int)(time(NULL) - g_run_t0);
    g_lin_done = 1;
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

static void rnd_begin(void *ctx, const dgrndinfo *info)
{
    (void)ctx;
    pthread_mutex_lock(&g_lock);
    g_cap = (double)info->capacity;
    pthread_mutex_unlock(&g_lock);
}

static void rnd_op(void *ctx, const dgrndop *op)
{
    (void)ctx;
    pthread_mutex_lock(&g_lock);
    pts_add(&g_rnd, &g_rnd_n, (double)op->off, (double)op->us);
    pthread_mutex_unlock(&g_lock);
}

static void rnd_err(void *ctx, const dgrnderr *e)
{
    (void)ctx;
    (void)e;
}

static void *rnd_thread(void *arg)
{
    dgrndcfg    cfg;
    dgrndresult res;
    char        err[256];
    int         rc;

    (void)arg;
    memset(&cfg, 0, sizeof cfg);
    cfg.dev    = g_rnd_path;
    cfg.blk    = RND_BLK;
    cfg.secs   = g_rnd_secs;
    cfg.warmup = RND_WARMUP;
    cfg.seed   = 0;                 /* из времени/pid */

    rc = dgrndcore(&cfg, rnd_begin, rnd_op, rnd_err, NULL,
                   &g_rnd_cancel, &res, err, sizeof err);

    pthread_mutex_lock(&g_lock);
    if (rc != 0) {
        snprintf(g_rnd_err, sizeof g_rnd_err, "%s", err);
        g_rnd_failed = 1;
        memset(&res, 0, sizeof res);
    } else {
        g_rnd_failed = 0;
    }
    g_rnd_res = res;
    g_rnd_elapsed_done = (int)(time(NULL) - g_run_t0);
    g_rnd_done = 1;
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

/* UI-поток: занятость контролов на время прогона */
static void set_controls_busy(int busy)
{
    [g_lin_start setEnabled:!busy];
    [g_rnd_start setEnabled:!busy];
    [g_lin_slider setEnabled:!busy];
    [g_rnd_slider setEnabled:!busy];
    if (g_lin_dev != NULL) [g_lin_dev setEnabled:!busy];
    if (g_rnd_dev != NULL) [g_rnd_dev setEnabled:!busy];
}

static void lin_start(void)
{
    pthread_t th;

    if (g_run != 0)
        return;

    pthread_mutex_lock(&g_lock);
    free(g_lin);
    g_lin = NULL;
    g_lin_n = 0;
    g_lin_total = 0;
    g_lin_eio = 0;
    g_lin_idx_done = 0;
    g_lin_failed = 0;
    g_lin_err[0] = 0;
    memset(&g_lin_res, 0, sizeof g_lin_res);
    pthread_mutex_unlock(&g_lock);

    g_lin_cancel = 0;
    g_lin_done = 0;
    g_run = 1;
    g_run_t0 = time(NULL);

    strncpy(g_lin_path, g_dev, sizeof g_lin_path - 1);
    g_lin_path[sizeof g_lin_path - 1] = 0;
    g_lin_duty = g_duty;

    set_controls_busy(1);
    status_lin();

    if (pthread_create(&th, NULL, lin_thread, NULL) != 0) {
        pthread_mutex_lock(&g_lock);
        snprintf(g_lin_err, sizeof g_lin_err, "не удалось создать поток");
        g_lin_failed = 1;
        g_lin_done = 1;
        pthread_mutex_unlock(&g_lock);
        return;
    }
    pthread_detach(th);
}

static void rnd_start(void)
{
    pthread_t th;

    if (g_run != 0)
        return;

    pthread_mutex_lock(&g_lock);
    free(g_rnd);
    g_rnd = NULL;
    g_rnd_n = 0;
    g_rnd_failed = 0;
    g_rnd_err[0] = 0;
    memset(&g_rnd_res, 0, sizeof g_rnd_res);
    pthread_mutex_unlock(&g_lock);

    g_rnd_cancel = 0;
    g_rnd_done = 0;
    g_run = 2;
    g_run_t0 = time(NULL);

    strncpy(g_rnd_path, g_dev, sizeof g_rnd_path - 1);
    g_rnd_path[sizeof g_rnd_path - 1] = 0;

    set_controls_busy(1);
    status_rnd();

    if (pthread_create(&th, NULL, rnd_thread, NULL) != 0) {
        pthread_mutex_lock(&g_lock);
        snprintf(g_rnd_err, sizeof g_rnd_err, "не удалось создать поток");
        g_rnd_failed = 1;
        g_rnd_done = 1;
        pthread_mutex_unlock(&g_lock);
        return;
    }
    pthread_detach(th);
}

/* ------------------------------------------------------- рисование */

static NSDictionary *text_attrs(float size)
{
    static NSDictionary *attrs8  = NULL;
    static NSDictionary *attrs9  = NULL;
    static NSDictionary *attrs10 = NULL;

    if (attrs8 == NULL) {
        attrs8  = [[NSDictionary dictionaryWithObjectsAndKeys:
            [NSFont systemFontOfSize:8.0f],  NSFontAttributeName, NULL]
            retain];
        attrs9  = [[NSDictionary dictionaryWithObjectsAndKeys:
            [NSFont systemFontOfSize:9.0f],  NSFontAttributeName, NULL]
            retain];
        attrs10 = [[NSDictionary dictionaryWithObjectsAndKeys:
            [NSFont systemFontOfSize:10.0f], NSFontAttributeName, NULL]
            retain];
    }
    if (size <= 8.0f)  return attrs8;
    if (size <= 9.0f)  return attrs9;
    return attrs10;
}

static NSDictionary *text_attrs_gray(void)
{
    static NSDictionary *g9 = NULL;

    if (g9 == NULL) {
        g9 = [[NSDictionary dictionaryWithObjectsAndKeys:
            [NSFont systemFontOfSize:9.0f], NSFontAttributeName,
            [NSColor darkGrayColor],         NSForegroundColorAttributeName,
            NULL] retain];
    }
    return g9;
}

static void draw_text(const char *s, NSPoint p, float size)
{
    NSString *str = [NSString stringWithUTF8String:s];
    [str drawAtPoint:p withAttributes:text_attrs(size)];
}

static void draw_text_gray(const char *s, NSPoint p)
{
    NSString *str = [NSString stringWithUTF8String:s];
    [str drawAtPoint:p withAttributes:text_attrs_gray()];
}

static float text_width(const char *s, float size)
{
    NSString *str = [NSString stringWithUTF8String:s];
    return [str sizeWithAttributes:text_attrs(size)].width;
}

/* круглые мантиссы для приборной шкалы */
static int is_nice(double x)
{
    static const double cand[10] = {1.0, 1.2, 1.5, 2.0, 2.5,
                                    3.0, 4.0, 5.0, 6.0, 8.0};
    double m;
    int    i;

    if (x <= 0.0)
        return 0;
    m = x;
    while (m >= 10.0) m /= 10.0;
    while (m < 1.0)   m *= 10.0;
    for (i = 0; i < 10; i++)
        if (fabs(m - cand[i]) < 1e-9)
            return 1;
    return 0;
}

static double nice_up(double x)
{
    static const double cand[10] = {1.0, 1.2, 1.5, 2.0, 2.5,
                                    3.0, 4.0, 5.0, 6.0, 8.0};
    double exp = 1.0;
    double f;
    int    i;

    if (x <= 0.0)
        return 1.0;
    while (x / exp >= 10.0) exp *= 10.0;
    while (x / exp < 1.0)   exp /= 10.0;
    f = x / exp;
    for (i = 0; i < 10; i++)
        if (f <= cand[i] + 1e-12)
            return cand[i] * exp;
    return 10.0 * exp;
}

/* Y-максимум под данные: круглые деления, запас ~8%, без пустого верха */
static double axis_vmax(double raw, int *ndiv)
{
    double vmax = nice_up(raw * 1.08);
    int    d;

    for (d = 4; d <= 6; d++) {
        if (is_nice(vmax / (double)d)) {
            *ndiv = d;
            return vmax;
        }
    }
    *ndiv = 5;
    return vmax;
}

/* рамка, сетка, оси и подписи; cap <= 0 - ёмкость ещё неизвестна */
static void draw_plot(NSRect pv, double vmax, int ndiv,
                      int y_ms, const char *ylab, double cap)
{
    float x0 = pv.origin.x, y0 = pv.origin.y;
    float x1 = x0 + pv.size.width, y1 = y0 + pv.size.height;
    NSBezierPath *grid = [NSBezierPath bezierPath];
    NSBezierPath *axes = [NSBezierPath bezierPath];
    int   i, gb;
    float t;

    if (cap > 0.0) {
        [[NSColor colorWithCalibratedWhite:0.88f alpha:1.0f] set];
        for (i = 1; i < ndiv; i++) {
            float y = y0 + ((float)i / (float)ndiv) * pv.size.height;
            [grid moveToPoint:NSMakePoint(x0, y)];
            [grid lineToPoint:NSMakePoint(x1, y)];
        }
        for (gb = 8; gb < 9999; gb += 8) {
            float x = x0 + (float)((double)gb * 1073741824.0 / cap) *
                           pv.size.width;
            if (x >= x1) break;
            [grid moveToPoint:NSMakePoint(x, y0)];
            [grid lineToPoint:NSMakePoint(x, y1)];
        }
        [grid setLineWidth:1.0f];
        [grid stroke];
    } else {
        [[NSColor colorWithCalibratedWhite:0.88f alpha:1.0f] set];
        for (i = 1; i < ndiv; i++) {
            float y = y0 + ((float)i / (float)ndiv) * pv.size.height;
            [grid moveToPoint:NSMakePoint(x0, y)];
            [grid lineToPoint:NSMakePoint(x1, y)];
        }
        [grid setLineWidth:1.0f];
        [grid stroke];
    }

    /* рамка графика */
    [[NSColor colorWithCalibratedWhite:0.78f alpha:1.0f] set];
    [axes moveToPoint:NSMakePoint(x0, y0)];
    [axes lineToPoint:NSMakePoint(x1, y0)];
    [axes lineToPoint:NSMakePoint(x1, y1)];
    [axes lineToPoint:NSMakePoint(x0, y1)];
    [axes closePath];
    [axes setLineWidth:1.0f];
    [axes stroke];

    [[NSColor blackColor] set];
    if (cap > 0.0) {
        for (gb = 0; gb < 9999; gb += 8) {
            float x = x0 + (float)((double)gb * 1073741824.0 / cap) *
                           pv.size.width;
            char  lbl[24];
            NSBezierPath *tick;
            if (x > x1) break;
            tick = [NSBezierPath bezierPath];
            [tick moveToPoint:NSMakePoint(x, y0)];
            [tick lineToPoint:NSMakePoint(x, y0 - 4.0f)];
            [tick stroke];
            snprintf(lbl, sizeof lbl, "%d", gb);
            draw_text(lbl,
                      NSMakePoint(x - text_width(lbl, 9.0f) * 0.5f,
                                  y0 - 14.0f), 9.0f);
        }
    }

    for (t = 0.0f; t <= (float)ndiv; t += 1.0f) {
        float y   = y0 + (t / (float)ndiv) * pv.size.height;
        double val = vmax * ((double)t / (double)ndiv);
        char   lbl[32];
        NSBezierPath *tick = [NSBezierPath bezierPath];

        [tick moveToPoint:NSMakePoint(x0, y)];
        [tick lineToPoint:NSMakePoint(x0 - 4.0f, y)];
        [tick stroke];
        if (y_ms)
            snprintf(lbl, sizeof lbl, "%.0f", val / 1000.0);
        else
            snprintf(lbl, sizeof lbl, "%.0f", val);
        draw_text(lbl,
                  NSMakePoint(x0 - 7.0f - text_width(lbl, 9.0f),
                              y - 4.0f), 9.0f);
    }

    {
        const char *xlab = "поверхность диска, GB";
        draw_text_gray(xlab,
            NSMakePoint(x0 + (pv.size.width - text_width(xlab, 9.0f)) * 0.5f,
                        y0 - 28.0f));
    }
    draw_text_gray(ylab, NSMakePoint(x0 + 4.0f, y1 + 6.0f));
}

@interface LinView : NSView
@end

@implementation LinView

- (void)drawRect:(NSRect)dirty
{
    NSRect f = [self bounds];
    float  padl = 56.0f, padr = 18.0f, padt = 26.0f, padb = 40.0f;
    NSRect pv;
    NSBezierPath *bp;
    double raw = 0.0, vmax, cap;
    int    ndiv = 4, i;

    (void)dirty;
    [[NSColor whiteColor] set];
    NSRectFill(f);

    pv = NSMakeRect(padl, padb,
                    NSWidth(f) - padl - padr,
                    NSHeight(f) - padt - padb);

    pthread_mutex_lock(&g_lock);
    for (i = 0; i < g_lin_n; i++)
        if (g_lin[i].val > raw)
            raw = g_lin[i].val;
    vmax = (raw > 0.0) ? axis_vmax(raw, &ndiv) : 40.0;
    cap = g_cap;

    draw_plot(pv, vmax, ndiv, 0, "MB/s", cap);

    if (g_lin_n > 0) {
        float mx = pv.origin.x +
                   (float)(g_lin[g_lin_n - 1].off / cap) * pv.size.width;

        [[NSColor colorWithCalibratedWhite:0.65f alpha:1.0f] set];
        [NSBezierPath strokeLineFromPoint:NSMakePoint(mx, pv.origin.y)
                                  toPoint:NSMakePoint(mx, pv.origin.y +
                                                      pv.size.height)];

        bp = [[NSBezierPath bezierPath] retain];
        [[NSColor blueColor] set];
        [bp setLineWidth:1.4f];
        for (i = 0; i < g_lin_n; i++) {
            NSPoint p = NSMakePoint(
                pv.origin.x + (float)(g_lin[i].off / cap) * pv.size.width,
                pv.origin.y + (float)(g_lin[i].val / vmax) * pv.size.height);
            if (i == 0)
                [bp moveToPoint:p];
            else
                [bp lineToPoint:p];
        }
        [bp stroke];
        [bp release];
    }
    pthread_mutex_unlock(&g_lock);
}

- (void)dealloc
{
    [super dealloc];
}

@end

@interface RndView : NSView
@end

@implementation RndView

- (void)drawRect:(NSRect)dirty
{
    NSRect f = [self bounds];
    float  padl = 56.0f, padr = 18.0f, padt = 26.0f, padb = 40.0f;
    NSRect pv;
    double raw = 0.0, vmax, cap, thr = 1e30;
    int    ndiv = 5, i;

    (void)dirty;
    [[NSColor whiteColor] set];
    NSRectFill(f);

    pv = NSMakeRect(padl, padb,
                    NSWidth(f) - padl - padr,
                    NSHeight(f) - padt - padb);

    pthread_mutex_lock(&g_lock);
    for (i = 0; i < g_rnd_n; i++)
        if (g_rnd[i].val > raw)
            raw = g_rnd[i].val;
    vmax = (raw > 0.0) ? axis_vmax(raw, &ndiv) : 50000.0;   /* µs */
    cap = g_cap;

    draw_plot(pv, vmax, ndiv, 1, "ms", cap);

    if (g_rnd_n > 0) {
        if (g_rnd_n >= 100)
            thr = pctl_of(g_rnd, g_rnd_n, 0.99);
        for (i = 0; i < g_rnd_n; i++) {
            float x = pv.origin.x +
                      (float)(g_rnd[i].off / cap) * pv.size.width;
            float y = pv.origin.y +
                      (float)(g_rnd[i].val / vmax) * pv.size.height;
            if (g_rnd[i].val > thr)
                [[NSColor redColor] set];
            else
                [[NSColor blackColor] set];
            NSRectFill(NSMakeRect(x - 1.5f, y - 1.5f, 3.0f, 3.0f));
        }
    }
    pthread_mutex_unlock(&g_lock);
}

- (void)dealloc
{
    [super dealloc];
}

@end

/* ------------------------------------------------------------ контроллер */

@interface Controller : NSObject
- (void)linStart:(id)s;
- (void)linStop:(id)s;
- (void)rndStart:(id)s;
- (void)rndStop:(id)s;
- (void)dutyChanged:(id)s;
- (void)secsChanged:(id)s;
- (void)devChanged:(id)s;
- (void)appActive:(NSNotification *)n;
- (void)windowDidResize:(NSNotification *)n;
- (void)tick:(NSTimer *)timer;
- (void)quit:(id)s;
@end

@implementation Controller

- (id)init
{
    self = [super init];
    if (self != nil) {
        [[NSNotificationCenter defaultCenter] addObserver:self
            selector:@selector(appActive:)
            name:NSApplicationDidBecomeActiveNotification
            object:nil];
        [[NSNotificationCenter defaultCenter] addObserver:self
            selector:@selector(windowDidResize:)
            name:NSWindowDidResizeNotification
            object:nil];
    }
    return self;
}

- (void)dealloc
{
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [super dealloc];
}

- (void)linStart:(id)s
{
    (void)s;
    lin_start();
}

- (void)linStop:(id)s
{
    (void)s;
    if (g_run == 1) {
        g_lin_cancel = 1;
        status_lin();
    }
}

- (void)rndStart:(id)s
{
    (void)s;
    rnd_start();
}

- (void)rndStop:(id)s
{
    (void)s;
    if (g_run == 2) {
        g_rnd_cancel = 1;
        status_rnd();
    }
}

- (void)dutyChanged:(id)s
{
    char lbl[64];
    g_duty = [s intValue];
    snprintf(lbl, sizeof lbl, "%d%%", g_duty);
    [g_duty_val setStringValue:[NSString stringWithUTF8String:lbl]];
    if (g_run == 0)
        status_lin();
}

- (void)secsChanged:(id)s
{
    char lbl[64];
    g_rnd_secs = [s intValue];
    snprintf(lbl, sizeof lbl, "%d s", g_rnd_secs);
    [g_secs_val setStringValue:[NSString stringWithUTF8String:lbl]];
    if (g_run == 0)
        status_rnd();
}

- (void)devChanged:(id)s
{
    NSString *t = [s titleOfSelectedItem];
    NSPopUpButton *other;
    const char *u;
    int i;

    if (t == nil)
        return;
    u = [t UTF8String];
    if (u == NULL)
        return;

    strncpy(g_dev, u, sizeof g_dev - 1);
    g_dev[sizeof g_dev - 1] = 0;

    {
        double c = probe_capacity(g_dev);
        if (c > 0.0)
            g_cap = c;
    }
    [g_lin_view setNeedsDisplay:YES];
    [g_rnd_view setNeedsDisplay:YES];

    other = (s == g_lin_dev) ? g_rnd_dev : g_lin_dev;
    if (other != NULL) {
        for (i = 0; i < [other numberOfItems]; i++) {
            if ([[other itemTitleAtIndex:i] isEqualToString:t]) {
                [other selectItemAtIndex:i];
                break;
            }
        }
    }
}

- (void)appActive:(NSNotification *)n
{
    (void)n;
    refresh_devs();
}

- (void)windowDidResize:(NSNotification *)n
{
    NSRect cr;
    int    i;

    (void)n;
    if (g_tabs == NULL || g_root == NULL)
        return;

    [g_tabs setFrame:[g_root bounds]];
    cr = [g_tabs contentRect];
    for (i = 0; i < (int)[g_tabs numberOfTabViewItems]; i++) {
        NSView *v = [[g_tabs tabViewItemAtIndex:i] view];
        if (v != nil)
            [v setFrame:cr];
    }
}

- (void)tick:(NSTimer *)timer
{
    int ldone, rdone;

    (void)timer;

    pthread_mutex_lock(&g_lock);
    ldone = g_lin_done;
    rdone = g_rnd_done;
    pthread_mutex_unlock(&g_lock);

    if (g_run == 1 && ldone) {
        g_run = 0;
        g_lin_done = 0;
        set_controls_busy(0);
        if (g_again) {
            g_again = 0;
            printf("again: restart linear\n");
            fflush(stdout);
            lin_start();
        }
    }
    if (g_run == 2 && rdone) {
        g_run = 0;
        g_rnd_done = 0;
        set_controls_busy(0);
        if (g_again) {
            g_again = 0;
            printf("again: restart random\n");
            fflush(stdout);
            rnd_start();
        }
    }

    if (g_stop_at > 0 && time(NULL) >= g_stop_at) {
        g_stop_at = 0;
        if (g_run == 1)
            g_lin_cancel = 1;
        if (g_run == 2)
            g_rnd_cancel = 1;
    }

    status_lin();
    status_rnd();
    [g_lin_view setNeedsDisplay:YES];
    [g_rnd_view setNeedsDisplay:YES];

    if (g_cycle) {
        g_cycle_ticks++;
        if (g_cycle_ticks % (int)(3.0 / TICK_SEC) == 0) {
            int cur = (int)[g_tabs indexOfTabViewItem:
                                [g_tabs selectedTabViewItem]];
            int nxt = (cur == 0) ? 1 : 0;
            [g_tabs selectTabViewItemAtIndex:nxt];
            pthread_mutex_lock(&g_lock);
            printf("cycle: tab %d -> %d   lin_n %d   rnd_n %d\n",
                   cur, nxt, g_lin_n, g_rnd_n);
            fflush(stdout);
            pthread_mutex_unlock(&g_lock);
        }
    }
}

- (void)quit:(id)s
{
    (void)s;
    g_lin_cancel = 1;
    g_rnd_cancel = 1;
    usleep(200000);
    [NSApp terminate:nil];
}

@end

/* ------------------------------------------------------------ сборка UI */

static NSTextField *make_label(NSView *v, const char *txt,
                               float x, float y, float w)
{
    NSTextField *lab = [[NSTextField alloc]
        initWithFrame:NSMakeRect(x, y, w, 18.0f)];
    [lab setStringValue:[NSString stringWithUTF8String:txt]];
    [lab setBezeled:NO];
    [lab setEditable:NO];
    [lab setDrawsBackground:NO];
    [lab setAutoresizingMask:NSViewMinYMargin];
    [v addSubview:lab];
    return [lab autorelease];
}

static NSPopUpButton *make_dev_popup(NSView *v, Controller *ctl,
                                     float x, float y, float w)
{
    NSPopUpButton *pop = [[NSPopUpButton alloc]
        initWithFrame:NSMakeRect(x, y, w, 24.0f) pullsDown:NO];
    [pop setFont:[NSFont systemFontOfSize:11.0f]];
    [pop setTarget:ctl];
    [pop setAction:@selector(devChanged:)];
    [pop setAutoresizingMask:NSViewMinYMargin];
    [v addSubview:pop];
    fill_dev_popup(pop);
    return [pop autorelease];
}

static NSView *make_lin_tab(Controller *ctl, NSSize sz)
{
    NSView *v = [[NSView alloc] initWithFrame:
        NSMakeRect(0.0f, 0.0f, sz.width, sz.height)];
    NSButton *b;
    float     h = sz.height, w = sz.width;
    float     y = h - 34.0f;

    make_label(v, "диск", 12.0f, y + 3.0f, 36.0f);
    g_lin_dev = make_dev_popup(v, ctl, 50.0f, y - 2.0f, 168.0f);

    make_label(v, "duty", 234.0f, y + 3.0f, 36.0f);
    g_lin_slider = [[NSSlider alloc] initWithFrame:
        NSMakeRect(272.0f, y - 4.0f, 180.0f, 24.0f)];
    [g_lin_slider setMinValue:10.0];
    [g_lin_slider setMaxValue:100.0];
    [g_lin_slider setIntValue:g_duty];
    [g_lin_slider setTarget:ctl];
    [g_lin_slider setAction:@selector(dutyChanged:)];
    [g_lin_slider setAutoresizingMask:NSViewMinYMargin];
    [v addSubview:g_lin_slider];

    g_duty_val = make_label(v, "10%", 458.0f, y + 3.0f, 56.0f);

    g_lin_status = [[NSTextField alloc]
        initWithFrame:NSMakeRect(12.0f, h - 58.0f, w - 24.0f, 18.0f)];
    [g_lin_status setFont:[NSFont systemFontOfSize:11.0f]];
    [g_lin_status setBezeled:NO];
    [g_lin_status setEditable:NO];
    [g_lin_status setDrawsBackground:NO];
    [g_lin_status setAutoresizingMask:NSViewWidthSizable | NSViewMinYMargin];
    [v addSubview:g_lin_status];

    g_lin_start = [[NSButton alloc]
        initWithFrame:NSMakeRect(w - 196.0f, y - 4.0f, 92.0f, 28.0f)];
    [g_lin_start setBezelStyle:NSRoundedBezelStyle];
    [g_lin_start setTitle:[NSString stringWithUTF8String:"Старт"]];
    [g_lin_start setTarget:ctl];
    [g_lin_start setAction:@selector(linStart:)];
    [g_lin_start setAutoresizingMask:NSViewMinXMargin | NSViewMinYMargin];
    [v addSubview:g_lin_start];

    b = [[NSButton alloc]
        initWithFrame:NSMakeRect(w - 96.0f, y - 4.0f, 92.0f, 28.0f)];
    [b setBezelStyle:NSRoundedBezelStyle];
    [b setTitle:[NSString stringWithUTF8String:"Стоп"]];
    [b setTarget:ctl];
    [b setAction:@selector(linStop:)];
    [b setAutoresizingMask:NSViewMinXMargin | NSViewMinYMargin];
    [v addSubview:b];
    [b release];

    g_lin_view = [[LinView alloc] initWithFrame:
        NSMakeRect(8.0f, 8.0f, w - 16.0f, h - 72.0f)];
    [g_lin_view setAutoresizingMask:NSViewWidthSizable |
                                    NSViewHeightSizable];
    [v addSubview:g_lin_view];

    status_lin();
    return v;
}

static NSView *make_rnd_tab(Controller *ctl, NSSize sz)
{
    NSView *v = [[NSView alloc] initWithFrame:
        NSMakeRect(0.0f, 0.0f, sz.width, sz.height)];
    NSButton *b;
    float     h = sz.height, w = sz.width;
    float     y = h - 34.0f;

    make_label(v, "диск", 12.0f, y + 3.0f, 36.0f);
    g_rnd_dev = make_dev_popup(v, ctl, 50.0f, y - 2.0f, 168.0f);

    make_label(v, "секунд", 234.0f, y + 3.0f, 52.0f);
    g_rnd_slider = [[NSSlider alloc] initWithFrame:
        NSMakeRect(290.0f, y - 4.0f, 162.0f, 24.0f)];
    [g_rnd_slider setMinValue:5.0];
    [g_rnd_slider setMaxValue:120.0];
    [g_rnd_slider setIntValue:g_rnd_secs];
    [g_rnd_slider setTarget:ctl];
    [g_rnd_slider setAction:@selector(secsChanged:)];
    [g_rnd_slider setAutoresizingMask:NSViewMinYMargin];
    [v addSubview:g_rnd_slider];

    g_secs_val = make_label(v, "30 s", 458.0f, y + 3.0f, 56.0f);

    g_rnd_status = [[NSTextField alloc]
        initWithFrame:NSMakeRect(12.0f, h - 58.0f, w - 24.0f, 18.0f)];
    [g_rnd_status setFont:[NSFont systemFontOfSize:11.0f]];
    [g_rnd_status setBezeled:NO];
    [g_rnd_status setEditable:NO];
    [g_rnd_status setDrawsBackground:NO];
    [g_rnd_status setAutoresizingMask:NSViewWidthSizable | NSViewMinYMargin];
    [v addSubview:g_rnd_status];

    g_rnd_start = [[NSButton alloc]
        initWithFrame:NSMakeRect(w - 196.0f, y - 4.0f, 92.0f, 28.0f)];
    [g_rnd_start setBezelStyle:NSRoundedBezelStyle];
    [g_rnd_start setTitle:[NSString stringWithUTF8String:"Старт"]];
    [g_rnd_start setTarget:ctl];
    [g_rnd_start setAction:@selector(rndStart:)];
    [g_rnd_start setAutoresizingMask:NSViewMinXMargin | NSViewMinYMargin];
    [v addSubview:g_rnd_start];

    b = [[NSButton alloc]
        initWithFrame:NSMakeRect(w - 96.0f, y - 4.0f, 92.0f, 28.0f)];
    [b setBezelStyle:NSRoundedBezelStyle];
    [b setTitle:[NSString stringWithUTF8String:"Стоп"]];
    [b setTarget:ctl];
    [b setAction:@selector(rndStop:)];
    [b setAutoresizingMask:NSViewMinXMargin | NSViewMinYMargin];
    [v addSubview:b];
    [b release];

    g_rnd_view = [[RndView alloc] initWithFrame:
        NSMakeRect(8.0f, 8.0f, w - 16.0f, h - 72.0f)];
    [g_rnd_view setAutoresizingMask:NSViewWidthSizable |
                                    NSViewHeightSizable];
    [v addSubview:g_rnd_view];

    status_rnd();
    return v;
}

static void sync_tabs_layout(void)
{
    NSRect cr;
    int    i;

    if (g_tabs == NULL || g_root == NULL)
        return;
    [g_tabs setFrame:[g_root bounds]];
    cr = [g_tabs contentRect];
    for (i = 0; i < (int)[g_tabs numberOfTabViewItems]; i++) {
        NSView *v = [[g_tabs tabViewItemAtIndex:i] view];
        if (v != nil)
            [v setFrame:cr];
    }
}

int main(int argc, char *argv[])
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    NSWindow      *win;
    NSRect         rc = NSMakeRect(0.0f, 0.0f, WIN_W, WIN_H);
    NSView        *root;
    Controller    *ctl;
    NSMenu        *menu, *submenu;
    NSMenuItem    *mi;
    NSTabViewItem *item;
    NSTimer       *timer;
    NSRect         cr;

    (void)argc;
    (void)argv;

    /* dev-хук: -w N -h N задают стартовый размер (проверка раскладки) */
    {
        NSArray *wa = [[NSProcessInfo processInfo] arguments];
        int i;
        for (i = 1; i < [wa count]; i++) {
            NSString *a = [wa objectAtIndex:i];
            if ([a isEqualToString:@"-w"] && i + 1 < [wa count]) {
                rc.size.width = [[wa objectAtIndex:i + 1] floatValue];
            } else if ([a isEqualToString:@"-h"] && i + 1 < [wa count]) {
                rc.size.height = [[wa objectAtIndex:i + 1] floatValue];
            }
        }
    }

    [NSApplication sharedApplication];
    ctl = [[Controller alloc] init];

    menu = [[NSMenu alloc] initWithTitle:
                [NSString stringWithUTF8String:"main"]];
    {
        NSMenuItem *appmi = [menu addItemWithTitle:
            [NSString stringWithUTF8String:"DiskGraph"]
            action:NULL keyEquivalent:[NSString stringWithUTF8String:""]];
        submenu = [[NSMenu alloc] initWithTitle:
            [NSString stringWithUTF8String:"DiskGraph"]];
        mi = [submenu addItemWithTitle:
            [NSString stringWithUTF8String:"Выход"]
            action:@selector(quit:)
            keyEquivalent:[NSString stringWithUTF8String:"q"]];
        [mi setTarget:ctl];
        [appmi setSubmenu:submenu];
        [submenu release];
    }
    [NSApp setMainMenu:menu];
    [menu release];

    win = [[NSWindow alloc]
             initWithContentRect:rc
             styleMask:NSTitledWindowMask |
                       NSClosableWindowMask |
                       NSMiniaturizableWindowMask |
                       NSResizableWindowMask
             backing:NSBackingStoreBuffered
             defer:NO];
    [win setTitle:[NSString stringWithUTF8String:
                    "DiskGraph - linear / random read test"]];
    [win setMinSize:NSMakeSize(760.0f, 560.0f)];
    [win setDelegate:ctl];
    root = [[[NSView alloc] initWithFrame:rc] autorelease];
    [root setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [win setContentView:root];
    g_root = root;

    g_tabs = [[NSTabView alloc] initWithFrame:[root bounds]];
    [g_tabs setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [root addSubview:g_tabs];

    cr = [g_tabs contentRect];

    item = [[NSTabViewItem alloc] initWithIdentifier:nil];
    [item setLabel:[NSString stringWithUTF8String:"Linear"]];
    [item setView:make_lin_tab(ctl, cr.size)];
    [g_tabs addTabViewItem:item];
    [item release];

    item = [[NSTabViewItem alloc] initWithIdentifier:nil];
    [item setLabel:[NSString stringWithUTF8String:"Random"]];
    [item setView:make_rnd_tab(ctl, cr.size)];
    [g_tabs addTabViewItem:item];
    [item release];

    sync_tabs_layout();

    {
        double c = probe_capacity(g_dev);
        if (c > 0.0)
            g_cap = c;
    }

    /* dev-хуки: -lin / -rnd стартуют реальный прогон, -cycle щёлкает
     * вкладки и печатает счётчики, -devs печатает список дисков */
    {
        NSArray *args = [[NSProcessInfo processInfo] arguments];
        int i;
        for (i = 1; i < [args count]; i++) {
            NSString *a = [args objectAtIndex:i];
            if ([a isEqualToString:@"-lin"]) {
                [g_tabs selectTabViewItemAtIndex:0];
                lin_start();
            } else if ([a isEqualToString:@"-rnd"]) {
                [g_tabs selectTabViewItemAtIndex:1];
                rnd_start();
            } else if ([a isEqualToString:@"-stop"] && i + 1 < [args count]) {
                g_stop_at = time(NULL) + [[args objectAtIndex:i + 1] intValue];
                i++;
            } else if ([a isEqualToString:@"-again"]) {
                g_again = 1;
            } else if ([a isEqualToString:@"-cycle"]) {
                g_cycle = 1;
                lin_start();
            } else if ([a isEqualToString:@"-devs"]) {
                NSArray *dl = scan_disks();
                int k;
                for (k = 0; k < (int)[dl count]; k++)
                    printf("%s\n", [[dl objectAtIndex:k] UTF8String]);
                exit(0);
            }
        }
    }

    [win center];
    [win makeKeyAndOrderFront:nil];

    timer = [NSTimer timerWithTimeInterval:TICK_SEC
                                    target:ctl
                                  selector:@selector(tick:)
                                  userInfo:nil
                                   repeats:YES];
    [[NSRunLoop currentRunLoop] addTimer:timer
                    forMode:NSDefaultRunLoopMode];

    [NSApp run];

    [g_tabs release];
    [ctl release];
    [win release];
    [pool release];
    return 0;
}
