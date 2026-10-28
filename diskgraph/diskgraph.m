/*
 * diskgraph.m - UI DiskGraph: вкладки Sequential read / Access map /
 *               Seek profile на РЕАЛЬНЫХ замерах через dglincore/dgrndcore
 *               + вкладка SMART на ядре ../smartinfo/smartcore (только
 *               чтение: статус, атрибуты, пороги)
 *
 * Фронт и бэк скрещены: вкладки зовут те же ядра, что и консольные
 * dblin/dbrnd, но данные приходят не в stdout, а в sink-калбеки:
 *   - Sequential read : сегмент -> точка (позиция, MB/s), кривая;
 *   - Access map      : оп -> точка (позиция на диске, лаг), позиции
 *                       равномерно по диску - карта времени доступа по
 *                       поверхности (зона, где сбоит позиционер или
 *                       поверхность, приподнята);
 *   - Seek profile    : хоп -> точка (дистанция хопа, лаг) на лог-оси X,
 *                       дистанции log-равномерно 8K..ёмкость (профиль
 *                       перемещения головки).
 * Ошибки чтения (EIO) - красные крестики на позиции (Sequential, Access).
 * Обе random-вкладки - один код (rndtab), разный режим ядра.
 *
 * Поток: UI-поток (AppKit) + один рабочий поток на запуск (pthread).
 * Рабочий поток только дописывает точки в массивы под мьютексом и
 * никогда не трогает ObjC. UI раз в 0.25 c снимает копию данных под
 * мьютексом и рисует уже без него - замер не ждёт отрисовку.
 * Одновременно идёт один прогон.
 *
 * Вкладки независимы: у каждой свой диск, своя ёмкость оси X и свои
 * данные; переключение вкладок данные не трогает.
 *
 * Права: чтение /dev/rdiskN требует root. При запуске приложение само
 * перезапускает себя через AuthorizationExecuteWithPrivileges (пароль
 * спрашивает SecurityAgent). Без прав работает и честно об этом пишет.
 *
 * Только ЧТЕНИЕ. Запись не выполняется никогда.
 *
 * ObjC строго objc1 (Jaguar..Leopard runtime): без свойств и fast
 * enumeration; @"..."-литералы только ASCII; только API 10.2.
 */
#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#include <dirent.h>
#include <sys/types.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <math.h>
#include <mach-o/dyld.h>
#include <Security/Authorization.h>
#include <Security/AuthorizationTags.h>

#include "dglincore.h"
#include "dgrndcore.h"
#include "dgdev.h"
#include "smartcore.h"

/* ---- параметры приборов (как в консольных тулзах) ---- */
#define LIN_SEG (32ULL << 20)      /* 32 MiB чанк  */
#define LIN_BLK (1ULL  << 20)      /* 1 MiB блок   */
#define RND_BLK 4096ULL            /* 4 KiB        */
#define RND_WARMUP 16
#define TICK_SEC 0.25

#define WIN_W 1000.0f
#define WIN_H  720.0f

#define GIB 1073741824.0
#define SEEK_XMIN 4096.0           /* левый край лог-оси хопов */
#define NBIN 24                    /* корзин по дистанции       */

/* g_run: какой прогон идёт */
enum { RUN_IDLE = 0, RUN_LIN = 1, RUN_ACC = 2, RUN_SEEK = 3 };

/* ------------------------------------------------------------ данные */

typedef struct {
    double x;     /* linear: позиция; random: дистанция, байты */
    double val;   /* MB/s (linear) / µs (random)               */
    int    tag;   /* random: +1 вперёд, -1 назад                */
} dpt;

/* растущий массив точек: ёмкость удваивается, без realloc на точку */
typedef struct {
    dpt *a;
    int  n;
    int  alloc;
} dbuf;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static int g_run = RUN_IDLE;            /* UI-поток */
static time_t g_run_t0 = 0;             /* UI-поток */

/* ---- Sequential read (под g_lock, кроме помеченного) ---- */
static dbuf g_lin = { NULL, 0, 0 };
static dbuf g_lin_errs = { NULL, 0, 0 };    /* x = позиция EIO-блока */
static volatile int g_lin_cancel = 0;
static int  g_lin_done = 0;
static int  g_lin_failed = 0;
static int  g_lin_oserr = 0;
static char g_lin_err[256] = "";
static dglinresult g_lin_res;
static uint64_t g_lin_total = 0;        /* сегментов в плане */
static uint64_t g_lin_bytes = 0;        /* суммы для живого avg */
static uint64_t g_lin_time_us = 0;
static int      g_lin_idx_done = 0;     /* сегментов завершено */
static int      g_lin_elapsed = 0;      /* длительность прогона, с */
static double   g_lin_datacap = 0.0;    /* ёмкость прочитанного диска */

/* UI-поток */
static char   g_lin_sel[64] = "/dev/rdisk0";
static double g_lin_selcap = 0.0;
static char   g_lin_path[64] = "";      /* снимок на Старт */
static int    g_lin_duty = 5;
static int    g_duty = 5;

/* ---- Access map / Seek profile: одна структура на вкладку ---- */
typedef struct {
    int          mode;          /* DGRND_ACCESS / DGRND_SEEK          */
    int          run_id;        /* RUN_ACC / RUN_SEEK                 */
    const char  *name;          /* "Access map"                       */

    /* под g_lock */
    dbuf         pts;
    dbuf         errs;          /* EIO: x = позиция (access)          */
    volatile int cancel;
    int          done, failed, oserr, elapsed;
    char         err[256];
    dgrndresult  res;
    double       datacap;

    /* UI-поток */
    char         sel[64];
    double       selcap;
    char         path[64];      /* снимок на Старт                    */
    int          secs, secs_run;
    int          stat_n;        /* по скольким точкам посчитано       */
    double       med, p99;
    double       short_med, long_med;   /* seek: <1 MB и > 1/2 диска */
    int          bn[NBIN];
    double       bmed[NBIN], blo[NBIN];

    NSPopUpButton *dev;
    NSSlider      *slider;
    NSTextField   *val;
    NSTextField   *status;
    NSButton      *start, *stop;
    NSView        *view;
} rndtab;

static rndtab g_acc;
static rndtab g_seek;

/* 1 - процесс с правами root (иначе чтение rdisk упрётся в EACCES) */
static int g_is_root = 0;

/* ---- UI Linear ---- */
static NSTextField   *g_lin_status = NULL;
static NSTextField   *g_duty_val  = NULL;
static NSButton      *g_lin_start = NULL;
static NSButton      *g_lin_stop  = NULL;
static NSSlider      *g_lin_slider = NULL;
static NSView        *g_lin_view = NULL;
static NSPopUpButton *g_lin_dev = NULL;
static NSTabView     *g_tabs = NULL;

/* dev-хук -cycle */
static int g_cycle = 0;
static int g_cycle_ticks = 0;

/* dev-хук -stop N: через N секунд после старта нажать "Стоп" */
static time_t g_stop_at = 0;

/* dev-хук -again: после завершения прогона сразу стартовать снова */
static int g_again = 0;

/* dev-хук -close N: через N секунд закрыть окно (как красной кнопкой) */
static time_t    g_close_at = 0;
static NSWindow *g_win = NULL;

static void rndtab_init(rndtab *t, int mode, int run_id, const char *name,
                        int secs)
{
    memset(t, 0, sizeof *t);
    t->mode   = mode;
    t->run_id = run_id;
    t->name   = name;
    t->secs   = secs;
    t->stat_n = -1;
    strcpy(t->sel, "/dev/rdisk0");
}

static rndtab *rndtab_of_run(int run)
{
    if (run == RUN_ACC)  return &g_acc;
    if (run == RUN_SEEK) return &g_seek;
    return NULL;
}

/* вызывать под g_lock */
static int pts_add(dbuf *b, double x, double val, int tag)
{
    if (b->n == b->alloc) {
        int  na = (b->alloc > 0) ? b->alloc * 2 : 1024;
        dpt *p  = (dpt *)realloc(b->a, (size_t)na * sizeof(dpt));
        if (p == NULL)
            return 0;
        b->a = p;
        b->alloc = na;
    }
    b->a[b->n].x   = x;
    b->a[b->n].val = val;
    b->a[b->n].tag = tag;
    b->n++;
    return 1;
}

/* вызывать под g_lock: копия src в dst (для рисования без лока) */
static void pts_copy(const dbuf *src, dbuf *dst)
{
    if (dst->alloc < src->n) {
        dpt *p = (dpt *)realloc(dst->a, (size_t)src->alloc * sizeof(dpt));
        if (p == NULL) {
            dst->n = 0;
            return;
        }
        dst->a = p;
        dst->alloc = src->alloc;
    }
    if (src->n > 0)
        memcpy(dst->a, src->a, (size_t)src->n * sizeof(dpt));
    dst->n = src->n;
}

static double mbs_of(uint64_t bytes, uint64_t us)
{
    return us > 0 ? (double)bytes / ((double)us / 1e6) / 1048576.0 : 0.0;
}

/* ------------------------------------------------------- оси X */

/* доля 0..1 по оси X: random seek - лог-ось от SEEK_XMIN, иначе линейная */
static double x_frac(int xlog, double x, double cap)
{
    double f;

    if (cap <= 0.0)
        return 0.0;
    if (xlog) {
        if (cap <= SEEK_XMIN)
            return 0.0;
        if (x < SEEK_XMIN)
            x = SEEK_XMIN;
        f = log(x / SEEK_XMIN) / log(cap / SEEK_XMIN);
    } else {
        f = x / cap;
    }
    if (f < 0.0) f = 0.0;
    if (f > 1.0) f = 1.0;
    return f;
}

static int bin_of(int xlog, double x, double cap)
{
    int b = (int)(x_frac(xlog, x, cap) * (double)NBIN);
    return (b >= NBIN) ? NBIN - 1 : b;
}

static double rnd_cap(const rndtab *t, int npts)
{
    /* ось по прочитанному диску, если данные есть или идёт прогон */
    if ((npts > 0 || g_run == t->run_id) && t->datacap > 0.0)
        return t->datacap;
    return t->selcap;
}

/* ------------------------------------------------------- статистика */

typedef struct {
    int    b;
    double v;
} binval;

static int bv_cmp(const void *a, const void *b)
{
    const binval *x = (const binval *)a, *y = (const binval *)b;
    if (x->b != y->b)
        return x->b - y->b;
    return (x->v > y->v) - (x->v < y->v);
}

static double median_sorted(const double *v, int n)
{
    return n > 0 ? dg_pct(v, (size_t)n, 0.50) : 0.0;
}

/*
 * UI-поток: med/p99, медиана и p10 по корзинам дистанции, для seek -
 * медианы коротких (<1 MB) и длинных (> 1/2 диска) хопов. Копия под
 * локом, сортировки вне лока; пересчёт только если точек прибавилось.
 */
static void rnd_stats_update(rndtab *t)
{
    static double *v = NULL, *sv = NULL, *lv = NULL;
    static binval *bv = NULL;
    static int     valloc = 0;
    int            n, i, ns = 0, nl = 0, xlog = (t->mode == DGRND_SEEK);
    double         cap;

    pthread_mutex_lock(&g_lock);
    n = t->pts.n;
    if (n == t->stat_n) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    if (n > valloc) {
        int a = t->pts.alloc;
        double *p1 = (double *)realloc(v,  (size_t)a * sizeof(double));
        double *p2 = (double *)realloc(sv, (size_t)a * sizeof(double));
        double *p3 = (double *)realloc(lv, (size_t)a * sizeof(double));
        binval *p4 = (binval *)realloc(bv, (size_t)a * sizeof(binval));
        if (p1) v = p1;
        if (p2) sv = p2;
        if (p3) lv = p3;
        if (p4) bv = p4;
        if (!p1 || !p2 || !p3 || !p4) {
            pthread_mutex_unlock(&g_lock);
            return;
        }
        valloc = a;
    }
    cap = rnd_cap(t, n);
    for (i = 0; i < n; i++) {
        const dpt *p = &t->pts.a[i];
        v[i] = p->val;
        bv[i].b = bin_of(xlog, p->x, cap);
        bv[i].v = p->val;
        if (p->x < 1048576.0)
            sv[ns++] = p->val;
        else if (p->x > cap * 0.5)
            lv[nl++] = p->val;
    }
    pthread_mutex_unlock(&g_lock);

    qsort(v, (size_t)n, sizeof(double), dg_dcmp);
    t->med = dg_pct(v, (size_t)n, 0.50);
    t->p99 = dg_pct(v, (size_t)n, 0.99);

    qsort(sv, (size_t)ns, sizeof(double), dg_dcmp);
    qsort(lv, (size_t)nl, sizeof(double), dg_dcmp);
    t->short_med = median_sorted(sv, ns);
    t->long_med  = median_sorted(lv, nl);

    qsort(bv, (size_t)n, sizeof(binval), bv_cmp);
    for (i = 0; i < NBIN; i++)
        t->bn[i] = 0;
    for (i = 0; i < n; ) {
        int b = bv[i].b, j = i, k;
        while (j < n && bv[j].b == b)
            j++;
        k = j - i;
        t->bn[b]   = k;
        t->bmed[b] = bv[i + (int)(0.50 * (double)(k - 1) + 0.5)].v;
        t->blo[b]  = bv[i + (int)(0.10 * (double)(k - 1) + 0.5)].v;
        i = j;
    }
    t->stat_n = n;
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

/* /dev/diskN идёт через кеш блоков ядра порциями по 4K (F_NOCACHE на
 * него не действует) - в меню помечен, чтобы не путать с замером */
static NSString *dev_title(NSString *path)
{
    if (strncmp([path UTF8String], "/dev/disk", 9) == 0)
        return [path stringByAppendingString:@" (buffered)"];
    return path;
}

/* путь из заголовка пункта: до первого пробела */
static void title_to_path(NSString *t, char *out, size_t outsz)
{
    const char *u = [t UTF8String];
    size_t      i;

    for (i = 0; u != NULL && u[i] != 0 && u[i] != ' ' && i + 1 < outsz; i++)
        out[i] = u[i];
    out[i] = 0;
}

/* заполнить popup и выставить в нём sel; если sel пропал - первый диск
 * (sel перезаписывается). Возврат 1 - выбор изменился. */
static int fill_dev_popup(NSPopUpButton *pop, char *sel, size_t selsz)
{
    NSArray *disks;
    int      i, idx = -1;

    if (pop == NULL)
        return 0;

    disks = scan_disks();
    [pop removeAllItems];

    for (i = 0; i < (int)[disks count]; i++) {
        NSString *t = [disks objectAtIndex:i];
        [pop addItemWithTitle:dev_title(t)];
        if (strcmp([t UTF8String], sel) == 0)
            idx = i;
    }

    if ([disks count] == 0) {
        [pop addItemWithTitle:[NSString stringWithUTF8String:"(нет дисков)"]];
        [pop selectItemAtIndex:0];
        return 0;
    }
    if (idx >= 0) {
        [pop selectItemAtIndex:idx];
        return 0;
    }
    [pop selectItemAtIndex:0];
    strncpy(sel, [[disks objectAtIndex:0] UTF8String], selsz - 1);
    sel[selsz - 1] = 0;
    return 1;
}

/* ------------------------------------------------------- шрифты/текст */

/*
 * C-строка -> NSString без nil: если snprintf/strncat обрезали русский
 * текст посреди UTF-8-последовательности, хвост отбрасывается (nil в
 * setStringValue: на старых AppKit - исключение).
 */
static NSString *u8(const char *s)
{
    NSString *r = [NSString stringWithUTF8String:s];
    char      b[1024];
    size_t    n;

    if (r != nil)
        return r;
    snprintf(b, sizeof b, "%s", s);
    n = strlen(b);
    while (n > 0 && ((unsigned char)b[n - 1] & 0x80))
        b[--n] = 0;                 /* до последнего ASCII-байта */
    r = [NSString stringWithUTF8String:b];
    return r != nil ? r : @"";
}

static NSColor *rgb(float r, float g, float b, float a)
{
    return [NSColor colorWithCalibratedRed:r green:g blue:b alpha:a];
}

static NSColor *col_line(void)    { return rgb(0.13f, 0.40f, 0.82f, 1.0f); }
static NSColor *col_fill(void)    { return rgb(0.13f, 0.40f, 0.82f, 0.13f); }
static NSColor *col_dot(void)     { return rgb(0.10f, 0.25f, 0.55f, 0.50f); }
static NSColor *col_dot_fwd(void) { return rgb(0.05f, 0.60f, 0.60f, 0.55f); }
static NSColor *col_spike(void)   { return rgb(0.85f, 0.15f, 0.12f, 0.85f); }
static NSColor *col_avg(void)     { return rgb(0.10f, 0.55f, 0.30f, 1.0f); }
static NSColor *col_p99(void)     { return rgb(0.85f, 0.35f, 0.10f, 1.0f); }

static NSDictionary *attrs(float size, int bold, NSColor *c)
{
    NSFont *f = bold ? [NSFont boldSystemFontOfSize:size]
                     : [NSFont systemFontOfSize:size];
    return [NSDictionary dictionaryWithObjectsAndKeys:
                f, NSFontAttributeName,
                c, NSForegroundColorAttributeName,
                NULL];
}

/* подписи тиков: кеш, рисуются десятками за кадр */
static NSDictionary *tick_attrs(void)
{
    static NSDictionary *d = NULL;
    if (d == NULL)
        d = [attrs(10.0f, 0, [NSColor colorWithCalibratedWhite:0.30f
                                                         alpha:1.0f]) retain];
    return d;
}

static NSSize text_size(const char *s, NSDictionary *a)
{
    return [[NSString stringWithUTF8String:s] sizeWithAttributes:a];
}

static void draw_text(const char *s, NSPoint p, NSDictionary *a)
{
    [[NSString stringWithUTF8String:s] drawAtPoint:p withAttributes:a];
}

/* ------------------------------------------------------- шкалы */

/* круглые мантиссы для приборной шкалы */
static const double k_cand[10] = {1.0, 1.2, 1.5, 2.0, 2.5,
                                  3.0, 4.0, 5.0, 6.0, 8.0};

static int is_nice(double x)
{
    double m;
    int    i;

    if (x <= 0.0)
        return 0;
    m = x;
    while (m >= 10.0) m /= 10.0;
    while (m < 1.0)   m *= 10.0;
    for (i = 0; i < 10; i++)
        if (fabs(m - k_cand[i]) < 1e-9)
            return 1;
    return 0;
}

static double nice_up(double x)
{
    double exp = 1.0;
    double f;
    int    i;

    if (x <= 0.0)
        return 1.0;
    while (x / exp >= 10.0) exp *= 10.0;
    while (x / exp < 1.0)   exp /= 10.0;
    f = x / exp;
    for (i = 0; i < 10; i++)
        if (f <= k_cand[i] + 1e-12)
            return k_cand[i] * exp;
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

/* шаг меток X (GB) из ряда 1-2-5 под ширину: не теснее ~70 px */
static double x_step_gb(double capgb, float width)
{
    static const double c[4] = {1.0, 2.0, 5.0, 10.0};
    double raw, exp = 1.0;
    int    maxt, i;

    maxt = (int)(width / 70.0f);
    if (maxt < 2)
        maxt = 2;
    raw = capgb / (double)maxt;
    if (raw <= 0.0)
        return 1.0;
    while (raw / exp >= 10.0) exp *= 10.0;
    while (raw / exp < 1.0)   exp /= 10.0;
    for (i = 0; i < 4; i++)
        if (raw / exp <= c[i] + 1e-12)
            return c[i] * exp;
    return 10.0 * exp;
}

/* 4096 -> "4K", 268435456 -> "256M", 4294967296 -> "4G" */
static void fmt_bytes(double b, char *out, size_t outsz)
{
    if (b >= GIB)
        snprintf(out, outsz, "%.0fG", b / GIB);
    else if (b >= 1048576.0)
        snprintf(out, outsz, "%.0fM", b / 1048576.0);
    else
        snprintf(out, outsz, "%.0fK", b / 1024.0);
}

/* ------------------------------------------------------- рисование */

typedef struct {
    NSRect pv;      /* область построения */
    double cap;     /* правый край оси X, байты; <= 0 - неизвестен */
    int    xlog;    /* 1 - лог-ось X от SEEK_XMIN */
    double vmax;    /* ось Y; <= 0 - данных нет, шкалы нет */
    int    ndiv;
} plotgeo;

static float geo_x(const plotgeo *g, double x)
{
    return NSMinX(g->pv) + (float)x_frac(g->xlog, x, g->cap) *
                           g->pv.size.width;
}

static NSPoint geo_pt(const plotgeo *g, double x, double val)
{
    double fy = (g->vmax > 0.0) ? val / g->vmax : 0.0;
    if (fy > 1.0) fy = 1.0;
    return NSMakePoint(geo_x(g, x),
                       NSMinY(g->pv) + (float)fy * g->pv.size.height);
}

static NSRect plot_rect(NSRect bounds)
{
    float padl = 52.0f, padr = 16.0f, padt = 24.0f, padb = 40.0f;
    return NSMakeRect(padl, padb,
                      NSWidth(bounds) - padl - padr,
                      NSHeight(bounds) - padt - padb);
}

/* метка X: вертикаль сетки + тик + подпись */
static void x_tick(const plotgeo *g, NSBezierPath *grid, NSBezierPath *ticks,
                   float x, const char *lbl, int with_grid)
{
    NSSize ts = text_size(lbl, tick_attrs());

    if (with_grid && x > NSMinX(g->pv) + 0.5f && x < NSMaxX(g->pv) - 0.5f) {
        [grid moveToPoint:NSMakePoint(x, NSMinY(g->pv))];
        [grid lineToPoint:NSMakePoint(x, NSMaxY(g->pv))];
    }
    [ticks moveToPoint:NSMakePoint(x, NSMinY(g->pv))];
    [ticks lineToPoint:NSMakePoint(x, NSMinY(g->pv) - 4.0f)];
    draw_text(lbl, NSMakePoint(x - ts.width * 0.5f, NSMinY(g->pv) - 17.0f),
              tick_attrs());
}

/* подложка, сетка, оси и подписи; ylab - имя оси Y, ydiv - делитель
 * для подписей (1 - как есть, 1000 - µs в ms), xlab - имя оси X */
static void draw_frame(const plotgeo *g, const char *ylab, double ydiv,
                       const char *xlab)
{
    NSRect pv = g->pv;
    float  x0 = NSMinX(pv), y0 = NSMinY(pv);
    float  x1 = NSMaxX(pv), y1 = NSMaxY(pv);
    float  dash[2] = {2.0f, 3.0f};
    NSBezierPath *grid = [NSBezierPath bezierPath];
    NSBezierPath *ticks = [NSBezierPath bezierPath];
    NSBezierPath *box;
    int    i;

    /* белая область построения в тонкой рамке */
    [[NSColor whiteColor] set];
    NSRectFill(pv);

    if (g->vmax > 0.0) {
        for (i = 1; i < g->ndiv; i++) {
            float y = y0 + ((float)i / (float)g->ndiv) * pv.size.height;
            [grid moveToPoint:NSMakePoint(x0, y)];
            [grid lineToPoint:NSMakePoint(x1, y)];
        }
    }

    if (g->cap > 0.0 && g->xlog) {
        /* лог-ось: степени 4 от 4K, при тесноте - степени 16 */
        double mul = 4.0, b;
        if (geo_x(g, SEEK_XMIN * 4.0) - x0 < 34.0f)
            mul = 16.0;
        for (b = SEEK_XMIN; b <= g->cap * 1.0001; b *= mul) {
            char lbl[16];
            fmt_bytes(b, lbl, sizeof lbl);
            x_tick(g, grid, ticks, geo_x(g, b), lbl, 1);
        }
    } else if (g->cap > 0.0) {
        double capgb = g->cap / GIB;
        double step  = x_step_gb(capgb, pv.size.width);
        int    k;

        for (k = 0; step * (double)k <= capgb + 1e-9; k++) {
            double gb = step * (double)k;
            char   lbl[24];
            if (step >= 1.0)
                snprintf(lbl, sizeof lbl, "%.0f", gb);
            else
                snprintf(lbl, sizeof lbl, "%.1f", gb);
            x_tick(g, grid, ticks, x0 + (float)(gb / capgb) * pv.size.width,
                   lbl, 1);
        }
    }

    [[NSColor colorWithCalibratedWhite:0.86f alpha:1.0f] set];
    [grid setLineWidth:1.0f];
    [grid setLineDash:dash count:2 phase:0.0f];
    [grid stroke];

    if (g->vmax > 0.0) {
        for (i = 0; i <= g->ndiv; i++) {
            float  y   = y0 + ((float)i / (float)g->ndiv) * pv.size.height;
            double val = g->vmax * ((double)i / (double)g->ndiv) / ydiv;
            char   lbl[32];
            NSSize ts;

            [ticks moveToPoint:NSMakePoint(x0, y)];
            [ticks lineToPoint:NSMakePoint(x0 - 4.0f, y)];
            if (g->vmax / ydiv < 10.0)
                snprintf(lbl, sizeof lbl, "%.1f", val);
            else
                snprintf(lbl, sizeof lbl, "%.0f", val);
            ts = text_size(lbl, tick_attrs());
            draw_text(lbl, NSMakePoint(x0 - 7.0f - ts.width, y - 7.0f),
                      tick_attrs());
        }
    }

    [[NSColor colorWithCalibratedWhite:0.55f alpha:1.0f] set];
    [ticks setLineWidth:1.0f];
    [ticks stroke];

    box = [NSBezierPath bezierPathWithRect:NSInsetRect(pv, -0.5f, -0.5f)];
    [[NSColor colorWithCalibratedWhite:0.72f alpha:1.0f] set];
    [box setLineWidth:1.0f];
    [box stroke];

    if (g->cap > 0.0) {
        NSDictionary *a = attrs(10.0f, 0,
            [NSColor colorWithCalibratedWhite:0.40f alpha:1.0f]);
        NSSize ts = text_size(xlab, a);
        draw_text(xlab, NSMakePoint(x0 + (pv.size.width - ts.width) * 0.5f,
                                    y0 - 33.0f), a);
    }
    if (g->vmax > 0.0)
        draw_text(ylab, NSMakePoint(x0 - 4.0f, y1 + 5.0f),
                  attrs(10.0f, 1,
                        [NSColor colorWithCalibratedWhite:0.25f alpha:1.0f]));
}

/* серая подсказка по центру пустой области */
static void draw_hint(NSRect pv, const char *s)
{
    NSDictionary *a = attrs(13.0f, 0,
        [NSColor colorWithCalibratedWhite:0.55f alpha:1.0f]);
    NSSize ts = text_size(s, a);

    draw_text(s, NSMakePoint(NSMidX(pv) - ts.width * 0.5f,
                             NSMidY(pv) - ts.height * 0.5f), a);
}

/* горизонтальная пунктирная отметка с подписью у правого края */
static void draw_hmark(const plotgeo *g, double val, NSColor *c,
                       const char *lbl)
{
    NSPoint p = geo_pt(g, 0.0, val);
    float   dash[2] = {5.0f, 3.0f};
    NSBezierPath *bp = [NSBezierPath bezierPath];
    NSDictionary *a = attrs(10.0f, 1, c);
    NSSize ts = text_size(lbl, a);
    NSRect bg;

    if (val > g->vmax)
        return;
    [bp moveToPoint:NSMakePoint(NSMinX(g->pv), p.y)];
    [bp lineToPoint:NSMakePoint(NSMaxX(g->pv), p.y)];
    [bp setLineWidth:1.0f];
    [bp setLineDash:dash count:2 phase:0.0f];
    [c set];
    [bp stroke];

    bg = NSMakeRect(NSMaxX(g->pv) - ts.width - 8.0f, p.y + 2.0f,
                    ts.width + 4.0f, ts.height);
    if (NSMaxY(bg) > NSMaxY(g->pv))
        bg.origin.y = p.y - ts.height - 2.0f;
    [[NSColor colorWithCalibratedWhite:1.0f alpha:0.85f] set];
    NSRectFillUsingOperation(bg, NSCompositeSourceOver);
    draw_text(lbl, NSMakePoint(bg.origin.x + 2.0f, bg.origin.y), a);
}

/* легенда в правом верхнем углу над графиком: цветной квадрат + текст */
static float draw_legend(const plotgeo *g, float right, NSColor *c,
                         const char *lbl, int dashed_line)
{
    NSDictionary *a = attrs(10.0f, 0,
        [NSColor colorWithCalibratedWhite:0.30f alpha:1.0f]);
    NSSize ts = text_size(lbl, a);
    float  y = NSMaxY(g->pv) + 5.0f;
    float  x = right - ts.width;

    draw_text(lbl, NSMakePoint(x, y), a);
    [c set];
    if (dashed_line < 0) {
        NSRectFillUsingOperation(NSMakeRect(x - 11.0f, y + 4.0f, 6.0f, 6.0f),
                                 NSCompositeSourceOver);
    } else {
        float dash[2] = {3.0f, 2.0f};
        NSBezierPath *bp = [NSBezierPath bezierPath];
        [bp moveToPoint:NSMakePoint(x - 18.0f, y + 7.0f)];
        [bp lineToPoint:NSMakePoint(x - 4.0f, y + 7.0f)];
        [bp setLineWidth:2.0f];
        if (dashed_line)
            [bp setLineDash:dash count:2 phase:0.0f];
        [bp stroke];
    }
    return x - 32.0f;
}

static const char *idle_hint(void)
{
    if (!g_is_root)
        return "нет прав на raw-диск - перезапустите приложение "
               "и введите пароль";
    return "выберите диск и нажмите Старт";
}

/* EIO: красные крестики у нижнего края на позиции ошибки */
static int draw_eio(const plotgeo *g, const dbuf *errs)
{
    NSBezierPath *bp = [NSBezierPath bezierPath];
    float y = NSMinY(g->pv) + 6.0f;
    int   i;

    for (i = 0; i < errs->n; i++) {
        float x = geo_x(g, errs->a[i].x);
        [bp moveToPoint:NSMakePoint(x - 4.0f, y - 4.0f)];
        [bp lineToPoint:NSMakePoint(x + 4.0f, y + 4.0f)];
        [bp moveToPoint:NSMakePoint(x - 4.0f, y + 4.0f)];
        [bp lineToPoint:NSMakePoint(x + 4.0f, y - 4.0f)];
    }
    [bp setLineWidth:2.0f];
    [col_spike() set];
    [bp stroke];
    return errs->n;
}

@interface LinView : NSView
@end

@implementation LinView

- (void)drawRect:(NSRect)dirty
{
    static dbuf snap = { NULL, 0, 0 };
    static dbuf esnap = { NULL, 0, 0 };
    plotgeo g;
    double  raw = 0.0;
    int     i, running = (g_run == RUN_LIN);
    uint64_t bytes, us;

    (void)dirty;

    pthread_mutex_lock(&g_lock);
    pts_copy(&g_lin, &snap);
    pts_copy(&g_lin_errs, &esnap);
    g.cap  = (snap.n > 0 || running) ? g_lin_datacap : 0.0;
    bytes  = g_lin_bytes;
    us     = g_lin_time_us;
    pthread_mutex_unlock(&g_lock);

    if (g.cap <= 0.0)
        g.cap = g_lin_selcap;
    g.xlog = 0;

    for (i = 0; i < snap.n; i++)
        if (snap.a[i].val > raw)
            raw = snap.a[i].val;

    g.pv   = plot_rect([self bounds]);
    g.ndiv = 4;
    g.vmax = (raw > 0.0) ? axis_vmax(raw, &g.ndiv) : 0.0;

    draw_frame(&g, "MB/s", 1.0, "позиция на диске, GB");
    if (esnap.n > 0) {
        char lbl[32];
        draw_eio(&g, &esnap);
        snprintf(lbl, sizeof lbl, "EIO: %d", esnap.n);
        draw_legend(&g, NSMaxX(g.pv), col_spike(), lbl, -1);
    }

    if (snap.n == 0) {
        draw_hint(g.pv, running ? "открываю диск..." : idle_hint());
        return;
    }

    {
        NSBezierPath *line = [NSBezierPath bezierPath];
        NSBezierPath *area = [NSBezierPath bezierPath];
        NSPoint first = geo_pt(&g, snap.a[0].x, snap.a[0].val);
        NSPoint last  = first;

        [area moveToPoint:NSMakePoint(first.x, NSMinY(g.pv))];
        for (i = 0; i < snap.n; i++) {
            NSPoint p = geo_pt(&g, snap.a[i].x, snap.a[i].val);
            if (i == 0)
                [line moveToPoint:p];
            else
                [line lineToPoint:p];
            [area lineToPoint:p];
            last = p;
        }
        [area lineToPoint:NSMakePoint(last.x, NSMinY(g.pv))];
        [area closePath];

        [col_fill() set];
        [area fill];

        [col_line() set];
        [line setLineWidth:1.5f];
        [line setLineJoinStyle:NSRoundLineJoinStyle];
        [line stroke];

        /* где сейчас головка */
        if (running) {
            float dash[2] = {3.0f, 3.0f};
            NSBezierPath *cur = [NSBezierPath bezierPath];
            [cur moveToPoint:NSMakePoint(last.x, NSMinY(g.pv))];
            [cur lineToPoint:NSMakePoint(last.x, NSMaxY(g.pv))];
            [cur setLineDash:dash count:2 phase:0.0f];
            [[NSColor colorWithCalibratedWhite:0.55f alpha:1.0f] set];
            [cur stroke];
        }
    }

    if (us > 0) {
        char lbl[48];
        double avg = mbs_of(bytes, us);
        snprintf(lbl, sizeof lbl, "avg %.1f MB/s", avg);
        draw_hmark(&g, avg, col_avg(), lbl);
    }
}

@end

/* Access map: лаг vs позиция; Seek profile: лаг vs дистанция хопа */
@interface RndView : NSView
{
    rndtab *tab;
}
- (void)setTab:(rndtab *)t;
@end

@implementation RndView

- (void)setTab:(rndtab *)t
{
    tab = t;
}

- (void)drawRect:(NSRect)dirty
{
    static dbuf snap = { NULL, 0, 0 };
    static dbuf esnap = { NULL, 0, 0 };
    rndtab *t = tab;
    plotgeo g;
    double  raw = 0.0, thr = 1e30;
    int     i, over = 0, seek, running;

    (void)dirty;
    if (t == NULL)
        return;
    seek    = (t->mode == DGRND_SEEK);
    running = (g_run == t->run_id);

    pthread_mutex_lock(&g_lock);
    pts_copy(&t->pts, &snap);
    pts_copy(&t->errs, &esnap);
    g.cap = rnd_cap(t, snap.n);
    pthread_mutex_unlock(&g_lock);
    g.xlog = seek;

    for (i = 0; i < snap.n; i++)
        if (snap.a[i].val > raw)
            raw = snap.a[i].val;

    /* единичный выброс (рекалибровка, спин-ап) не сплющивает облако:
     * шкала по 2*p99, всё выше - красной меткой у верхнего края */
    if (snap.n >= 100 && t->p99 > 0.0) {
        if (!seek)
            thr = t->p99;
        if (raw > 3.0 * t->p99)
            raw = 2.0 * t->p99;
    }

    g.pv   = plot_rect([self bounds]);
    g.ndiv = 5;
    g.vmax = (raw > 0.0) ? axis_vmax(raw, &g.ndiv) : 0.0;

    draw_frame(&g, "ms", 1000.0,
               seek ? "дистанция хопа (лог. шкала)"
                    : "позиция на диске, GB");

    if (snap.n == 0) {
        draw_hint(g.pv, running ? "открываю диск, прогрев..." : idle_hint());
        return;
    }

    {
        NSBezierPath *dots   = [NSBezierPath bezierPath];
        NSBezierPath *fwd    = [NSBezierPath bezierPath];
        NSBezierPath *spikes = [NSBezierPath bezierPath];

        for (i = 0; i < snap.n; i++) {
            NSPoint p = geo_pt(&g, snap.a[i].x, snap.a[i].val);
            NSRect  r = NSMakeRect(p.x - 1.5f, p.y - 1.5f, 3.0f, 3.0f);
            if (snap.a[i].val > g.vmax) {
                /* за шкалой: треугольник остриём вверх у края */
                float y = NSMaxY(g.pv) - 1.0f;
                [spikes moveToPoint:NSMakePoint(p.x - 3.0f, y - 5.0f)];
                [spikes lineToPoint:NSMakePoint(p.x + 3.0f, y - 5.0f)];
                [spikes lineToPoint:NSMakePoint(p.x, y)];
                [spikes closePath];
                over++;
            } else if (snap.a[i].val > thr) {
                [spikes appendBezierPathWithRect:r];
            } else if (seek && snap.a[i].tag > 0) {
                [fwd appendBezierPathWithRect:r];
            } else {
                [dots appendBezierPathWithRect:r];
            }
        }
        [col_dot() set];
        [dots fill];
        [col_dot_fwd() set];
        [fwd fill];
        [col_spike() set];
        [spikes fill];
    }

    /* кривые по корзинам дистанции: медиана и p10 (≈ чистый seek) */
    if (t->stat_n >= 50) {
        NSBezierPath *med = [NSBezierPath bezierPath];
        NSBezierPath *lo  = [NSBezierPath bezierPath];
        float  dash[2] = {4.0f, 2.0f};
        int    started = 0;

        for (i = 0; i < NBIN; i++) {
            double fx;
            float  x;
            NSPoint pm, pl;

            if (t->bn[i] < 5)
                continue;
            fx = ((double)i + 0.5) / (double)NBIN;
            x  = NSMinX(g.pv) + (float)fx * g.pv.size.width;
            pm = geo_pt(&g, 0.0, t->bmed[i]);
            pl = geo_pt(&g, 0.0, t->blo[i]);
            pm.x = pl.x = x;
            if (!started) {
                [med moveToPoint:pm];
                [lo moveToPoint:pl];
                started = 1;
            } else {
                [med lineToPoint:pm];
                [lo lineToPoint:pl];
            }
        }
        [med setLineWidth:2.0f];
        [med setLineJoinStyle:NSRoundLineJoinStyle];
        [lo setLineWidth:2.0f];
        [lo setLineJoinStyle:NSRoundLineJoinStyle];
        [lo setLineDash:dash count:2 phase:0.0f];
        [col_avg() set];
        [med stroke];
        [col_p99() set];
        [lo stroke];
    }

    if (!seek && snap.n >= 20) {
        char lbl[48];
        snprintf(lbl, sizeof lbl, "p99 %.1f ms", t->p99 / 1000.0);
        draw_hmark(&g, t->p99, col_spike(), lbl);
    }

    {
        float right = NSMaxX(g.pv);
        if (esnap.n > 0) {
            char lbl[32];
            if (!seek)
                draw_eio(&g, &esnap);   /* у seek нет дистанции ошибки */
            snprintf(lbl, sizeof lbl, "EIO: %d", esnap.n);
            right = draw_legend(&g, right, col_spike(), lbl, -1);
        }
        if (over > 0) {
            char lbl[48];
            snprintf(lbl, sizeof lbl, "выше шкалы: %d", over);
            right = draw_legend(&g, right, col_spike(), lbl, -1);
        }
        right = draw_legend(&g, right, col_p99(),
                            seek ? "p10 (≈ seek)" : "p10", 1);
        right = draw_legend(&g, right, col_avg(), "медиана", 0);
        if (seek) {
            right = draw_legend(&g, right, col_dot(), "назад", -1);
            right = draw_legend(&g, right, col_dot_fwd(), "вперёд", -1);
        }
    }
}

@end

/* ------------------------------------------------------- статус-строки */

enum { ST_NORMAL, ST_DONE, ST_ERROR };

static void set_status(NSTextField *f, const char *s, int kind)
{
    if (f == NULL)
        return;
    [f setStringValue:u8(s)];
    [f setFont:(kind == ST_DONE) ? [NSFont boldSystemFontOfSize:11.0f]
                                 : [NSFont systemFontOfSize:11.0f]];
    [f setTextColor:(kind == ST_ERROR) ? rgb(0.75f, 0.10f, 0.08f, 1.0f)
                                       : [NSColor controlTextColor]];
}

static void err_text(char *s, size_t sz, int oserr, const char *err)
{
    if (dg_is_perm_err(oserr))
        snprintf(s, sz, "Ошибка: нет прав на raw-диск - перезапустите "
                        "приложение и введите пароль администратора");
    else
        snprintf(s, sz, "Ошибка: %s", err);
}

static const char *k_noroot =
    "Нет прав на raw-диск - перезапустите приложение и введите пароль "
    "администратора";

/* ------------------------------------------------------------ SMART */

/*
 * SMART всех целых дисков читает фоновый поток (ядро smartcore, ретраи
 * спящего диска до ~3 с - UI не замирает). Результат - массив под
 * g_lock + номер поколения; тик видит новое поколение и копирует его
 * в g_sm_ui (UI-поток), откуда читают вкладка SMART и статусы.
 */
#define SM_MAX 16

static sm_disk g_sm_res[SM_MAX];        /* под g_lock */
static int     g_sm_res_n = 0;
static int     g_sm_gen = 0;            /* +1 на каждый завершённый опрос */
static int     g_sm_busy = 0;           /* UI-поток: опрос идёт */

static sm_disk g_sm_ui[SM_MAX];         /* UI-поток */
static int     g_sm_ui_n = 0;
static int     g_sm_ui_gen = 0;
static char    g_sm_sel[16] = "disk0";

static NSPopUpButton *g_sm_dev = NULL;
static NSButton      *g_sm_refresh = NULL;
static NSTextField   *g_sm_verdict = NULL;
static NSTextField   *g_sm_note = NULL;
static NSTextField   *g_sm_ident = NULL;
static NSTextField   *g_sm_explain = NULL;
static NSTableView   *g_sm_table = NULL;
static NSTabViewItem *g_sm_tab = NULL;

static const sm_disk *sm_find(const char *name)
{
    int i;
    for (i = 0; i < g_sm_ui_n; i++)
        if (strcmp(g_sm_ui[i].name, name) == 0)
            return &g_sm_ui[i];
    return NULL;
}

/* "/dev/rdisk1" и "/dev/disk1" -> "disk1" */
static void dev_to_disk(const char *path, char *out, size_t outsz)
{
    const char *p = strrchr(path, '/');
    p = (p != NULL) ? p + 1 : path;
    if (p[0] == 'r')
        p++;
    snprintf(out, outsz, "%s", p);
}

/* хвост статуса замера: "   ·   SMART: WARN (...)" для плохого диска */
static void sm_tail(const char *path, char *out, size_t outsz)
{
    char           nm[16];
    const sm_disk *d;

    out[0] = 0;
    dev_to_disk(path, nm, sizeof nm);
    d = sm_find(nm);
    if (d != NULL && (d->vd == VD_WARN || d->vd == VD_FAIL))
        snprintf(out, outsz, "   ·   SMART: %s (%s)",
                 sm_verdict_str(d->vd), d->note);
}

static void status_lin(void)
{
    char s[320];
    int  n, idx, failed, canc, oserr, elapsed;
    uint64_t total, bytes, us;
    dglinresult res;
    char err[256];

    pthread_mutex_lock(&g_lock);
    n      = g_lin.n;
    idx    = g_lin_idx_done;
    total  = g_lin_total;
    bytes  = g_lin_bytes;
    us     = g_lin_time_us;
    failed = g_lin_failed;
    oserr  = g_lin_oserr;
    canc   = g_lin_cancel;
    res    = g_lin_res;
    elapsed = g_lin_elapsed;
    strncpy(err, g_lin_err, sizeof err);
    err[sizeof err - 1] = 0;
    pthread_mutex_unlock(&g_lock);

    if (g_run == RUN_LIN) {
        int t = (int)(time(NULL) - g_run_t0);
        if (canc)
            snprintf(s, sizeof s, "Останавливаюсь после текущего блока...");
        else if (total > 0)
            snprintf(s, sizeof s,
                     "Сегмент %d / %llu   ·   %.1f MB/s   ·   "
                     "duty %d%%   ·   %d:%02d",
                     idx, (unsigned long long)total, mbs_of(bytes, us),
                     g_lin_duty, t / 60, t % 60);
        else
            snprintf(s, sizeof s, "Открываю %s ...", g_lin_path);
        set_status(g_lin_status, s, ST_NORMAL);
    } else if (failed) {
        err_text(s, sizeof s, oserr, err);
        set_status(g_lin_status, s, ST_ERROR);
    } else if (n > 0) {
        snprintf(s, sizeof s,
                 "Готово: %llu сегментов   ·   avg %.1f MB/s   ·   "
                 "eio %llu   ·   %d:%02d%s",
                 (unsigned long long)res.chunks,
                 mbs_of(res.bytes, res.time_us),
                 (unsigned long long)res.eio,
                 elapsed / 60, elapsed % 60,
                 res.stopped ? "   [остановлено]" : "");
        set_status(g_lin_status, s, ST_DONE);
    } else if (!g_is_root) {
        set_status(g_lin_status, k_noroot, ST_ERROR);
    } else {
        if (g_lin_selcap > 0.0)
            snprintf(s, sizeof s,
                     "Будет прочитано ≈ %.1f GB из %.1f GB "
                     "(%d%% поверхности, сегменты по 32 MB)",
                     g_lin_selcap / GIB * (double)g_duty / 100.0,
                     g_lin_selcap / GIB, g_duty);
        else
            snprintf(s, sizeof s, "Выберите диск и нажмите Старт");
        {
            char tail[160];
            sm_tail(g_lin_sel, tail, sizeof tail);
            strncat(s, tail, sizeof s - strlen(s) - 1);
        }
        set_status(g_lin_status, s, ST_NORMAL);
    }
}

/* seek: "короткие ... · длинные ..." для статуса */
static void seek_summary(const rndtab *t, char *out, size_t outsz)
{
    snprintf(out, outsz,
             "хопы < 1 MB: med %.1f ms   ·   > ½ диска: med %.1f ms",
             t->short_med / 1000.0, t->long_med / 1000.0);
}

static void status_rnd(rndtab *t)
{
    char s[400], extra[160];
    int  n, failed, canc, oserr, elapsed;
    dgrndresult res;
    char err[256];
    int  seek = (t->mode == DGRND_SEEK);

    pthread_mutex_lock(&g_lock);
    n       = t->pts.n;
    failed  = t->failed;
    oserr   = t->oserr;
    canc    = t->cancel;
    res     = t->res;
    elapsed = t->elapsed;
    strncpy(err, t->err, sizeof err);
    err[sizeof err - 1] = 0;
    pthread_mutex_unlock(&g_lock);

    extra[0] = 0;
    if (seek && t->stat_n > 0)
        seek_summary(t, extra, sizeof extra);

    if (g_run == t->run_id) {
        int tt = (int)(time(NULL) - g_run_t0);
        if (tt < 0) tt = 0;
        if (tt > t->secs_run) tt = t->secs_run;
        if (canc)
            snprintf(s, sizeof s, "Останавливаюсь...");
        else if (seek)
            snprintf(s, sizeof s, "%s %d   ·   %d / %d с   ·   %s",
                     "хопов", n, tt, t->secs_run, extra);
        else
            snprintf(s, sizeof s,
                     "ops %d   ·   %d / %d с   ·   med %.1f ms   ·   "
                     "p99 %.1f ms",
                     n, tt, t->secs_run, t->med / 1000.0, t->p99 / 1000.0);
        set_status(t->status, s, ST_NORMAL);
    } else if (failed) {
        err_text(s, sizeof s, oserr, err);
        set_status(t->status, s, ST_ERROR);
    } else if (n > 0) {
        if (seek)
            snprintf(s, sizeof s,
                     "Готово: %llu хопов   ·   %s   ·   eio %llu   ·   "
                     "%d:%02d%s",
                     (unsigned long long)res.ops, extra,
                     (unsigned long long)res.eio,
                     elapsed / 60, elapsed % 60,
                     res.stopped ? "   [остановлено]" : "");
        else
            snprintf(s, sizeof s,
                     "Готово: %llu ops   ·   med %.1f ms   ·   p99 %.1f ms   ·"
                     "   eio %llu   ·   %d:%02d%s",
                     (unsigned long long)res.ops,
                     res.med_us / 1000.0, res.p99_us / 1000.0,
                     (unsigned long long)res.eio,
                     elapsed / 60, elapsed % 60,
                     res.stopped ? "   [остановлено]" : "");
        set_status(t->status, s, ST_DONE);
    } else if (!g_is_root) {
        set_status(t->status, k_noroot, ST_ERROR);
    } else {
        if (seek)
            snprintf(s, sizeof s,
                     "%d с хопов от 8 KB до всей ёмкости - профиль "
                     "\"время перемещения головки vs дистанция\"", t->secs);
        else
            snprintf(s, sizeof s,
                     "%d с чтений по 4 KB в случайные точки - карта времени "
                     "доступа по поверхности", t->secs);
        {
            char tail[160];
            sm_tail(t->sel, tail, sizeof tail);
            strncat(s, tail, sizeof s - strlen(s) - 1);
        }
        set_status(t->status, s, ST_NORMAL);
    }
}

static void status_all(void)
{
    status_lin();
    status_rnd(&g_acc);
    status_rnd(&g_seek);
}

/* ------------------------------------------------------- рабочие потоки */

static void lin_begin(void *ctx, const dglininfo *info)
{
    (void)ctx;
    pthread_mutex_lock(&g_lock);
    g_lin_total = info->chunks;
    g_lin_datacap = (double)info->capacity;
    pthread_mutex_unlock(&g_lock);
}

static void lin_err(void *ctx, const dglinerr *e)
{
    (void)ctx;
    pthread_mutex_lock(&g_lock);
    pts_add(&g_lin_errs, (double)e->off, 0.0, 0);
    pthread_mutex_unlock(&g_lock);
}

static void lin_seg(void *ctx, const dglinseg *seg)
{
    (void)ctx;
    pthread_mutex_lock(&g_lock);
    /* точка - в середине сегмента: кривая от начала до конца диска */
    pts_add(&g_lin, (double)seg->off + (double)seg->len * 0.5,
            mbs_of(seg->bytes, seg->time_us), 0);
    g_lin_bytes   += seg->bytes;
    g_lin_time_us += seg->time_us;
    g_lin_idx_done = (int)(seg->index + 1);
    pthread_mutex_unlock(&g_lock);
}

static void *lin_thread(void *arg)
{
    dglincfg    cfg;
    dglinresult res;
    char        err[256];
    int         rc;
    time_t      t0 = time(NULL);

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
    g_lin_failed = (rc != 0);
    g_lin_oserr  = res.oserr;
    if (rc != 0)
        snprintf(g_lin_err, sizeof g_lin_err, "%s", err);
    g_lin_res = res;
    g_lin_elapsed = (int)(time(NULL) - t0);
    g_lin_done = 1;
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

static void rnd_begin(void *ctx, const dgrndinfo *info)
{
    rndtab *t = (rndtab *)ctx;
    pthread_mutex_lock(&g_lock);
    t->datacap = (double)info->capacity;
    pthread_mutex_unlock(&g_lock);
}

/* access: X = позиция на диске; seek: X = дистанция хопа */
static void rnd_op(void *ctx, const dgrndop *op)
{
    rndtab *t = (rndtab *)ctx;
    double  x = (t->mode == DGRND_SEEK) ? (double)op->dist : (double)op->off;

    pthread_mutex_lock(&g_lock);
    pts_add(&t->pts, x, (double)op->us, op->dir);
    pthread_mutex_unlock(&g_lock);
}

static void rnd_err(void *ctx, const dgrnderr *e)
{
    rndtab *t = (rndtab *)ctx;
    pthread_mutex_lock(&g_lock);
    pts_add(&t->errs, (double)e->off, (double)e->us, 0);
    pthread_mutex_unlock(&g_lock);
}

static void *rnd_thread(void *arg)
{
    rndtab     *t = (rndtab *)arg;
    dgrndcfg    cfg;
    dgrndresult res;
    char        err[256];
    int         rc;
    time_t      t0 = time(NULL);

    memset(&cfg, 0, sizeof cfg);
    cfg.dev    = t->path;
    cfg.blk    = RND_BLK;
    cfg.secs   = t->secs_run;
    cfg.warmup = RND_WARMUP;
    cfg.seed   = 0;                 /* из времени/pid */
    cfg.mode   = t->mode;

    rc = dgrndcore(&cfg, rnd_begin, rnd_op, rnd_err, t,
                   &t->cancel, &res, err, sizeof err);

    pthread_mutex_lock(&g_lock);
    t->failed = (rc != 0);
    t->oserr  = res.oserr;
    if (rc != 0)
        snprintf(t->err, sizeof t->err, "%s", err);
    t->res = res;
    t->elapsed = (int)(time(NULL) - t0);
    t->done = 1;
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

/* UI-поток: доступность контролов по состоянию прогона */
static void update_rnd_controls(rndtab *t)
{
    int idle = (g_run == RUN_IDLE);

    [t->start setEnabled:idle];
    [t->stop setEnabled:(g_run == t->run_id && !t->cancel)];
    [t->slider setEnabled:idle];
    [t->dev setEnabled:idle];
}

static void update_controls(void)
{
    int idle = (g_run == RUN_IDLE);

    [g_lin_start setEnabled:idle];
    [g_lin_stop setEnabled:(g_run == RUN_LIN && !g_lin_cancel)];
    [g_lin_slider setEnabled:idle];
    [g_lin_dev setEnabled:idle];
    update_rnd_controls(&g_acc);
    update_rnd_controls(&g_seek);
}

static int start_thread(void *(*fn)(void *), void *arg)
{
    pthread_t th;

    if (pthread_create(&th, NULL, fn, arg) != 0)
        return 0;
    pthread_detach(th);
    return 1;
}

static void lin_start(void)
{
    if (g_run != RUN_IDLE)
        return;

    pthread_mutex_lock(&g_lock);
    g_lin.n = 0;
    g_lin_errs.n = 0;
    g_lin_total = 0;
    g_lin_bytes = 0;
    g_lin_time_us = 0;
    g_lin_idx_done = 0;
    g_lin_failed = 0;
    g_lin_oserr = 0;
    g_lin_err[0] = 0;
    g_lin_done = 0;
    g_lin_datacap = g_lin_selcap;
    memset(&g_lin_res, 0, sizeof g_lin_res);
    pthread_mutex_unlock(&g_lock);

    g_lin_cancel = 0;
    g_run = RUN_LIN;
    g_run_t0 = time(NULL);

    strncpy(g_lin_path, g_lin_sel, sizeof g_lin_path - 1);
    g_lin_path[sizeof g_lin_path - 1] = 0;
    g_lin_duty = g_duty;

    if (!start_thread(lin_thread, NULL)) {
        pthread_mutex_lock(&g_lock);
        snprintf(g_lin_err, sizeof g_lin_err, "не удалось создать поток");
        g_lin_failed = 1;
        g_lin_done = 1;
        pthread_mutex_unlock(&g_lock);
    }
    update_controls();
    status_lin();
}

static void rnd_start(rndtab *t)
{
    if (g_run != RUN_IDLE)
        return;

    pthread_mutex_lock(&g_lock);
    t->pts.n = 0;
    t->errs.n = 0;
    t->failed = 0;
    t->oserr = 0;
    t->err[0] = 0;
    t->done = 0;
    t->datacap = t->selcap;
    memset(&t->res, 0, sizeof t->res);
    pthread_mutex_unlock(&g_lock);

    t->med = t->p99 = t->short_med = t->long_med = 0.0;
    t->stat_n = -1;
    memset(t->bn, 0, sizeof t->bn);
    t->cancel = 0;
    g_run = t->run_id;
    g_run_t0 = time(NULL);

    strncpy(t->path, t->sel, sizeof t->path - 1);
    t->path[sizeof t->path - 1] = 0;
    t->secs_run = t->secs;

    if (!start_thread(rnd_thread, t)) {
        pthread_mutex_lock(&g_lock);
        snprintf(t->err, sizeof t->err, "не удалось создать поток");
        t->failed = 1;
        t->done = 1;
        pthread_mutex_unlock(&g_lock);
    }
    update_controls();
    status_rnd(t);
}

/* UI-поток: выбор в popup поменялся - ёмкость для оси и подсказки */
static void lin_sel_changed(void)
{
    g_lin_selcap = dg_probe_capacity(g_lin_sel);
    status_lin();
    [g_lin_view setNeedsDisplay:YES];
}

static void rnd_sel_changed(rndtab *t)
{
    t->selcap = dg_probe_capacity(t->sel);
    t->stat_n = -1;                 /* корзины зависят от ёмкости */
    status_rnd(t);
    [t->view setNeedsDisplay:YES];
}

static void refresh_devs(void)
{
    if (g_run != RUN_IDLE)
        return;                     /* popup-ы заблокированы - не трогаем */
    if (fill_dev_popup(g_lin_dev, g_lin_sel, sizeof g_lin_sel))
        lin_sel_changed();
    if (fill_dev_popup(g_acc.dev, g_acc.sel, sizeof g_acc.sel))
        rnd_sel_changed(&g_acc);
    if (fill_dev_popup(g_seek.dev, g_seek.sel, sizeof g_seek.sel))
        rnd_sel_changed(&g_seek);
}

/* по контролу - его random-вкладка (NULL - Linear) */
static rndtab *rndtab_of(id ctl)
{
    if (ctl == g_acc.start || ctl == g_acc.stop || ctl == g_acc.slider ||
        ctl == g_acc.dev)
        return &g_acc;
    if (ctl == g_seek.start || ctl == g_seek.stop || ctl == g_seek.slider ||
        ctl == g_seek.dev)
        return &g_seek;
    return NULL;
}

/* ------------------------------------------------------- SMART: опрос */

static void *sm_thread(void *arg)
{
    static sm_disk tmp[SM_MAX];        /* один поток за раз (g_sm_busy) */
    char names[SM_MAX][16];
    int  n, i;

    (void)arg;
    n = sm_scan(names, SM_MAX);
    for (i = 0; i < n; i++)
        sm_probe(names[i], &tmp[i]);

    pthread_mutex_lock(&g_lock);
    memcpy(g_sm_res, tmp, (size_t)n * sizeof(sm_disk));
    g_sm_res_n = n;
    g_sm_gen++;
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

static void sm_refresh(void)
{
    if (g_sm_busy)
        return;
    g_sm_busy = 1;
    if (!start_thread(sm_thread, NULL)) {
        g_sm_busy = 0;
        return;
    }
    [g_sm_refresh setEnabled:NO];
    [g_sm_verdict setStringValue:[NSString stringWithUTF8String:"..."]];
    [g_sm_note setStringValue:[NSString stringWithUTF8String:
                                  "читаю SMART..."]];
}

/* короткое пояснение атрибута для колонки "Что это" */
static const char *sm_explain(int id)
{
    switch (id) {
    case 1:   return "ошибки чтения; у Seagate RAW велик - норма";
    case 3:   return "время раскрутки шпинделя";
    case 4:   return "число пусков/остановок шпинделя";
    case 5:   return "переназначенные сектора: > 0 - поверхность сыпется";
    case 7:   return "ошибки позиционирования; у Seagate RAW велик - норма";
    case 9:   return "наработка, часы";
    case 10:  return "повторы раскрутки - механика или питание";
    case 12:  return "число включений";
    case 184: return "ошибки тракта данных внутри диска";
    case 187: return "неисправимые ошибки, о которых диск сообщил";
    case 188: return "таймауты команд";
    case 189: return "запись при высоком полёте головки";
    case 190:
    case 194: return "температура, °C";
    case 191: return "удары (датчик ускорения)";
    case 192: return "аварийные парковки при пропадании питания";
    case 193: return "парковки головок (ноутбуки: ресурс ~300-600 тыс.)";
    case 195: return "аппаратная ECC-коррекция (Seagate)";
    case 196: return "события переназначения";
    case 197: return "нестабильные сектора: данные под угрозой";
    case 198: return "нечитаемые сектора по фоновому скану";
    case 199: return "CRC-ошибки шлейфа/контроллера, не поверхность";
    case 200: return "ошибки записи";
    default:  return "";
    }
}

/* человеческое пояснение к вердикту: по каждой находке строка */
static void sm_findings(const sm_disk *d, char *out, size_t outsz)
{
    int i;

    out[0] = 0;
    if (!d->have_data) {
        if (d->need_root)
            snprintf(out, outsz, "SMART читается только с правами "
                                 "администратора.");
        else
            snprintf(out, outsz, "Данных SMART нет: %s. USB/FireWire-"
                     "мосты и RAID-тома SMART не пропускают.", d->note);
        return;
    }
    if (d->exceeded)
        snprintf(out + strlen(out), outsz - strlen(out),
                 "Диск сам сообщает о превышении порога (SMART status: "
                 "FAIL) - готовьтесь к замене.  ");
    for (i = 0; i < d->nattr; i++) {
        const sm_attr *a = &d->attrs[i];
        unsigned long  v = (unsigned long)(a->raw & 0xffffffffULL);
        char           b[512];

        if (a->level == SM_LV_OK)
            continue;
        b[0] = 0;
        if (a->id == 197)
            snprintf(b, sizeof b, "Pending = %lu: диск не смог прочитать "
                     "%lu сектор(ов), данные в них под угрозой - сделайте "
                     "копию.  ", v, v);
        else if (a->id == 5)
            snprintf(b, sizeof b, "Reallocated = %lu: диск уже подменил "
                     "%lu сектор(ов) резервными - поверхность деградирует, "
                     "следите за ростом.  ", v, v);
        else if (a->id == 198)
            snprintf(b, sizeof b, "Offline Uncorrectable = %lu: фоновый "
                     "скан нашёл нечитаемые сектора.  ", v);
        else if (a->id == 199)
            snprintf(b, sizeof b, "UDMA CRC = %lu: ошибки передачи по "
                     "шлейфу/контроллеру, а не поверхности. Проверьте "
                     "80-жильный кабель и разъёмы.  ", v);
        else if (a->level == SM_LV_FAIL)
            snprintf(b, sizeof b, "Атрибут %d (%s) дошёл до порога "
                     "(%d <= %d) - диск прогнозирует отказ.  ",
                     a->id, sm_attr_name(a->id) ? sm_attr_name(a->id) : "?",
                     a->value, a->thr);
        strncat(out, b, outsz - strlen(out) - 1);
    }
    if (out[0] == 0)
        snprintf(out, outsz, "Критичные счётчики (5, 197, 198, 199) "
                 "нулевые, пороги не достигнуты.");
}

/* заголовок пункта popup: "disk0 - ST360021A, 55.9 GB" */
static NSString *sm_title(const sm_disk *d)
{
    char t[96];
    if (d->model[0])
        snprintf(t, sizeof t, "%s - %s, %.1f GB", d->name, d->model,
                 (double)d->bytes / GIB);
    else
        snprintf(t, sizeof t, "%s", d->name);
    return [NSString stringWithUTF8String:t];
}

/* UI-поток: показать выбранный диск (плашка, паспорт, таблица) */
static void sm_show(void)
{
    const sm_disk *d = sm_find(g_sm_sel);
    char  s[1024];
    NSColor *c;

    if (g_sm_verdict == nil)
        return;
    [g_sm_table reloadData];
    if (d == NULL) {
        [g_sm_verdict setStringValue:[NSString stringWithUTF8String:"-"]];
        [g_sm_note setStringValue:@""];
        [g_sm_ident setStringValue:@""];
        [g_sm_explain setStringValue:@""];
        return;
    }

    switch (d->vd) {
    case VD_OK:   c = col_avg(); break;
    case VD_WARN: c = col_p99(); break;
    case VD_FAIL: c = col_spike(); break;
    default:      c = [NSColor grayColor]; break;
    }
    [g_sm_verdict setTextColor:c];
    [g_sm_verdict setStringValue:
        [NSString stringWithUTF8String:sm_verdict_str(d->vd)]];
    [g_sm_note setStringValue:[NSString stringWithUTF8String:
        d->note[0] ? d->note : (d->vd == VD_OK ? "всё в порядке" : "")]];

    {
        char h[64] = "", t[24] = "";
        if (d->has_hours)
            snprintf(h, sizeof h, "   ·   %llu ч (%.1f лет)",
                     (unsigned long long)d->n_hours,
                     (double)d->n_hours / 8766.0);
        if (d->temp >= 0)
            snprintf(t, sizeof t, "   ·   %d °C", d->temp);
        snprintf(s, sizeof s, "%s   ·   S/N %s   ·   FW %s   ·   %s   ·   "
                 "%.1f GB%s%s",
                 d->model[0] ? d->model : "-",
                 d->serial[0] ? d->serial : "-",
                 d->fw[0] ? d->fw : "-",
                 d->bus[0] ? d->bus : "-",
                 (double)d->bytes / GIB, h, t);
        [g_sm_ident setStringValue:u8(s)];
    }
    sm_findings(d, s, sizeof s);
    [g_sm_explain setStringValue:u8(s)];
}

/* UI-поток: тик - забрать новое поколение опроса */
static void sm_poll(void)
{
    int gen, i, bad = 0;

    pthread_mutex_lock(&g_lock);
    gen = g_sm_gen;
    if (gen != g_sm_ui_gen) {
        memcpy(g_sm_ui, g_sm_res, (size_t)g_sm_res_n * sizeof(sm_disk));
        g_sm_ui_n = g_sm_res_n;
    }
    pthread_mutex_unlock(&g_lock);
    if (gen == g_sm_ui_gen)
        return;
    g_sm_ui_gen = gen;
    g_sm_busy = 0;
    [g_sm_refresh setEnabled:YES];

    /* popup: имена + модели; выбор сохраняется */
    [g_sm_dev removeAllItems];
    for (i = 0; i < g_sm_ui_n; i++) {
        [g_sm_dev addItemWithTitle:sm_title(&g_sm_ui[i])];
        if (strcmp(g_sm_ui[i].name, g_sm_sel) == 0)
            [g_sm_dev selectItemAtIndex:i];
        if (g_sm_ui[i].vd == VD_WARN || g_sm_ui[i].vd == VD_FAIL)
            bad = 1;
    }
    if (g_sm_ui_n > 0 && sm_find(g_sm_sel) == NULL) {
        snprintf(g_sm_sel, sizeof g_sm_sel, "%s", g_sm_ui[0].name);
        [g_sm_dev selectItemAtIndex:0];
    }

    /* "лампочка" на ярлыке вкладки */
    [g_sm_tab setLabel:[NSString stringWithUTF8String:
                           bad ? "SMART (!)" : "SMART"]];
    [g_tabs setNeedsDisplay:YES];
    sm_show();
}

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
- (void)smRefresh:(id)s;
- (void)smDevChanged:(id)s;
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
    if (g_run == RUN_LIN) {
        g_lin_cancel = 1;
        update_controls();
        status_lin();
    }
}

- (void)rndStart:(id)s
{
    rndtab *t = rndtab_of(s);
    if (t != NULL)
        rnd_start(t);
}

- (void)rndStop:(id)s
{
    rndtab *t = rndtab_of(s);
    if (t == NULL)
        t = rndtab_of_run(g_run);   /* dev-хук -stop шлёт nil */
    if (t != NULL && g_run == t->run_id) {
        t->cancel = 1;
        update_controls();
        status_rnd(t);
    }
}

- (void)dutyChanged:(id)s
{
    char lbl[64];
    g_duty = [s intValue];
    snprintf(lbl, sizeof lbl, "%d %%", g_duty);
    [g_duty_val setStringValue:[NSString stringWithUTF8String:lbl]];
    if (g_run == RUN_IDLE)
        status_lin();
}

- (void)secsChanged:(id)s
{
    rndtab *t = rndtab_of(s);
    char    lbl[64];

    if (t == NULL)
        return;
    t->secs = [s intValue];
    snprintf(lbl, sizeof lbl, "%d с", t->secs);
    [t->val setStringValue:[NSString stringWithUTF8String:lbl]];
    if (g_run == RUN_IDLE)
        status_rnd(t);
}

- (void)devChanged:(id)s
{
    NSString *title = [s titleOfSelectedItem];
    rndtab   *t = rndtab_of(s);
    char      path[64];

    if (title == nil)
        return;
    title_to_path(title, path, sizeof path);
    if (path[0] != '/')
        return;
    if (t == NULL) {
        strcpy(g_lin_sel, path);
        lin_sel_changed();
    } else {
        strcpy(t->sel, path);
        rnd_sel_changed(t);
    }
}

- (void)appActive:(NSNotification *)n
{
    (void)n;
    refresh_devs();
}

- (void)smRefresh:(id)s
{
    (void)s;
    sm_refresh();
}

- (void)smDevChanged:(id)s
{
    int i = (int)[s indexOfSelectedItem];
    if (i >= 0 && i < g_sm_ui_n) {
        snprintf(g_sm_sel, sizeof g_sm_sel, "%s", g_sm_ui[i].name);
        sm_show();
    }
}

/* таблица SMART: data source / delegate (API 10.0) */
- (int)numberOfRowsInTableView:(NSTableView *)tv
{
    const sm_disk *d = sm_find(g_sm_sel);
    (void)tv;
    return (d != NULL && d->have_data) ? d->nattr : 0;
}

- (id)tableView:(NSTableView *)tv objectValueForTableColumn:(NSTableColumn *)col
            row:(int)row
{
    const sm_disk *d = sm_find(g_sm_sel);
    const sm_attr *a;
    NSString      *cid = [col identifier];
    char           b[256];

    (void)tv;
    if (d == NULL || row < 0 || row >= d->nattr)
        return @"";
    a = &d->attrs[row];
    b[0] = 0;
    if ([cid isEqualToString:@"id"])
        snprintf(b, sizeof b, "%d", a->id);
    else if ([cid isEqualToString:@"name"])
        snprintf(b, sizeof b, "%s", sm_attr_name(a->id) ?
                 sm_attr_name(a->id) : "Vendor_Specific");
    else if ([cid isEqualToString:@"val"])
        snprintf(b, sizeof b, "%d", a->value);
    else if ([cid isEqualToString:@"wor"])
        snprintf(b, sizeof b, "%d", a->worst);
    else if ([cid isEqualToString:@"thr"])
        snprintf(b, sizeof b, a->has_thr ? "%d" : "?", a->thr);
    else if ([cid isEqualToString:@"raw"])
        snprintf(b, sizeof b, "%lu", (unsigned long)(a->raw & 0xffffffffULL));
    else if ([cid isEqualToString:@"type"])
        snprintf(b, sizeof b, "%s", (a->flags & 1) ? "pre-fail" : "old-age");
    else if ([cid isEqualToString:@"what"])
        snprintf(b, sizeof b, "%s", sm_explain(a->id));
    return u8(b);
}

- (void)tableView:(NSTableView *)tv willDisplayCell:(id)cell
   forTableColumn:(NSTableColumn *)col row:(int)row
{
    const sm_disk *d = sm_find(g_sm_sel);
    int            lv = SM_LV_OK;

    (void)tv;
    (void)col;
    if (d != NULL && row >= 0 && row < d->nattr)
        lv = d->attrs[row].level;
    if (![cell respondsToSelector:@selector(setDrawsBackground:)])
        return;
    if (lv == SM_LV_OK) {
        [cell setDrawsBackground:NO];
        [cell setTextColor:[NSColor controlTextColor]];
    } else {
        [cell setDrawsBackground:YES];
        [cell setBackgroundColor:(lv == SM_LV_FAIL)
            ? rgb(1.0f, 0.82f, 0.80f, 1.0f) : rgb(1.0f, 0.90f, 0.72f, 1.0f)];
        [cell setTextColor:(lv == SM_LV_FAIL)
            ? rgb(0.60f, 0.0f, 0.0f, 1.0f) : rgb(0.45f, 0.22f, 0.0f, 1.0f)];
    }
}

/* view вкладок сами не тянутся за NSTabView - выставляем contentRect */
- (void)windowDidResize:(NSNotification *)n
{
    NSRect cr;
    int    i;

    (void)n;
    if (g_tabs == NULL)
        return;
    cr = [g_tabs contentRect];
    for (i = 0; i < (int)[g_tabs numberOfTabViewItems]; i++) {
        NSView *v = [[g_tabs tabViewItemAtIndex:i] view];
        if (v != nil)
            [v setFrame:cr];
    }
    [g_sm_table sizeLastColumnToFit];
}

- (void)tick:(NSTimer *)timer
{
    int     done = 0;
    rndtab *rt;

    (void)timer;

    rt = rndtab_of_run(g_run);
    pthread_mutex_lock(&g_lock);
    if (g_run == RUN_LIN)
        done = g_lin_done;
    else if (rt != NULL)
        done = rt->done;
    pthread_mutex_unlock(&g_lock);

    rnd_stats_update(&g_acc);
    rnd_stats_update(&g_seek);
    sm_poll();

    if (g_run != RUN_IDLE && done) {
        int was = g_run;
        g_run = RUN_IDLE;
        update_controls();
        if (g_again) {
            g_again = 0;
            printf("again: restart %d\n", was);
            fflush(stdout);
            if (was == RUN_LIN)
                lin_start();
            else
                rnd_start(rt);
        }
    }

    if (g_stop_at > 0 && time(NULL) >= g_stop_at) {
        g_stop_at = 0;
        if (g_run == RUN_LIN)
            [self linStop:nil];
        else if (g_run != RUN_IDLE)
            [self rndStop:nil];
    }

    if (g_close_at > 0 && time(NULL) >= g_close_at) {
        g_close_at = 0;
        printf("close: performClose\n");
        fflush(stdout);
        [g_win performClose:nil];
        return;
    }

    status_all();
    [g_lin_view setNeedsDisplay:YES];
    [g_acc.view setNeedsDisplay:YES];
    [g_seek.view setNeedsDisplay:YES];

    if (g_cycle) {
        g_cycle_ticks++;
        if (g_cycle_ticks % (int)(3.0 / TICK_SEC) == 0) {
            int cur = (int)[g_tabs indexOfTabViewItem:
                                [g_tabs selectedTabViewItem]];
            int nxt = (cur + 1) % 3;
            [g_tabs selectTabViewItemAtIndex:nxt];
            pthread_mutex_lock(&g_lock);
            printf("cycle: tab %d -> %d   lin_n %d   acc_n %d   seek_n %d\n",
                   cur, nxt, g_lin.n, g_acc.pts.n, g_seek.pts.n);
            fflush(stdout);
            pthread_mutex_unlock(&g_lock);
        }
    }
}

- (void)quit:(id)s
{
    (void)s;
    [NSApp terminate:nil];
}

/* NSApp delegate: закрыли окно - выходим (root-процесс не висит) */
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)a
{
    (void)a;
    return YES;
}

/* дать ядру выйти из pread и закрыть fd; не дольше секунды */
- (void)applicationWillTerminate:(NSNotification *)n
{
    rndtab *rt = rndtab_of_run(g_run);
    int     i, done = 1;

    (void)n;
    g_lin_cancel = 1;
    g_acc.cancel = 1;
    g_seek.cancel = 1;
    for (i = 0; i < 20; i++) {
        pthread_mutex_lock(&g_lock);
        if (g_run == RUN_LIN)
            done = g_lin_done;
        else if (rt != NULL)
            done = rt->done;
        pthread_mutex_unlock(&g_lock);
        if (done)
            break;
        usleep(50000);
    }
}

@end

/* ------------------------------------------------------------ сборка UI */

static void tip(NSView *v, const char *s)
{
    /* NSView setToolTip: - с 10.0; ярлыки вкладок тултипов не умеют
     * до 10.6, поэтому подсказки висят на контролах и графике */
    [v setToolTip:[NSString stringWithUTF8String:s]];
}

static NSTextField *make_label(NSView *v, const char *txt,
                               float x, float y, float w, int right)
{
    NSTextField *lab = [[NSTextField alloc]
        initWithFrame:NSMakeRect(x, y, w, 17.0f)];
    [lab setStringValue:[NSString stringWithUTF8String:txt]];
    [lab setFont:[NSFont systemFontOfSize:12.0f]];
    [lab setBezeled:NO];
    [lab setEditable:NO];
    [lab setSelectable:NO];
    [lab setDrawsBackground:NO];
    if (right)
        [lab setAlignment:NSRightTextAlignment];
    [lab setAutoresizingMask:NSViewMinYMargin];
    [v addSubview:lab];
    return [lab autorelease];
}

static NSButton *make_button(NSView *v, Controller *ctl, const char *title,
                             SEL act, float x, float y)
{
    NSButton *b = [[NSButton alloc]
        initWithFrame:NSMakeRect(x, y, 96.0f, 32.0f)];
    [b setBezelStyle:NSRoundedBezelStyle];
    [b setTitle:[NSString stringWithUTF8String:title]];
    [b setTarget:ctl];
    [b setAction:act];
    [b setAutoresizingMask:NSViewMinXMargin | NSViewMinYMargin];
    [v addSubview:b];
    return [b autorelease];
}

static const char *k_tip_dev =
    "Устройство для замера.\n"
    "/dev/rdiskN - raw-нода: чтение идёт прямо с диска, запросами "
    "нашего размера. Для замера - только она.\n"
    "/dev/diskN (buffered) - блочная нода: через кеш ядра порциями по "
    "4 KB, F_NOCACHE на неё не действует. Только для сравнения.";

/* что даёт вкладка и как выглядит норма - серая строка под контролами */
static const char *k_desc_seq =
    "Скорость чтения подряд по всей поверхности: как быстро диск читает "
    "большие файлы. Норма - плавный спад от начала к концу (внешние "
    "дорожки быстрее); провалы - медленные или переназначенные участки.";
static const char *k_desc_acc =
    "Время доступа в случайные точки поверхности: как диск справляется с "
    "мелкими разбросанными файлами. Норма - ровная полоса по всему диску "
    "(у краёв чуть выше - это геометрия); приподнятый участок или красные "
    "крестики - зона, где сбоят позиционер или поверхность.";
static const char *k_desc_seek =
    "Время прыжка головки в зависимости от его длины: механика "
    "позиционера. Норма - плавный рост от долей ms на коротких прыжках до "
    "полного хода; ступеньки и разрывы - проблемы механики.";
static const char *k_desc_smart =
    "Самодиагностика: счётчики ошибок и износа, которые диск ведёт сам. "
    "Только чтение - тесты не запускаются, состояние диска не меняется.";

#define DESC_H 30.0f                /* две строки по 11 pt */

static void make_desc(NSView *v, NSSize sz, const char *txt)
{
    NSTextField *t = [[NSTextField alloc] initWithFrame:
        NSMakeRect(14.0f, sz.height - 44.0f - DESC_H, sz.width - 28.0f,
                   DESC_H)];
    [t setStringValue:u8(txt)];
    [t setFont:[NSFont systemFontOfSize:11.0f]];
    [t setTextColor:[NSColor colorWithCalibratedWhite:0.40f alpha:1.0f]];
    [t setBezeled:NO];
    [t setEditable:NO];
    [t setSelectable:NO];
    [t setDrawsBackground:NO];
    [t setAutoresizingMask:NSViewWidthSizable | NSViewMinYMargin];
    [v addSubview:t];
    [t release];
}

/*
 * Вкладка: ряд контролов (диск, слайдер, Старт/Стоп), описание, статус,
 * график на остальное место. t == NULL - Sequential read, иначе random.
 */
static NSView *make_tab(Controller *ctl, NSSize sz, rndtab *t)
{
    NSView        *v = [[NSView alloc] initWithFrame:
                            NSMakeRect(0.0f, 0.0f, sz.width, sz.height)];
    float          h = sz.height, w = sz.width;
    float          y = h - 36.0f;
    int            lin = (t == NULL);
    NSPopUpButton *pop;
    NSSlider      *sl;
    NSTextField   *val, *st;
    NSView        *gv;
    NSButton      *bstart, *bstop;
    char           lbl[32];

    make_label(v, "Диск:", 10.0f, y + 4.0f, 40.0f, 1);
    pop = [[NSPopUpButton alloc]
        initWithFrame:NSMakeRect(54.0f, y - 1.0f, 186.0f, 26.0f) pullsDown:NO];
    [pop setTarget:ctl];
    [pop setAction:@selector(devChanged:)];
    [pop setAutoresizingMask:NSViewMinYMargin];
    tip(pop, k_tip_dev);
    [v addSubview:pop];
    [pop release];

    make_label(v, lin ? "Duty:" : "Время:", 248.0f, y + 4.0f, 52.0f, 1);
    sl = [[NSSlider alloc] initWithFrame:
        NSMakeRect(306.0f, y + 1.0f, 170.0f, 22.0f)];
    [sl setMinValue:lin ? 1.0 : 5.0];
    [sl setMaxValue:lin ? 100.0 : 120.0];
    [sl setIntValue:lin ? g_duty : t->secs];
    [sl setTarget:ctl];
    [sl setAction:lin ? @selector(dutyChanged:) : @selector(secsChanged:)];
    [sl setAutoresizingMask:NSViewMinYMargin];
    tip(sl, lin ? "Какую долю поверхности прочитать: сегменты по 32 MB "
                  "равномерно по всему диску. 5% - быстрый профиль зон, "
                  "100% - сплошной проход."
                : "Длительность прогона в секундах.");
    [v addSubview:sl];
    [sl release];

    if (lin)
        snprintf(lbl, sizeof lbl, "%d %%", g_duty);
    else
        snprintf(lbl, sizeof lbl, "%d с", t->secs);
    val = make_label(v, lbl, 482.0f, y + 4.0f, 60.0f, 0);
    [val setFont:[NSFont boldSystemFontOfSize:12.0f]];

    st = [[NSTextField alloc]
        initWithFrame:NSMakeRect(14.0f, h - 66.0f - DESC_H, w - 28.0f,
                                 17.0f)];
    [st setBezeled:NO];
    [st setEditable:NO];
    [st setSelectable:NO];
    [st setDrawsBackground:NO];
    [st setAutoresizingMask:NSViewWidthSizable | NSViewMinYMargin];
    [v addSubview:st];
    [st release];

    gv = [[(lin ? [LinView class] : [RndView class]) alloc] initWithFrame:
        NSMakeRect(0.0f, 0.0f, w, h - 72.0f - DESC_H)];
    [gv setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [v addSubview:gv];
    [gv release];

    bstart = make_button(v, ctl, "Старт",
                         lin ? @selector(linStart:) : @selector(rndStart:),
                         w - 206.0f, y - 4.0f);
    bstop  = make_button(v, ctl, "Стоп",
                         lin ? @selector(linStop:) : @selector(rndStop:),
                         w - 108.0f, y - 4.0f);
    tip(bstop, "Остановить прогон; уже снятые точки остаются на графике.");

    make_desc(v, sz, lin ? k_desc_seq
                         : (t->mode == DGRND_SEEK ? k_desc_seek : k_desc_acc));

    if (lin) {
        g_lin_dev = pop; g_lin_slider = sl; g_duty_val = val;
        g_lin_status = st; g_lin_view = gv;
        g_lin_start = bstart; g_lin_stop = bstop;
        tip(bstart, "Sequential read: сегменты по 32 MB блоками 1 MB "
                    "равномерно от начала до конца диска. Кривая MB/s "
                    "показывает зоны: снаружи быстрее, к центру медленнее.");
        tip(gv, "Скорость последовательного чтения (MB/s) по позиции на "
                "диске.\nПунктир - средняя скорость, красные крестики - "
                "ошибки чтения (EIO).");
        fill_dev_popup(pop, g_lin_sel, sizeof g_lin_sel);
    } else {
        t->dev = pop; t->slider = sl; t->val = val;
        t->status = st; t->view = gv;
        t->start = bstart; t->stop = bstop;
        [(RndView *)gv setTab:t];
        if (t->mode == DGRND_SEEK) {
            tip(bstart, "Seek profile: цепочка хопов - следующее чтение 4 KB "
                        "на +-d от предыдущего, d log-равномерно от 8 KB до "
                        "всей ёмкости. Меряет время перемещения головки в "
                        "зависимости от дистанции.");
            tip(gv, "Время хопа (ms) vs дистанция (лог. шкала).\n"
                    "Зелёная линия - медиана по дистанции (seek + "
                    "в среднем полоборота), оранжевый пунктир - p10, "
                    "близко к чистому seek.\n"
                    "Короткие хопы вперёд могут попасть в кеш диска "
                    "(доли ms).");
        } else {
            tip(bstart, "Access map: чтения 4 KB в равномерно случайные "
                        "точки всего диска. Время доступа = seek + ожидание "
                        "вращения + передача; медиана - классическая цифра "
                        "random access time.");
            tip(gv, "Время доступа (ms) по позиции на диске.\n"
                    "Линии - медиана и p10 по полосам позиции: горб на "
                    "них - зона, куда головка приходит хуже.\n"
                    "Красные точки - выше p99, крестики внизу - ошибки "
                    "чтения (EIO).");
        }
        fill_dev_popup(pop, t->sel, sizeof t->sel);
    }
    return [v autorelease];
}

static NSTextField *make_text(NSView *v, NSRect r, float size, int bold)
{
    NSTextField *t = [[NSTextField alloc] initWithFrame:r];
    [t setStringValue:@""];
    [t setFont:bold ? [NSFont boldSystemFontOfSize:size]
                    : [NSFont systemFontOfSize:size]];
    [t setBezeled:NO];
    [t setEditable:NO];
    [t setSelectable:NO];
    [t setDrawsBackground:NO];
    [t setAutoresizingMask:NSViewWidthSizable | NSViewMinYMargin];
    [v addSubview:t];
    return [t autorelease];
}

static void sm_column(NSTableView *tv, NSString *ident, const char *title,
                      float w, int right)
{
    NSTableColumn *c = [[NSTableColumn alloc] initWithIdentifier:ident];
    [[c headerCell] setStringValue:[NSString stringWithUTF8String:title]];
    [c setWidth:w];
    [c setMinWidth:24.0f];
    [c setEditable:NO];
    [[c dataCell] setFont:[NSFont systemFontOfSize:11.0f]];
    if (right)
        [[c dataCell] setAlignment:NSRightTextAlignment];
    [tv addTableColumn:c];
    [c release];
}

/*
 * Вкладка SMART: диск + "Обновить", плашка вердикта, паспорт,
 * пояснение находок, таблица атрибутов. Только чтение SMART.
 */
static NSView *make_smart_tab(Controller *ctl, NSSize sz)
{
    NSView       *v = [[NSView alloc] initWithFrame:
                           NSMakeRect(0.0f, 0.0f, sz.width, sz.height)];
    float         h = sz.height, w = sz.width;
    float         y = h - 36.0f;
    NSScrollView *sv;
    NSTableView  *tv;

    make_label(v, "Диск:", 10.0f, y + 4.0f, 40.0f, 1);
    g_sm_dev = [[NSPopUpButton alloc]
        initWithFrame:NSMakeRect(54.0f, y - 1.0f, 300.0f, 26.0f) pullsDown:NO];
    [g_sm_dev setTarget:ctl];
    [g_sm_dev setAction:@selector(smDevChanged:)];
    [g_sm_dev setAutoresizingMask:NSViewMinYMargin];
    [g_sm_dev addItemWithTitle:[NSString stringWithUTF8String:g_sm_sel]];
    tip(g_sm_dev, "Целые диски. SMART отдают внутренние ATA/SATA-диски; "
                  "USB/FireWire-мосты и RAID-тома - нет.");
    [v addSubview:g_sm_dev];
    [g_sm_dev release];

    make_desc(v, sz, k_desc_smart);

    g_sm_refresh = make_button(v, ctl, "Обновить", @selector(smRefresh:),
                               w - 108.0f, y - 4.0f);
    tip(g_sm_refresh, "Перечитать SMART всех дисков. Только чтение: "
                      "статус, атрибуты, пороги - тесты не запускаются.");

    g_sm_verdict = make_text(v, NSMakeRect(14.0f, h - DESC_H - 86.0f, 90.0f, 32.0f),
                             26.0f, 1);
    tip(g_sm_verdict, "OK - критичные счётчики нулевые; WARN - есть "
                      "переназначенные/нечитаемые сектора или CRC-ошибки "
                      "шлейфа; FAIL - pending-сектора или атрибут на пороге.");
    g_sm_note = make_text(v, NSMakeRect(110.0f, h - DESC_H - 74.0f, w - 124.0f, 18.0f),
                          13.0f, 1);
    g_sm_ident = make_text(v, NSMakeRect(110.0f, h - DESC_H - 92.0f, w - 124.0f, 17.0f),
                           11.0f, 0);
    g_sm_explain = make_text(v, NSMakeRect(14.0f, h - DESC_H - 132.0f, w - 28.0f,
                                           34.0f), 11.0f, 0);
    [g_sm_explain setTextColor:
        [NSColor colorWithCalibratedWhite:0.25f alpha:1.0f]];

    sv = [[NSScrollView alloc] initWithFrame:
        NSMakeRect(8.0f, 8.0f, w - 16.0f, h - 146.0f - DESC_H)];
    [sv setHasVerticalScroller:YES];
    if ([sv respondsToSelector:@selector(setAutohidesScrollers:)])
        [sv setAutohidesScrollers:YES];         /* 10.3+ */
    [sv setBorderType:NSBezelBorder];
    [sv setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];

    tv = [[NSTableView alloc] initWithFrame:[[sv contentView] bounds]];
    sm_column(tv, @"id",   "ID",       34.0f, 1);
    sm_column(tv, @"name", "Атрибут", 170.0f, 0);
    sm_column(tv, @"val",  "Value",    46.0f, 1);
    sm_column(tv, @"wor",  "Worst",    46.0f, 1);
    sm_column(tv, @"thr",  "Thr",      38.0f, 1);
    sm_column(tv, @"raw",  "Raw",      84.0f, 1);
    sm_column(tv, @"type", "Тип",      64.0f, 0);
    sm_column(tv, @"what", "Что это", 300.0f, 0);
    [tv setDataSource:ctl];
    [tv setDelegate:ctl];
    [tv setAllowsColumnReordering:NO];
    [tv setAutoresizingMask:NSViewWidthSizable];
    [tv setRowHeight:16.0f];
    tip(tv, "Value/Worst - нормированные вендором значения (больше - "
            "лучше), Thr - порог; Raw - сырой счётчик (младшие 32 бита). "
            "Подсвечены строки, из-за которых вердикт WARN/FAIL.");
    [sv setDocumentView:tv];
    [tv sizeLastColumnToFit];
    g_sm_table = tv;
    [tv release];
    [v addSubview:sv];
    [sv release];

    return [v autorelease];
}

/* ------------------------------------------------------- подъём прав */

/*
 * Перезапуск себя с правами root через Authorization Services:
 * пароль спрашивает системный SecurityAgent, в коде пароля нет.
 * Для автотестов можно задать DISKGRAPH_AUTH_USER/DISKGRAPH_AUTH_PASS,
 * тогда диалог не появляется.
 * DISKGRAPH_NOELEVATE=1 - не поднимать права (путь ошибки EACCES).
 *
 * Возврат 1 - root-экземпляр запущен, текущему пора выходить;
 *         0 - не вышло, продолжаем без прав (чтение упрётся в EACCES).
 */
static int relaunch_as_root(int argc, char **argv)
{
    AuthorizationRef      auth = NULL;
    AuthorizationItem     items[1];
    AuthorizationRights   rights;
    AuthorizationEnvironment env;
    AuthorizationItem     envitems[2];
    OSStatus              st;
    char                  path[1024];
    uint32_t              sz = sizeof path;
    char                **args;
    int                   i, n = 0, rc = 0;
    const char           *u = getenv("DISKGRAPH_AUTH_USER");
    const char           *p = getenv("DISKGRAPH_AUTH_PASS");

    if (_NSGetExecutablePath(path, &sz) != 0)
        return 0;

    /* -psn_X_Y Finder выдал этому процессу; root-копии он чужой */
    args = (char **)calloc((size_t)argc + 1, sizeof(char *));
    if (args == NULL)
        return 0;
    for (i = 1; i < argc; i++)
        if (strncmp(argv[i], "-psn_", 5) != 0)
            args[n++] = argv[i];
    args[n] = NULL;

    memset(items, 0, sizeof items);
    items[0].name        = kAuthorizationRightExecute;
    items[0].valueLength = strlen(path);
    items[0].value       = path;
    rights.count = 1;
    rights.items = items;

    if (u != NULL && p != NULL) {
        envitems[0].name        = kAuthorizationEnvironmentUsername;
        envitems[0].valueLength = strlen(u);
        envitems[0].value       = (void *)u;
        envitems[0].flags       = 0;
        envitems[1].name        = kAuthorizationEnvironmentPassword;
        envitems[1].valueLength = strlen(p);
        envitems[1].value       = (void *)p;
        envitems[1].flags       = 0;
        env.count = 2;
        env.items = envitems;
        st = AuthorizationCreate(&rights, &env,
                kAuthorizationFlagInteractionAllowed |
                kAuthorizationFlagExtendRights, &auth);
    } else {
        st = AuthorizationCreate(&rights, kAuthorizationEmptyEnvironment,
                kAuthorizationFlagInteractionAllowed |
                kAuthorizationFlagExtendRights, &auth);
    }
    if (st != errAuthorizationSuccess) {
        fprintf(stderr, "diskgraph: authorization failed (%ld)\n", (long)st);
        free(args);
        if (auth != NULL)
            AuthorizationFree(auth, kAuthorizationFlagDefaults);
        return 0;
    }

    st = AuthorizationExecuteWithPrivileges(auth, path,
            kAuthorizationFlagDefaults, args, NULL);
    if (st == errAuthorizationSuccess)
        rc = 1;
    else
        fprintf(stderr, "diskgraph: cannot relaunch as root (%ld)\n",
                (long)st);

    AuthorizationFree(auth, kAuthorizationFlagDefaults);
    free(args);
    return rc;
}

static int has_arg(int argc, char **argv, const char *a)
{
    int i;
    for (i = 1; i < argc; i++)
        if (strcmp(argv[i], a) == 0)
            return 1;
    return 0;
}

static void add_tab(Controller *ctl, NSSize sz, rndtab *t, const char *label)
{
    NSTabViewItem *item = [[NSTabViewItem alloc] initWithIdentifier:nil];
    [item setLabel:[NSString stringWithUTF8String:label]];
    [item setView:make_tab(ctl, sz, t)];
    [g_tabs addTabViewItem:item];
    [item release];
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
    NSTimer       *timer;
    NSSize         csz;
    int            sized = 0, i;

    /* -devs: только список дисков, без окна и без прав */
    if (has_arg(argc, argv, "-devs")) {
        NSArray *dl = scan_disks();
        for (i = 0; i < (int)[dl count]; i++)
            printf("%s\n", [[dl objectAtIndex:i] UTF8String]);
        [pool release];
        return 0;
    }

    /* чтение /dev/rdiskN требует root: поднимаем права при запуске */
    if (geteuid() != 0 && getenv("DISKGRAPH_NOELEVATE") == NULL &&
        relaunch_as_root(argc, argv)) {
        [pool release];
        return 0;
    }
    g_is_root = (geteuid() == 0) ? 1 : 0;

    rndtab_init(&g_acc,  DGRND_ACCESS, RUN_ACC,  "Access map",   30);
    rndtab_init(&g_seek, DGRND_SEEK,   RUN_SEEK, "Seek profile", 30);

    /* dev-хук: -w N -h N задают стартовый размер (проверка раскладки) */
    for (i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "-w") == 0) {
            rc.size.width = (float)atof(argv[i + 1]);
            sized = 1;
        } else if (strcmp(argv[i], "-h") == 0) {
            rc.size.height = (float)atof(argv[i + 1]);
            sized = 1;
        }
    }

    [NSApplication sharedApplication];
    ctl = [[Controller alloc] init];
    [NSApp setDelegate:ctl];

    menu = [[NSMenu alloc] initWithTitle:
                [NSString stringWithUTF8String:"main"]];
    {
        NSMenuItem *appmi = [menu addItemWithTitle:
            [NSString stringWithUTF8String:"DiskGraph"]
            action:NULL keyEquivalent:[NSString stringWithUTF8String:""]];
        submenu = [[NSMenu alloc] initWithTitle:
            [NSString stringWithUTF8String:"DiskGraph"]];
        [submenu addItemWithTitle:
            [NSString stringWithUTF8String:"Закрыть окно"]
            action:@selector(performClose:)
            keyEquivalent:[NSString stringWithUTF8String:"w"]];
        mi = [submenu addItemWithTitle:
            [NSString stringWithUTF8String:"Выход"]
            action:@selector(quit:)
            keyEquivalent:[NSString stringWithUTF8String:"q"]];
        [mi setTarget:ctl];
        [appmi setSubmenu:submenu];
        /* без nib Cocoa не знает, что это меню приложения, и рисует
         * второй "DiskGraph"; setAppleMenu: есть, но не в заголовках */
        if ([NSApp respondsToSelector:@selector(setAppleMenu:)])
            [NSApp performSelector:@selector(setAppleMenu:)
                        withObject:submenu];
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
    [win setTitle:[NSString stringWithUTF8String:"DiskGraph"]];
    g_win = win;
    [win setMinSize:NSMakeSize(840.0f, 520.0f)];
    root = [[[NSView alloc] initWithFrame:rc] autorelease];
    [root setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [win setContentView:root];

    /* вкладки с отступом от краёв окна - как в системных панелях */
    g_tabs = [[NSTabView alloc] initWithFrame:
                 NSInsetRect([root bounds], 8.0f, 6.0f)];
    [g_tabs setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [root addSubview:g_tabs];

    csz = [g_tabs contentRect].size;
    add_tab(ctl, csz, NULL,    "Sequential read");
    add_tab(ctl, csz, &g_acc,  "Access map");
    add_tab(ctl, csz, &g_seek, "Seek profile");
    g_sm_tab = [[NSTabViewItem alloc] initWithIdentifier:nil];
    [g_sm_tab setLabel:[NSString stringWithUTF8String:"SMART"]];
    [g_sm_tab setView:make_smart_tab(ctl, csz)];
    [g_tabs addTabViewItem:g_sm_tab];
    [ctl windowDidResize:nil];

    g_lin_selcap  = dg_probe_capacity(g_lin_sel);
    g_acc.selcap  = dg_probe_capacity(g_acc.sel);
    g_seek.selcap = dg_probe_capacity(g_seek.sel);
    update_controls();
    status_all();
    sm_refresh();                   /* SMART всех дисков - фоном */

    /* dev-хуки: -ldev/-adev/-sdev PATH - диск вкладки; -lin/-acc/-seek
     * стартуют реальный прогон; -cycle щёлкает вкладки и печатает
     * счётчики */
    for (i = 1; i + 1 < argc; i++) {
        rndtab *t = NULL;
        if (strcmp(argv[i], "-ldev") == 0) {
            strncpy(g_lin_sel, argv[i + 1], sizeof g_lin_sel - 1);
            fill_dev_popup(g_lin_dev, g_lin_sel, sizeof g_lin_sel);
            lin_sel_changed();
        } else if (strcmp(argv[i], "-adev") == 0) {
            t = &g_acc;
        } else if (strcmp(argv[i], "-duty") == 0) {
            char lbl[16];
            g_duty = atoi(argv[i + 1]);
            if (g_duty < 1) g_duty = 1;
            if (g_duty > 100) g_duty = 100;
            [g_lin_slider setIntValue:g_duty];
            snprintf(lbl, sizeof lbl, "%d %%", g_duty);
            [g_duty_val setStringValue:u8(lbl)];
        } else if (strcmp(argv[i], "-smdev") == 0) {
            snprintf(g_sm_sel, sizeof g_sm_sel, "%s", argv[i + 1]);
        } else if (strcmp(argv[i], "-sdev") == 0) {
            t = &g_seek;
        }
        if (t != NULL) {
            strncpy(t->sel, argv[i + 1], sizeof t->sel - 1);
            fill_dev_popup(t->dev, t->sel, sizeof t->sel);
            rnd_sel_changed(t);
        }
    }
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-lin") == 0) {
            [g_tabs selectTabViewItemAtIndex:0];
            lin_start();
        } else if (strcmp(argv[i], "-acc") == 0) {
            [g_tabs selectTabViewItemAtIndex:1];
            rnd_start(&g_acc);
        } else if (strcmp(argv[i], "-seek") == 0) {
            [g_tabs selectTabViewItemAtIndex:2];
            rnd_start(&g_seek);
        } else if (strcmp(argv[i], "-smart") == 0) {
            [g_tabs selectTabViewItemAtIndex:3];
        } else if (strcmp(argv[i], "-tab") == 0 && i + 1 < argc) {
            [g_tabs selectTabViewItemAtIndex:atoi(argv[i + 1])];
            i++;
        } else if (strcmp(argv[i], "-stop") == 0 && i + 1 < argc) {
            g_stop_at = time(NULL) + atoi(argv[i + 1]);
            i++;
        } else if (strcmp(argv[i], "-close") == 0 && i + 1 < argc) {
            g_close_at = time(NULL) + atoi(argv[i + 1]);
            i++;
        } else if (strcmp(argv[i], "-again") == 0) {
            g_again = 1;
        } else if (strcmp(argv[i], "-cycle") == 0) {
            g_cycle = 1;
            lin_start();
        }
    }

    /* позиция/размер окна запоминаются, если не заданы -w/-h */
    if (sized || ![win setFrameUsingName:@"DiskGraphMain"])
        [win center];
    if (!sized)
        [win setFrameAutosaveName:@"DiskGraphMain"];
    [win makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];

    timer = [NSTimer timerWithTimeInterval:TICK_SEC
                                    target:ctl
                                  selector:@selector(tick:)
                                  userInfo:nil
                                   repeats:YES];
    [[NSRunLoop currentRunLoop] addTimer:timer
                    forMode:NSDefaultRunLoopMode];
    /* таймер тикает и пока тянут ресайз окна/слайдер */
    [[NSRunLoop currentRunLoop] addTimer:timer
                    forMode:NSEventTrackingRunLoopMode];

    [NSApp run];

    [g_tabs release];
    [ctl release];
    [win release];
    [pool release];
    return 0;
}
