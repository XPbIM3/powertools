/*
 * diskgraph.m - UI DiskGraph: две вкладки (Linear / Random) на РЕАЛЬНЫХ
 *               замерах через dglincore/dgrndcore
 *
 * Фронт и бэк скрещены: вкладки зовут те же ядра, что и консольные
 * dblin/dbrnd, но данные приходят не в stdout, а в sink-калбеки:
 *   - linear: sink сегмента -> точка (offset, MB/s) на графике;
 *   - random: sink опа     -> точка (offset, лаг µs) на scatter.
 *
 * Поток: UI-поток (AppKit) + один рабочий поток на запуск (pthread).
 * Рабочий поток только дописывает точки в массивы под мьютексом и
 * никогда не трогает ObjC. UI раз в 0.25 c снимает копию данных под
 * мьютексом и рисует уже без него - замер не ждёт отрисовку.
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
 * enumeration; @"..."-литералы только ASCII.
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

/* ---- параметры приборов (как в консольных тулзах) ---- */
#define LIN_SEG (32ULL << 20)      /* 32 MiB чанк  */
#define LIN_BLK (1ULL  << 20)      /* 1 MiB блок   */
#define RND_BLK 4096ULL            /* 4 KiB        */
#define RND_WARMUP 16
#define TICK_SEC 0.25

#define WIN_W 1000.0f
#define WIN_H  720.0f

#define GIB 1073741824.0

/* ------------------------------------------------------------ данные */

typedef struct {
    double off;   /* позиция по ёмкости, байты */
    double val;   /* MB/s (linear) / µs (random) */
} dpt;

/* растущий массив точек: ёмкость удваивается, без realloc на точку */
typedef struct {
    dpt *a;
    int  n;
    int  alloc;
} dbuf;

static dbuf g_lin = { NULL, 0, 0 };
static dbuf g_rnd = { NULL, 0, 0 };

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---- состояние прогонов (под g_lock, кроме помеченного) ---- */
static int  g_run = 0;                  /* UI-поток: 0 idle, 1 lin, 2 rnd */
static volatile int g_lin_cancel = 0;
static volatile int g_rnd_cancel = 0;
static int  g_lin_done = 0, g_rnd_done = 0;
static int  g_lin_failed = 0, g_rnd_failed = 0;
static int  g_lin_oserr = 0, g_rnd_oserr = 0;
static char g_lin_err[256] = "";
static char g_rnd_err[256] = "";
static dglinresult g_lin_res;
static dgrndresult g_rnd_res;
static uint64_t g_lin_total = 0;        /* сегментов в плане */
static uint64_t g_lin_bytes = 0;        /* суммы для живого avg */
static uint64_t g_lin_time_us = 0;
static int      g_lin_idx_done = 0;     /* сегментов завершено */
static int      g_lin_elapsed = 0;      /* длительность прогона, с */
static int      g_rnd_elapsed = 0;
static time_t   g_run_t0 = 0;           /* UI-поток */

/* ёмкость устройства, которое реально читали (ось X при данных) */
static double g_lin_datacap = 0.0;
static double g_rnd_datacap = 0.0;

/* UI-поток: выбор в popup вкладки и его ёмкость (ось X без данных) */
static char   g_lin_sel[64] = "/dev/rdisk0";
static char   g_rnd_sel[64] = "/dev/rdisk0";
static double g_lin_selcap = 0.0;
static double g_rnd_selcap = 0.0;

/* снимок параметров на момент Старт (читает рабочий поток) */
static char g_lin_path[64] = "";
static int  g_lin_duty = 10;
static char g_rnd_path[64] = "";
static int  g_rnd_secs_run = 30;

/* UI-поток: значения слайдеров */
static int g_duty = 10;
static int g_rnd_secs = 30;

/* UI-поток: med/p99 random, пересчёт раз в тик */
static double g_rnd_med = 0.0, g_rnd_p99 = 0.0;
static int    g_rnd_stat_n = -1;

/* 1 - процесс с правами root (иначе чтение rdisk упрётся в EACCES) */
static int g_is_root = 0;

/* ---- UI ---- */
static NSTextField   *g_lin_status = NULL;
static NSTextField   *g_rnd_status = NULL;
static NSTextField   *g_duty_val  = NULL;
static NSTextField   *g_secs_val  = NULL;
static NSButton      *g_lin_start = NULL;
static NSButton      *g_rnd_start = NULL;
static NSButton      *g_lin_stop  = NULL;
static NSButton      *g_rnd_stop  = NULL;
static NSSlider      *g_lin_slider = NULL;
static NSSlider      *g_rnd_slider = NULL;
static NSView        *g_lin_view = NULL;
static NSView        *g_rnd_view = NULL;
static NSPopUpButton *g_lin_dev = NULL;
static NSPopUpButton *g_rnd_dev = NULL;
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

/* вызывать под g_lock */
static int pts_add(dbuf *b, double off, double val)
{
    if (b->n == b->alloc) {
        int  na = (b->alloc > 0) ? b->alloc * 2 : 1024;
        dpt *p  = (dpt *)realloc(b->a, (size_t)na * sizeof(dpt));
        if (p == NULL)
            return 0;
        b->a = p;
        b->alloc = na;
    }
    b->a[b->n].off = off;
    b->a[b->n].val = val;
    b->n++;
    return 1;
}

/* вызывать под g_lock; память не отдаём - следующий прогон её займёт */
static void pts_clear(dbuf *b)
{
    b->n = 0;
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

/* UI-поток: med/p99 random по снимку, сортировка вне лока */
static void rnd_stats_update(void)
{
    static double *v = NULL;
    static int     valloc = 0;
    int            n, i;

    pthread_mutex_lock(&g_lock);
    n = g_rnd.n;
    if (n == g_rnd_stat_n) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    if (n > valloc) {
        double *p = (double *)realloc(v, (size_t)g_rnd.alloc * sizeof(double));
        if (p == NULL) {
            pthread_mutex_unlock(&g_lock);
            return;
        }
        v = p;
        valloc = g_rnd.alloc;
    }
    for (i = 0; i < n; i++)
        v[i] = g_rnd.a[i].val;
    pthread_mutex_unlock(&g_lock);

    qsort(v, (size_t)n, sizeof(double), dg_dcmp);
    g_rnd_med = dg_pct(v, (size_t)n, 0.50);
    g_rnd_p99 = dg_pct(v, (size_t)n, 0.99);
    g_rnd_stat_n = n;
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
        [pop addItemWithTitle:t];
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

static NSColor *rgb(float r, float g, float b, float a)
{
    return [NSColor colorWithCalibratedRed:r green:g blue:b alpha:a];
}

static NSColor *col_line(void)    { return rgb(0.13f, 0.40f, 0.82f, 1.0f); }
static NSColor *col_fill(void)    { return rgb(0.13f, 0.40f, 0.82f, 0.13f); }
static NSColor *col_dot(void)     { return rgb(0.10f, 0.25f, 0.55f, 0.55f); }
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

/* ------------------------------------------------------- рисование */

typedef struct {
    NSRect pv;      /* область построения */
    double cap;     /* ось X, байты; <= 0 - неизвестна */
    double vmax;    /* ось Y; <= 0 - данных нет, шкалы нет */
    int    ndiv;
} plotgeo;

static NSPoint geo_pt(const plotgeo *g, double off, double val)
{
    double fx = (g->cap > 0.0) ? off / g->cap : 0.0;
    double fy = (g->vmax > 0.0) ? val / g->vmax : 0.0;
    if (fy > 1.0) fy = 1.0;
    return NSMakePoint(g->pv.origin.x + (float)fx * g->pv.size.width,
                       g->pv.origin.y + (float)fy * g->pv.size.height);
}

static NSRect plot_rect(NSRect bounds)
{
    float padl = 52.0f, padr = 16.0f, padt = 24.0f, padb = 40.0f;
    return NSMakeRect(padl, padb,
                      NSWidth(bounds) - padl - padr,
                      NSHeight(bounds) - padt - padb);
}

/* подложка, сетка, оси и подписи; ylab - имя оси Y, ydiv - делитель
 * для подписей (1 - как есть, 1000 - µs в ms) */
static void draw_frame(const plotgeo *g, const char *ylab, double ydiv)
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

    if (g->cap > 0.0) {
        double capgb = g->cap / GIB;
        double step  = x_step_gb(capgb, pv.size.width);
        double gb;
        int    k;

        for (k = 0; ; k++) {
            float x;
            char  lbl[24];
            NSSize ts;

            gb = step * (double)k;
            if (gb > capgb + 1e-9)
                break;
            x = x0 + (float)(gb / capgb) * pv.size.width;
            if (k > 0 && x < x1 - 0.5f) {
                [grid moveToPoint:NSMakePoint(x, y0)];
                [grid lineToPoint:NSMakePoint(x, y1)];
            }
            [ticks moveToPoint:NSMakePoint(x, y0)];
            [ticks lineToPoint:NSMakePoint(x, y0 - 4.0f)];
            if (step >= 1.0)
                snprintf(lbl, sizeof lbl, "%.0f", gb);
            else
                snprintf(lbl, sizeof lbl, "%.1f", gb);
            ts = text_size(lbl, tick_attrs());
            draw_text(lbl, NSMakePoint(x - ts.width * 0.5f, y0 - 17.0f),
                      tick_attrs());
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

    {
        NSDictionary *a = attrs(10.0f, 0,
            [NSColor colorWithCalibratedWhite:0.40f alpha:1.0f]);
        const char *xlab = "позиция на диске, GB";
        NSSize ts = text_size(xlab, a);

        if (g->cap > 0.0)
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

static const char *idle_hint(void)
{
    if (!g_is_root)
        return "нет прав на raw-диск - перезапустите приложение "
               "и введите пароль";
    return "выберите диск и нажмите Старт";
}

@interface LinView : NSView
@end

@implementation LinView

- (void)drawRect:(NSRect)dirty
{
    static dbuf snap = { NULL, 0, 0 };
    plotgeo g;
    double  raw = 0.0;
    int     i, running;
    uint64_t bytes, us;

    (void)dirty;

    pthread_mutex_lock(&g_lock);
    pts_copy(&g_lin, &snap);
    g.cap  = (snap.n > 0 || g_run == 1) ? g_lin_datacap : g_lin_selcap;
    bytes  = g_lin_bytes;
    us     = g_lin_time_us;
    pthread_mutex_unlock(&g_lock);
    running = (g_run == 1);

    if (g.cap <= 0.0)
        g.cap = g_lin_selcap;

    for (i = 0; i < snap.n; i++)
        if (snap.a[i].val > raw)
            raw = snap.a[i].val;

    g.pv   = plot_rect([self bounds]);
    g.ndiv = 4;
    g.vmax = (raw > 0.0) ? axis_vmax(raw, &g.ndiv) : 0.0;

    draw_frame(&g, "MB/s", 1.0);

    if (snap.n == 0) {
        draw_hint(g.pv, running ? "открываю диск..." : idle_hint());
        return;
    }

    {
        NSBezierPath *line = [NSBezierPath bezierPath];
        NSBezierPath *area = [NSBezierPath bezierPath];
        NSPoint first = geo_pt(&g, snap.a[0].off, snap.a[0].val);
        NSPoint last  = first;

        [area moveToPoint:NSMakePoint(first.x, NSMinY(g.pv))];
        for (i = 0; i < snap.n; i++) {
            NSPoint p = geo_pt(&g, snap.a[i].off, snap.a[i].val);
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

@interface RndView : NSView
@end

@implementation RndView

- (void)drawRect:(NSRect)dirty
{
    static dbuf snap = { NULL, 0, 0 };
    plotgeo g;
    double  raw = 0.0, thr = 1e30;
    int     i, over = 0, running;

    (void)dirty;

    pthread_mutex_lock(&g_lock);
    pts_copy(&g_rnd, &snap);
    g.cap = (snap.n > 0 || g_run == 2) ? g_rnd_datacap : g_rnd_selcap;
    pthread_mutex_unlock(&g_lock);
    running = (g_run == 2);

    if (g.cap <= 0.0)
        g.cap = g_rnd_selcap;

    for (i = 0; i < snap.n; i++)
        if (snap.a[i].val > raw)
            raw = snap.a[i].val;

    /* единичный выброс (рекалибровка, спин-ап) не сплющивает облако:
     * шкала по 2*p99, всё выше - красной меткой у верхнего края */
    if (snap.n >= 100 && g_rnd_p99 > 0.0) {
        thr = g_rnd_p99;
        if (raw > 3.0 * g_rnd_p99)
            raw = 2.0 * g_rnd_p99;
    }

    g.pv   = plot_rect([self bounds]);
    g.ndiv = 5;
    g.vmax = (raw > 0.0) ? axis_vmax(raw, &g.ndiv) : 0.0;

    draw_frame(&g, "ms", 1000.0);

    if (snap.n == 0) {
        draw_hint(g.pv, running ? "открываю диск, прогрев..." : idle_hint());
        return;
    }

    {
        NSBezierPath *dots   = [NSBezierPath bezierPath];
        NSBezierPath *spikes = [NSBezierPath bezierPath];

        for (i = 0; i < snap.n; i++) {
            NSPoint p = geo_pt(&g, snap.a[i].off, snap.a[i].val);
            if (snap.a[i].val > g.vmax) {
                /* за шкалой: треугольник остриём вверх у края */
                float y = NSMaxY(g.pv) - 1.0f;
                [spikes moveToPoint:NSMakePoint(p.x - 3.0f, y - 5.0f)];
                [spikes lineToPoint:NSMakePoint(p.x + 3.0f, y - 5.0f)];
                [spikes lineToPoint:NSMakePoint(p.x, y)];
                [spikes closePath];
                over++;
            } else if (snap.a[i].val > thr) {
                [spikes appendBezierPathWithRect:
                    NSMakeRect(p.x - 1.5f, p.y - 1.5f, 3.0f, 3.0f)];
            } else {
                [dots appendBezierPathWithRect:
                    NSMakeRect(p.x - 1.5f, p.y - 1.5f, 3.0f, 3.0f)];
            }
        }
        [col_dot() set];
        [dots fill];
        [col_spike() set];
        [spikes fill];
    }

    if (snap.n >= 20) {
        char lbl[48];
        snprintf(lbl, sizeof lbl, "med %.1f ms", g_rnd_med / 1000.0);
        draw_hmark(&g, g_rnd_med, col_avg(), lbl);
        snprintf(lbl, sizeof lbl, "p99 %.1f ms", g_rnd_p99 / 1000.0);
        draw_hmark(&g, g_rnd_p99, col_p99(), lbl);
    }

    if (over > 0) {
        char lbl[48];
        snprintf(lbl, sizeof lbl, "выше шкалы: %d", over);
        draw_text(lbl, NSMakePoint(NSMaxX(g.pv) - text_size(lbl,
                      tick_attrs()).width, NSMaxY(g.pv) + 5.0f),
                  attrs(10.0f, 0, col_spike()));
    }
}

@end

/* ------------------------------------------------------- статус-строки */

enum { ST_NORMAL, ST_DONE, ST_ERROR };

static void set_status(NSTextField *f, const char *s, int kind)
{
    if (f == NULL)
        return;
    [f setStringValue:[NSString stringWithUTF8String:s]];
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

    if (g_run == 1) {
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
        snprintf(s, sizeof s, "Нет прав на raw-диск - перезапустите "
                              "приложение и введите пароль администратора");
        set_status(g_lin_status, s, ST_ERROR);
    } else {
        if (g_lin_selcap > 0.0)
            snprintf(s, sizeof s,
                     "Будет прочитано ≈ %.1f GB из %.1f GB "
                     "(%d%% поверхности, сегменты по 32 MB)",
                     g_lin_selcap / GIB * (double)g_duty / 100.0,
                     g_lin_selcap / GIB, g_duty);
        else
            snprintf(s, sizeof s, "Выберите диск и нажмите Старт");
        set_status(g_lin_status, s, ST_NORMAL);
    }
}

static void status_rnd(void)
{
    char s[320];
    int  n, failed, canc, oserr, elapsed;
    dgrndresult res;
    char err[256];

    pthread_mutex_lock(&g_lock);
    n       = g_rnd.n;
    failed  = g_rnd_failed;
    oserr   = g_rnd_oserr;
    canc    = g_rnd_cancel;
    res     = g_rnd_res;
    elapsed = g_rnd_elapsed;
    strncpy(err, g_rnd_err, sizeof err);
    err[sizeof err - 1] = 0;
    pthread_mutex_unlock(&g_lock);

    if (g_run == 2) {
        int t = (int)(time(NULL) - g_run_t0);
        if (t < 0) t = 0;
        if (t > g_rnd_secs_run) t = g_rnd_secs_run;
        if (canc)
            snprintf(s, sizeof s, "Останавливаюсь...");
        else
            snprintf(s, sizeof s,
                     "ops %d   ·   %d / %d с   ·   med %.1f ms   ·   "
                     "p99 %.1f ms",
                     n, t, g_rnd_secs_run,
                     g_rnd_med / 1000.0, g_rnd_p99 / 1000.0);
        set_status(g_rnd_status, s, ST_NORMAL);
    } else if (failed) {
        err_text(s, sizeof s, oserr, err);
        set_status(g_rnd_status, s, ST_ERROR);
    } else if (n > 0) {
        snprintf(s, sizeof s,
                 "Готово: %llu ops   ·   med %.1f ms   ·   p99 %.1f ms   ·   "
                 "eio %llu   ·   %d:%02d%s",
                 (unsigned long long)res.ops,
                 res.med_us / 1000.0, res.p99_us / 1000.0,
                 (unsigned long long)res.eio,
                 elapsed / 60, elapsed % 60,
                 res.stopped ? "   [остановлено]" : "");
        set_status(g_rnd_status, s, ST_DONE);
    } else if (!g_is_root) {
        snprintf(s, sizeof s, "Нет прав на raw-диск - перезапустите "
                              "приложение и введите пароль администратора");
        set_status(g_rnd_status, s, ST_ERROR);
    } else {
        snprintf(s, sizeof s,
                 "%d с случайных чтений по 4 KB по всей поверхности - "
                 "нажмите Старт", g_rnd_secs);
        set_status(g_rnd_status, s, ST_NORMAL);
    }
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

static void lin_seg(void *ctx, const dglinseg *seg)
{
    (void)ctx;
    pthread_mutex_lock(&g_lock);
    pts_add(&g_lin, (double)seg->off, mbs_of(seg->bytes, seg->time_us));
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

    rc = dglincore(&cfg, lin_begin, lin_seg, NULL, NULL,
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
    (void)ctx;
    pthread_mutex_lock(&g_lock);
    g_rnd_datacap = (double)info->capacity;
    pthread_mutex_unlock(&g_lock);
}

static void rnd_op(void *ctx, const dgrndop *op)
{
    (void)ctx;
    pthread_mutex_lock(&g_lock);
    pts_add(&g_rnd, (double)op->off, (double)op->us);
    pthread_mutex_unlock(&g_lock);
}

static void *rnd_thread(void *arg)
{
    dgrndcfg    cfg;
    dgrndresult res;
    char        err[256];
    int         rc;
    time_t      t0 = time(NULL);

    (void)arg;
    memset(&cfg, 0, sizeof cfg);
    cfg.dev    = g_rnd_path;
    cfg.blk    = RND_BLK;
    cfg.secs   = g_rnd_secs_run;
    cfg.warmup = RND_WARMUP;
    cfg.seed   = 0;                 /* из времени/pid */

    rc = dgrndcore(&cfg, rnd_begin, rnd_op, NULL, NULL,
                   &g_rnd_cancel, &res, err, sizeof err);

    pthread_mutex_lock(&g_lock);
    g_rnd_failed = (rc != 0);
    g_rnd_oserr  = res.oserr;
    if (rc != 0)
        snprintf(g_rnd_err, sizeof g_rnd_err, "%s", err);
    g_rnd_res = res;
    g_rnd_elapsed = (int)(time(NULL) - t0);
    g_rnd_done = 1;
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

/* UI-поток: доступность контролов по состоянию прогона */
static void update_controls(void)
{
    int idle = (g_run == 0);

    [g_lin_start setEnabled:idle];
    [g_rnd_start setEnabled:idle];
    [g_lin_stop setEnabled:(g_run == 1 && !g_lin_cancel)];
    [g_rnd_stop setEnabled:(g_run == 2 && !g_rnd_cancel)];
    [g_lin_slider setEnabled:idle];
    [g_rnd_slider setEnabled:idle];
    [g_lin_dev setEnabled:idle];
    [g_rnd_dev setEnabled:idle];
}

static int start_thread(void *(*fn)(void *))
{
    pthread_t th;

    if (pthread_create(&th, NULL, fn, NULL) != 0)
        return 0;
    pthread_detach(th);
    return 1;
}

static void lin_start(void)
{
    if (g_run != 0)
        return;

    pthread_mutex_lock(&g_lock);
    pts_clear(&g_lin);
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
    g_run = 1;
    g_run_t0 = time(NULL);

    strncpy(g_lin_path, g_lin_sel, sizeof g_lin_path - 1);
    g_lin_path[sizeof g_lin_path - 1] = 0;
    g_lin_duty = g_duty;

    if (!start_thread(lin_thread)) {
        pthread_mutex_lock(&g_lock);
        snprintf(g_lin_err, sizeof g_lin_err, "не удалось создать поток");
        g_lin_failed = 1;
        g_lin_done = 1;
        pthread_mutex_unlock(&g_lock);
    }
    update_controls();
    status_lin();
}

static void rnd_start(void)
{
    if (g_run != 0)
        return;

    pthread_mutex_lock(&g_lock);
    pts_clear(&g_rnd);
    g_rnd_failed = 0;
    g_rnd_oserr = 0;
    g_rnd_err[0] = 0;
    g_rnd_done = 0;
    g_rnd_datacap = g_rnd_selcap;
    memset(&g_rnd_res, 0, sizeof g_rnd_res);
    pthread_mutex_unlock(&g_lock);

    g_rnd_med = g_rnd_p99 = 0.0;
    g_rnd_stat_n = -1;
    g_rnd_cancel = 0;
    g_run = 2;
    g_run_t0 = time(NULL);

    strncpy(g_rnd_path, g_rnd_sel, sizeof g_rnd_path - 1);
    g_rnd_path[sizeof g_rnd_path - 1] = 0;
    g_rnd_secs_run = g_rnd_secs;

    if (!start_thread(rnd_thread)) {
        pthread_mutex_lock(&g_lock);
        snprintf(g_rnd_err, sizeof g_rnd_err, "не удалось создать поток");
        g_rnd_failed = 1;
        g_rnd_done = 1;
        pthread_mutex_unlock(&g_lock);
    }
    update_controls();
    status_rnd();
}

/* UI-поток: выбор в popup поменялся - ёмкость для оси и подсказки */
static void sel_changed(int lin)
{
    if (lin) {
        g_lin_selcap = dg_probe_capacity(g_lin_sel);
        status_lin();
        [g_lin_view setNeedsDisplay:YES];
    } else {
        g_rnd_selcap = dg_probe_capacity(g_rnd_sel);
        status_rnd();
        [g_rnd_view setNeedsDisplay:YES];
    }
}

static void refresh_devs(void)
{
    if (g_run != 0)
        return;                     /* popup-ы заблокированы - не трогаем */
    if (fill_dev_popup(g_lin_dev, g_lin_sel, sizeof g_lin_sel))
        sel_changed(1);
    if (fill_dev_popup(g_rnd_dev, g_rnd_sel, sizeof g_rnd_sel))
        sel_changed(0);
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
        update_controls();
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
        update_controls();
        status_rnd();
    }
}

- (void)dutyChanged:(id)s
{
    char lbl[64];
    g_duty = [s intValue];
    snprintf(lbl, sizeof lbl, "%d %%", g_duty);
    [g_duty_val setStringValue:[NSString stringWithUTF8String:lbl]];
    if (g_run == 0)
        status_lin();
}

- (void)secsChanged:(id)s
{
    char lbl[64];
    g_rnd_secs = [s intValue];
    snprintf(lbl, sizeof lbl, "%d с", g_rnd_secs);
    [g_secs_val setStringValue:[NSString stringWithUTF8String:lbl]];
    if (g_run == 0)
        status_rnd();
}

- (void)devChanged:(id)s
{
    NSString   *t = [s titleOfSelectedItem];
    const char *u;
    int         lin = (s == g_lin_dev);
    char       *sel = lin ? g_lin_sel : g_rnd_sel;

    if (t == nil || (u = [t UTF8String]) == NULL || u[0] != '/')
        return;
    strncpy(sel, u, 63);
    sel[63] = 0;
    sel_changed(lin);
}

- (void)appActive:(NSNotification *)n
{
    (void)n;
    refresh_devs();
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
}

- (void)tick:(NSTimer *)timer
{
    int ldone, rdone;

    (void)timer;

    pthread_mutex_lock(&g_lock);
    ldone = g_lin_done;
    rdone = g_rnd_done;
    pthread_mutex_unlock(&g_lock);

    rnd_stats_update();

    if (g_run == 1 && ldone) {
        g_run = 0;
        update_controls();
        if (g_again) {
            g_again = 0;
            printf("again: restart linear\n");
            fflush(stdout);
            lin_start();
        }
    }
    if (g_run == 2 && rdone) {
        g_run = 0;
        update_controls();
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
            [self linStop:nil];
        if (g_run == 2)
            [self rndStop:nil];
    }

    if (g_close_at > 0 && time(NULL) >= g_close_at) {
        g_close_at = 0;
        printf("close: performClose\n");
        fflush(stdout);
        [g_win performClose:nil];
        return;
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
                   cur, nxt, g_lin.n, g_rnd.n);
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
    int i, done = 1;

    (void)n;
    g_lin_cancel = 1;
    g_rnd_cancel = 1;
    for (i = 0; i < 20; i++) {
        pthread_mutex_lock(&g_lock);
        done = (g_run != 1 || g_lin_done) && (g_run != 2 || g_rnd_done);
        pthread_mutex_unlock(&g_lock);
        if (done)
            break;
        usleep(50000);
    }
}

@end

/* ------------------------------------------------------------ сборка UI */

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

/*
 * Вкладка: ряд контролов (диск, слайдер, Старт/Стоп), статус-строка,
 * график на остальное место. lin = 1 - Linear, 0 - Random.
 */
static NSView *make_tab(Controller *ctl, NSSize sz, int lin)
{
    NSView        *v = [[NSView alloc] initWithFrame:
                            NSMakeRect(0.0f, 0.0f, sz.width, sz.height)];
    float          h = sz.height, w = sz.width;
    float          y = h - 36.0f;
    NSPopUpButton *pop;
    NSSlider      *sl;
    NSTextField   *val, *st;
    NSView        *gv;

    make_label(v, "Диск:", 10.0f, y + 4.0f, 40.0f, 1);
    pop = [[NSPopUpButton alloc]
        initWithFrame:NSMakeRect(54.0f, y - 1.0f, 170.0f, 26.0f) pullsDown:NO];
    [pop setTarget:ctl];
    [pop setAction:@selector(devChanged:)];
    [pop setAutoresizingMask:NSViewMinYMargin];
    [v addSubview:pop];
    [pop release];

    make_label(v, lin ? "Duty:" : "Время:", 236.0f, y + 4.0f, 52.0f, 1);
    sl = [[NSSlider alloc] initWithFrame:
        NSMakeRect(294.0f, y + 1.0f, 180.0f, 22.0f)];
    [sl setMinValue:lin ? 10.0 : 5.0];
    [sl setMaxValue:lin ? 100.0 : 120.0];
    [sl setIntValue:lin ? g_duty : g_rnd_secs];
    [sl setTarget:ctl];
    [sl setAction:lin ? @selector(dutyChanged:) : @selector(secsChanged:)];
    [sl setAutoresizingMask:NSViewMinYMargin];
    [v addSubview:sl];
    [sl release];

    {
        char lbl[32];
        if (lin)
            snprintf(lbl, sizeof lbl, "%d %%", g_duty);
        else
            snprintf(lbl, sizeof lbl, "%d с", g_rnd_secs);
        val = make_label(v, lbl, 480.0f, y + 4.0f, 60.0f, 0);
        [val setFont:[NSFont boldSystemFontOfSize:12.0f]];
    }

    st = [[NSTextField alloc]
        initWithFrame:NSMakeRect(14.0f, h - 62.0f, w - 28.0f, 17.0f)];
    [st setBezeled:NO];
    [st setEditable:NO];
    [st setSelectable:NO];
    [st setDrawsBackground:NO];
    [st setAutoresizingMask:NSViewWidthSizable | NSViewMinYMargin];
    [v addSubview:st];
    [st release];

    gv = [[(lin ? [LinView class] : [RndView class]) alloc] initWithFrame:
        NSMakeRect(0.0f, 0.0f, w, h - 68.0f)];
    [gv setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [v addSubview:gv];
    [gv release];

    if (lin) {
        g_lin_dev = pop; g_lin_slider = sl; g_duty_val = val;
        g_lin_status = st; g_lin_view = gv;
        g_lin_start = make_button(v, ctl, "Старт", @selector(linStart:),
                                  w - 206.0f, y - 4.0f);
        g_lin_stop  = make_button(v, ctl, "Стоп", @selector(linStop:),
                                  w - 108.0f, y - 4.0f);
        fill_dev_popup(pop, g_lin_sel, sizeof g_lin_sel);
    } else {
        g_rnd_dev = pop; g_rnd_slider = sl; g_secs_val = val;
        g_rnd_status = st; g_rnd_view = gv;
        g_rnd_start = make_button(v, ctl, "Старт", @selector(rndStart:),
                                  w - 206.0f, y - 4.0f);
        g_rnd_stop  = make_button(v, ctl, "Стоп", @selector(rndStop:),
                                  w - 108.0f, y - 4.0f);
        fill_dev_popup(pop, g_rnd_sel, sizeof g_rnd_sel);
    }
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
        mi = [submenu addItemWithTitle:
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
    [win setMinSize:NSMakeSize(820.0f, 520.0f)];
    root = [[[NSView alloc] initWithFrame:rc] autorelease];
    [root setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [win setContentView:root];

    /* вкладки с отступом от краёв окна - как в системных панелях */
    g_tabs = [[NSTabView alloc] initWithFrame:
                 NSInsetRect([root bounds], 8.0f, 6.0f)];
    [g_tabs setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [root addSubview:g_tabs];

    cr = [g_tabs contentRect];

    item = [[NSTabViewItem alloc] initWithIdentifier:nil];
    [item setLabel:[NSString stringWithUTF8String:"Linear"]];
    [item setView:make_tab(ctl, cr.size, 1)];
    [g_tabs addTabViewItem:item];
    [item release];

    item = [[NSTabViewItem alloc] initWithIdentifier:nil];
    [item setLabel:[NSString stringWithUTF8String:"Random"]];
    [item setView:make_tab(ctl, cr.size, 0)];
    [g_tabs addTabViewItem:item];
    [item release];
    [ctl windowDidResize:nil];

    g_lin_selcap = dg_probe_capacity(g_lin_sel);
    g_rnd_selcap = dg_probe_capacity(g_rnd_sel);
    update_controls();
    status_lin();
    status_rnd();

    /* dev-хуки: -ldev/-rdev PATH - диск вкладки, -lin / -rnd стартуют
     * реальный прогон, -cycle щёлкает вкладки и печатает счётчики */
    for (i = 1; i + 1 < argc; i++) {
        char *sel = NULL;
        if (strcmp(argv[i], "-ldev") == 0)
            sel = g_lin_sel;
        else if (strcmp(argv[i], "-rdev") == 0)
            sel = g_rnd_sel;
        if (sel != NULL) {
            strncpy(sel, argv[i + 1], 63);
            sel[63] = 0;
            fill_dev_popup(sel == g_lin_sel ? g_lin_dev : g_rnd_dev, sel, 64);
            sel_changed(sel == g_lin_sel);
        }
    }
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-lin") == 0) {
            [g_tabs selectTabViewItemAtIndex:0];
            lin_start();
        } else if (strcmp(argv[i], "-rnd") == 0) {
            [g_tabs selectTabViewItemAtIndex:1];
            rnd_start();
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
