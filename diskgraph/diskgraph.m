/*
 * diskgraph.m - UI этап 4a: две вкладки (Linear / Random) на мок-данных
 *
 * Отладка ВНЕШНЕГО ВИДА без запуска алгоритмов: графики питаются
 * искусственными данными, сгенерированными по профилю реального
 * disk0 (quicksilver, 55.9 G):
 *   - linear: зонный спад 36 -> 24 MB/s, шум +-0.5, редкие провалы;
 *             кривая отрастает слева направо по мере "прохода";
 *   - random: лаг 4K-чтений вокруг 14.3 ms, спайки до 40 ms;
 *             точки постятся пачками по мере "замера".
 *
 * Правила вида (после ревизии):
 *   - прогресс/сводка живут ТОЛЬКО в статус-строке вкладки, на графике
 *     их нет - никакого дублирования;
 *   - у осей есть подписи: X - "поверхность диска, GB", Y - "MB/s"/"ms";
 *   - Y-шкала подгоняется под данные (круглые деления, без пустого верха);
 *   - вкладка тянется вместе с окном, график не вылезает за контент;
 *   - селектор устройства на каждой вкладке: /dev/rdiskN (raw) и
 *     /dev/diskN (buffered) - парами, чтобы сравнивать; на этом этапе
 *     выбор чисто визуальный, реального чтения нет.
 *
 * Dev-хуки (для скриншотов и проверок): -lin / -rnd автостарт мока,
 * -cycle сам щёлкает вкладки и печатает счётчики точек (проверка, что
 * данные не теряются), -devs печатает список дисков и выходит,
 * -w N -h N задают стартовый размер окна.
 *
 * Стык для будущего движка: мок-слой гадит данные через те же
 * pts_add/массивы, что потом будет наполнять sink настоящего ядра
 * (dglincore/dgrndcore). UI-код переписывать не придётся.
 *
 * Рамки этапа: NO IOKit, NO root/Auth, NO реальных дисков - только
 * вид, вкладки, слайдеры, живой рендер.
 *
 * ObjC строго objc1 (Jaguar..Leopard runtime): без @"..."-литералов,
 * без свойств и fast enumeration - стиль diskgauge/graphprobe.
 */
#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#include <dirent.h>
#include <sys/types.h>
#include <unistd.h>
#include <math.h>

/* ---- мок-геометрия: реальный disk0 quicksilver-g4-867 ---- */
#define MOCK_CAP   (60021399040.0)          /* 55.90 G, байты */
#define MOCK_CHUNK (32.0 * 1048576.0)       /* 32 MiB чанк    */
#define MOCK_OPS_PER_S 70                   /* ops/s random   */
#define TICK_SEC   0.25                     /* шаг мока       */

/* окно/вкладка */
#define WIN_W 1000.0f
#define WIN_H  720.0f

typedef struct {
    double off;   /* позиция по ёмкости, байты */
    double val;   /* MB/s (linear) / µs (random) */
} dpt;

static dpt *g_lin = NULL;  static int g_lin_n = 0;
static dpt *g_rnd = NULL;  static int g_rnd_n = 0;

static double g_cap = MOCK_CAP;

/* параметры "приборов" (слайдеры) и выбранное устройство */
static int  g_duty     = 10;
static int  g_rnd_secs = 30;
static char g_dev[64]  = "/dev/rdisk0";

/* состояние мока */
static int    g_lin_on = 0;
static int    g_lin_chunks = 0;   /* сегментов в текущем проходе */
static int    g_lin_idx = 0;
static int    g_lin_feed = 1;     /* сегментов за тик            */
static double g_lin_skip = 0.0;

static int    g_rnd_on = 0;
static double g_rnd_elapsed = 0.0;

/* dev-хук -cycle: сам щёлкает вкладки и печатает счётчики точек */
static int    g_cycle = 0;
static int    g_cycle_ticks = 0;

/* UI-хвосты */
static NSTextField   *g_lin_status = NULL;
static NSTextField   *g_rnd_status = NULL;
static NSTextField   *g_duty_val  = NULL;
static NSTextField   *g_secs_val  = NULL;
static NSButton      *g_lin_start = NULL;
static NSButton      *g_rnd_start = NULL;
static NSView        *g_lin_view = NULL;
static NSView        *g_rnd_view = NULL;
static NSPopUpButton *g_lin_dev = NULL;
static NSPopUpButton *g_rnd_dev = NULL;
static NSTabView     *g_tabs = NULL;
static NSView        *g_root = NULL;

/* ------------------------------------------------------------ данные */

static int pts_add(dpt **arr, int *npt, double off, double val)
{
    {
        dpt *p = (dpt *)realloc(*arr, (size_t)(*npt + 1) * sizeof(dpt));
        if (p == NULL)
            return 0;
        *arr = p;
    }
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

/* /dev/rdiskN (raw) и /dev/diskN (buffered) - только целые диски,
 * разбиения (sN) не подмешиваем: их бессмысленно гонять целиком.
 * Пары идут рядом, чтобы можно было сравнить raw/buffered. */
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
            int         val = 0, ok = 1, raw;

            if (strncmp(nm, "rdisk", 5) == 0) {
                raw = 1;
                p = nm + 5;
            } else if (strncmp(nm, "disk", 4) == 0) {
                raw = 0;
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
        int  have_raw, have_buf;

        snprintf(pa, sizeof pa, "/dev/rdisk%d", nums[i]);
        snprintf(pb, sizeof pb, "/dev/disk%d",  nums[i]);
        have_raw = (access(pa, F_OK) == 0);
        have_buf = (access(pb, F_OK) == 0);
        if (have_raw)
            [a addObject:[NSString stringWithUTF8String:pa]];
        if (have_buf)
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
        if ([t isEqualToString:
                [NSString stringWithUTF8String:g_dev]])
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

/* ------------------------------------------------------- мок-двигалка */

/* профиль поверхности: внешние зоны быстрее, шум, редкие провалы */
static double mock_lin_value(double off)
{
    double frac = off / g_cap;
    double base = 36.5 - 13.0 * frac;
    double noise = ((double)rand() / (double)RAND_MAX - 0.5) * 1.0;
    if ((double)rand() / (double)RAND_MAX < 0.03)
        base -= 5.0 + 4.0 * ((double)rand() / (double)RAND_MAX);
    return base + noise;
}

/* лаг random 4K: сгущение у 13-15 ms + редкий спайк до ~40 ms */
static double mock_rnd_value(void)
{
    double u = (double)rand() / (double)RAND_MAX;
    double v = (double)rand() / (double)RAND_MAX;
    double ms = 13.2 + 6.0 * u * v * 2.0;
    if ((double)rand() / (double)RAND_MAX < 0.02)
        ms += 10.0 + 15.0 * ((double)rand() / (double)RAND_MAX);
    return ms * 1000.0;
}

static void status_lin(void)
{
    char s[192];
    if (g_lin_n == 0 && !g_lin_on) {
        snprintf(s, sizeof s, "linear: нет данных - нажмите Старт");
    } else if (g_lin_on) {
        snprintf(s, sizeof s,
                 "linear: сегмент %d / %d   avg %.1f MB/s   duty %d%%",
                 g_lin_idx, g_lin_chunks,
                 avg_of(g_lin, g_lin_n), g_duty);
    } else {
        snprintf(s, sizeof s,
                 "linear готово: %d сегментов   avg %.1f MB/s   duty %d%%",
                 g_lin_n, avg_of(g_lin, g_lin_n), g_duty);
    }
    [g_lin_status setStringValue:[NSString stringWithUTF8String:s]];
}

static void status_rnd(void)
{
    char s[192];
    if (g_rnd_n == 0 && !g_rnd_on) {
        snprintf(s, sizeof s, "random: нет данных - нажмите Старт");
    } else if (g_rnd_on) {
        snprintf(s, sizeof s,
                 "random: ops %d   %.1f / %d s   med %.1f ms   p99 %.1f ms",
                 g_rnd_n, g_rnd_elapsed, g_rnd_secs,
                 pctl_of(g_rnd, g_rnd_n, 0.50) / 1000.0,
                 pctl_of(g_rnd, g_rnd_n, 0.99) / 1000.0);
    } else {
        snprintf(s, sizeof s,
                 "random готово: %d ops   med %.1f ms   p99 %.1f ms",
                 g_rnd_n,
                 pctl_of(g_rnd, g_rnd_n, 0.50) / 1000.0,
                 pctl_of(g_rnd, g_rnd_n, 0.99) / 1000.0);
    }
    [g_rnd_status setStringValue:[NSString stringWithUTF8String:s]];
}

static void mock_lin_start(void)
{
    if (g_lin_on)
        return;
    g_lin_n = 0;
    g_lin_idx = 0;
    g_lin_skip = MOCK_CHUNK * (double)(100 - g_duty) / (double)g_duty;
    g_lin_chunks = (int)(g_cap / (MOCK_CHUNK + g_lin_skip));
    if (g_lin_chunks < 1)
        g_lin_chunks = 1;
    g_lin_feed = g_lin_chunks / 200 + 1;   /* ~50 s на полный проход */
    g_lin_on = 1;
    [g_lin_start setEnabled:NO];
    status_lin();
}

static void mock_lin_stop(void)
{
    g_lin_on = 0;
    [g_lin_start setEnabled:YES];
    status_lin();
}

static void mock_lin_tick(void)
{
    int k;
    for (k = 0; k < g_lin_feed && g_lin_idx < g_lin_chunks; k++, g_lin_idx++) {
        double off = (double)g_lin_idx * (MOCK_CHUNK + g_lin_skip);
        if (off + MOCK_CHUNK > g_cap)
            off = g_cap - MOCK_CHUNK;
        pts_add(&g_lin, &g_lin_n, off, mock_lin_value(off));
    }
    if (g_lin_idx >= g_lin_chunks) {
        g_lin_on = 0;
        [g_lin_start setEnabled:YES];
    }
    status_lin();
    [g_lin_view setNeedsDisplay:YES];
}

static void mock_rnd_start(void)
{
    if (g_rnd_on)
        return;
    g_rnd_n = 0;
    g_rnd_elapsed = 0.0;
    g_rnd_on = 1;
    [g_rnd_start setEnabled:NO];
    status_rnd();
}

static void mock_rnd_stop(void)
{
    g_rnd_on = 0;
    [g_rnd_start setEnabled:YES];
    status_rnd();
}

static void mock_rnd_tick(void)
{
    int k;
    int per = (int)(MOCK_OPS_PER_S * TICK_SEC + 0.5);
    for (k = 0; k < per; k++) {
        double off = ((double)rand() / (double)RAND_MAX) * g_cap;
        pts_add(&g_rnd, &g_rnd_n, off, mock_rnd_value());
    }
    g_rnd_elapsed += TICK_SEC;
    if (g_rnd_elapsed >= (double)g_rnd_secs)
        g_rnd_on = 0, [g_rnd_start setEnabled:YES];
    status_rnd();
    [g_rnd_view setNeedsDisplay:YES];
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

static NSDictionary *text_attrs_gray(float size)
{
    static NSDictionary *g9 = NULL;

    if (g9 == NULL) {
        g9 = [[NSDictionary dictionaryWithObjectsAndKeys:
            [NSFont systemFontOfSize:9.0f], NSFontAttributeName,
            [NSColor darkGrayColor],         NSForegroundColorAttributeName,
            NULL] retain];
    }
    (void)size;
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
    [str drawAtPoint:p withAttributes:text_attrs_gray(9.0f)];
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

/* Y-максимум под данные: крпуные деления, запас ~8%, без пустого верха */
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

/* сетка + оси + подписи X/Y */
static void draw_plot(NSRect pv, double vmax, int ndiv,
                      int y_ms, const char *ylab)
{
    float x0 = pv.origin.x, y0 = pv.origin.y;
    float x1 = x0 + pv.size.width, y1 = y0 + pv.size.height;
    NSBezierPath *grid = [NSBezierPath bezierPath];
    NSBezierPath *axes = [NSBezierPath bezierPath];
    int   i, gb;
    float t;

    /* сетка */
    [[NSColor colorWithCalibratedWhite:0.88f alpha:1.0f] set];
    for (i = 1; i < ndiv; i++) {
        float y = y0 + ((float)i / (float)ndiv) * pv.size.height;
        [grid moveToPoint:NSMakePoint(x0, y)];
        [grid lineToPoint:NSMakePoint(x1, y)];
    }
    for (gb = 8; gb < 9999; gb += 8) {
        float x = x0 + (float)((double)gb * 1073741824.0 / g_cap) *
                       pv.size.width;
        if (x >= x1) break;
        [grid moveToPoint:NSMakePoint(x, y0)];
        [grid lineToPoint:NSMakePoint(x, y1)];
    }
    [grid setLineWidth:1.0f];
    [grid stroke];

    /* рамка графика: правый/верхний край видны */
    [[NSColor colorWithCalibratedWhite:0.78f alpha:1.0f] set];
    [axes moveToPoint:NSMakePoint(x0, y0)];
    [axes lineToPoint:NSMakePoint(x1, y0)];
    [axes lineToPoint:NSMakePoint(x1, y1)];
    [axes lineToPoint:NSMakePoint(x0, y1)];
    [axes closePath];
    [axes setLineWidth:1.0f];
    [axes stroke];

    /* деления X и подписи GB */
    [[NSColor blackColor] set];
    for (gb = 0; gb < 9999; gb += 8) {
        float x = x0 + (float)((double)gb * 1073741824.0 / g_cap) *
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

    /* деления Y и подписи значений */
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

    /* подписи осей: X - что и в чём, Y - единица */
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
    double raw = 0.0, vmax;
    int    ndiv = 4, i;

    (void)dirty;
    [[NSColor whiteColor] set];
    NSRectFill(f);

    pv = NSMakeRect(padl, padb,
                    NSWidth(f) - padl - padr,
                    NSHeight(f) - padt - padb);

    for (i = 0; i < g_lin_n; i++)
        if (g_lin[i].val > raw)
            raw = g_lin[i].val;
    vmax = (raw > 0.0) ? axis_vmax(raw, &ndiv) : 40.0;

    draw_plot(pv, vmax, ndiv, 0, "MB/s");

    if (g_lin_n == 0)
        return;

    /* метка текущей позиции прохода */
    {
        float mx = pv.origin.x +
                   (float)(g_lin[g_lin_n - 1].off / g_cap) * pv.size.width;
        [[NSColor colorWithCalibratedWhite:0.65f alpha:1.0f] set];
        [NSBezierPath strokeLineFromPoint:NSMakePoint(mx, pv.origin.y)
                                  toPoint:NSMakePoint(mx, pv.origin.y +
                                                      pv.size.height)];
    }

    bp = [[NSBezierPath bezierPath] retain];
    [[NSColor blueColor] set];
    [bp setLineWidth:1.4f];
    for (i = 0; i < g_lin_n; i++) {
        NSPoint p = NSMakePoint(
            pv.origin.x + (float)(g_lin[i].off / g_cap) * pv.size.width,
            pv.origin.y + (float)(g_lin[i].val / vmax) * pv.size.height);
        if (i == 0)
            [bp moveToPoint:p];
        else
            [bp lineToPoint:p];
    }
    [bp stroke];
    [bp release];
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
    double raw = 0.0, vmax, thr;
    int    ndiv = 5, i;

    (void)dirty;
    [[NSColor whiteColor] set];
    NSRectFill(f);

    pv = NSMakeRect(padl, padb,
                    NSWidth(f) - padl - padr,
                    NSHeight(f) - padt - padb);

    for (i = 0; i < g_rnd_n; i++)
        if (g_rnd[i].val > raw)
            raw = g_rnd[i].val;
    vmax = (raw > 0.0) ? axis_vmax(raw, &ndiv) : 50.0;

    draw_plot(pv, vmax, ndiv, 1, "ms");

    if (g_rnd_n == 0)
        return;

    /* спайки (хвост выше p99) - красным, остальное - чёрным */
    thr = (g_rnd_n >= 100) ? pctl_of(g_rnd, g_rnd_n, 0.99) : 1e30;
    for (i = 0; i < g_rnd_n; i++) {
        float x = pv.origin.x +
                  (float)(g_rnd[i].off / g_cap) * pv.size.width;
        float y = pv.origin.y +
                  (float)(g_rnd[i].val / vmax) * pv.size.height;
        if (g_rnd[i].val > thr)
            [[NSColor redColor] set];
        else
            [[NSColor blackColor] set];
        NSRectFill(NSMakeRect(x - 1.5f, y - 1.5f, 3.0f, 3.0f));
    }
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
- (void)tick:(NSTimer *)timer;
- (void)quit:(id)s;
- (void)windowDidResize:(NSNotification *)n;
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
    mock_lin_start();
}

- (void)linStop:(id)s
{
    (void)s;
    mock_lin_stop();
}

- (void)rndStart:(id)s
{
    (void)s;
    mock_rnd_start();
}

- (void)rndStop:(id)s
{
    (void)s;
    mock_rnd_stop();
}

- (void)dutyChanged:(id)s
{
    char lbl[64];
    g_duty = [s intValue];
    snprintf(lbl, sizeof lbl, "%d%%", g_duty);
    [g_duty_val setStringValue:[NSString stringWithUTF8String:lbl]];
    if (!g_lin_on)
        status_lin();
}

- (void)secsChanged:(id)s
{
    char lbl[64];
    g_rnd_secs = [s intValue];
    snprintf(lbl, sizeof lbl, "%d s", g_rnd_secs);
    [g_secs_val setStringValue:[NSString stringWithUTF8String:lbl]];
    if (!g_rnd_on)
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
    (void)timer;
    if (g_lin_on)
        mock_lin_tick();
    if (g_rnd_on)
        mock_rnd_tick();

    if (g_cycle) {
        g_cycle_ticks++;
        if (g_cycle_ticks % (int)(3.0 / TICK_SEC) == 0) {
            int cur = (int)[g_tabs indexOfTabViewItem:
                                [g_tabs selectedTabViewItem]];
            int nxt = (cur == 0) ? 1 : 0;
            [g_tabs selectTabViewItemAtIndex:nxt];
            printf("cycle: tab %d -> %d   lin_n %d   rnd_n %d\n",
                   cur, nxt, g_lin_n, g_rnd_n);
            fflush(stdout);
            [g_lin_view setNeedsDisplay:YES];
            [g_rnd_view setNeedsDisplay:YES];
        }
    }
}

- (void)quit:(id)s
{
    (void)s;
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
    return [pop autorelease];   /* живёт в супервью, указатель остаётся валидным */
}

static NSView *make_lin_tab(Controller *ctl, NSSize sz)
{
    NSView *v = [[NSView alloc] initWithFrame:
        NSMakeRect(0.0f, 0.0f, sz.width, sz.height)];
    NSSlider *sl;
    NSButton *b;
    float     h = sz.height, w = sz.width;
    float     y = h - 34.0f;

    make_label(v, "диск", 12.0f, y + 3.0f, 36.0f);
    g_lin_dev = make_dev_popup(v, ctl, 50.0f, y - 2.0f, 168.0f);

    make_label(v, "duty", 234.0f, y + 3.0f, 36.0f);
    sl = [[NSSlider alloc] initWithFrame:NSMakeRect(272.0f, y - 4.0f,
                                                    180.0f, 24.0f)];
    [sl setMinValue:10.0];
    [sl setMaxValue:100.0];
    [sl setIntValue:g_duty];
    [sl setTarget:ctl];
    [sl setAction:@selector(dutyChanged:)];
    [sl setAutoresizingMask:NSViewMinYMargin];
    [v addSubview:sl];
    [sl release];

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
    NSSlider *sl;
    NSButton *b;
    float     h = sz.height, w = sz.width;
    float     y = h - 34.0f;

    make_label(v, "диск", 12.0f, y + 3.0f, 36.0f);
    g_rnd_dev = make_dev_popup(v, ctl, 50.0f, y - 2.0f, 168.0f);

    make_label(v, "секунд", 234.0f, y + 3.0f, 52.0f);
    sl = [[NSSlider alloc] initWithFrame:NSMakeRect(290.0f, y - 4.0f,
                                                    162.0f, 24.0f)];
    [sl setMinValue:5.0];
    [sl setMaxValue:120.0];
    [sl setIntValue:g_rnd_secs];
    [sl setTarget:ctl];
    [sl setAction:@selector(secsChanged:)];
    [sl setAutoresizingMask:NSViewMinYMargin];
    [v addSubview:sl];
    [sl release];

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
    srand((unsigned)time(NULL));

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
                    "DiskGraph - UI mock (Linear / Random)"]];
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

    /* dev-хук для скриншотов: -lin / -rnd автостартуют мок и вкладку */
    {
        NSArray *args = [[NSProcessInfo processInfo] arguments];
        int i;
        for (i = 1; i < [args count]; i++) {
            NSString *a = [args objectAtIndex:i];
            if ([a isEqualToString:@"-lin"]) {
                [g_tabs selectTabViewItemAtIndex:0];
                mock_lin_start();
            } else if ([a isEqualToString:@"-rnd"]) {
                [g_tabs selectTabViewItemAtIndex:1];
                mock_rnd_start();
            } else if ([a isEqualToString:@"-cycle"]) {
                g_cycle = 1;      /* оба мока + авто-щёлканье вкладок */
                mock_lin_start();
                mock_rnd_start();
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
