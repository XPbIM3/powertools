/*
 * graphprobe.m - AppKit-рисование клетка-пробник для парка (10.2..10.5 PPC)
 *
 * Задача: ДО всего большого UI подтвердить живым фактом, что окно
 * NSWindow + NSView drawRect (NSBezierPath / NSRectFill) рисует на
 * целевых клетках матрицы UI_test.  Одна ступень UI-рецепта:
 * carbon-probe = окно+кнопка (Carbon), tray-probe = трей (AppKit),
 * здесь = ГРАФИК (AppKit drawRect) - новый слой.
 *
 * Что рисует (в одном окне, две панели):
 *   - верх: полилиния линейного свипа из CSV dbsweep (MB/s вдоль ёмкости)
 *   - низ: scatter random-чтений (сёсто 1 op: X = офсет, Y = лаг µs)
 *   - E-строки CSV (ошибки чтения =.bad sector) - красные квадраты
 *
 * Source CSV: argv[1], default /tmp/probe.csv.  Формат dbsweep.csv:
 *   L,<offset>,<bytes>,<latency_us>,<ok>     линейные блоки
 *   R,<offset>,<bytes>,<latency_us>          random ops
 *   E,<offset>,<errno>                       ошибки чтения
 *
 * ObjC строго objc1 (Panther/Jaguar runtime): без @"..."-литералов,
 * без свойств, без fast enumeration - в стиле diskgauge/traycpu.
 * Деплой: бандл GraphProbe.app + open по ssh (реепт каркаса).
 */
#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>

/* ------------------------------------------------------------ данные */

typedef struct {
    double off;   /* capacity position, bytes  */
    double val;   /* MB/s (L) or latency us (R) */
} dpt;

static dpt  *g_lin    = NULL;
static int   g_lin_n  = 0;
static dpt  *g_rnd    = NULL;
static int   g_rnd_n  = 0;
static dpt  *g_err    = NULL;
static int   g_err_n  = 0;
static double g_maxoff = 0.0;      /* farthest capacity touched */

static int pts_add(dpt **arr, int *n, double off, double val)
{
    {
        dpt *p = (dpt *)realloc(*arr, (size_t)(*n + 1) * sizeof(dpt));
        if (p == NULL)
            return 0;
        *arr = p;
    }
    (*arr)[*n].off = off;
    (*arr)[*n].val = val;
    (*n)++;
    return 1;
}

static int csv_load(const char *path)
{
    FILE  *f;
    char   line[512];

    f = fopen(path, "r");
    if (f == NULL)
        return 0;

    while (fgets(line, sizeof line, f) != NULL) {
        unsigned long long off = 0, bytes = 0, us = 0;
        double      mbs;

        if (line[0] == '#' || line[0] == '\n' || line[0] == '\0')
            continue;

        if (sscanf(line, "L,%llu,%llu,%llu", &off, &bytes, &us) == 3) {
            if (us <= 0)
                continue;
            mbs = (double)bytes / ((double)us / 1e6) / 1048576.0;
            if (!pts_add(&g_lin, &g_lin_n, (double)off, mbs))
                break;
            if ((double)off > g_maxoff) g_maxoff = (double)off;
        } else if (sscanf(line, "R,%llu,%llu,%llu", &off, &bytes, &us) == 3) {
            if (us <= 0)
                continue;
            if (!pts_add(&g_rnd, &g_rnd_n, (double)off, (double)us))
                break;
            if ((double)off > g_maxoff) g_maxoff = (double)off;
        } else if (sscanf(line, "E,%llu", &off) == 1) {
            if (!pts_add(&g_err, &g_err_n, (double)off, 0.0))
                break;
            if ((double)off > g_maxoff) g_maxoff = (double)off;
        }
    }
    fclose(f);
    return (g_lin_n + g_rnd_n + g_err_n) > 0;
}

/* ------------------------------------------------------- рисование */

static void draw_text(const char *s, NSPoint p, float size)
{
    static NSDictionary *attrs10 = NULL;
    static NSDictionary *attrs8  = NULL;
    NSDictionary        *attrs;
    NSString            *str;

    if (attrs10 == NULL) {
        attrs10 = [NSDictionary dictionaryWithObjectsAndKeys:
            [NSFont systemFontOfSize:10.0f], NSFontAttributeName, NULL];
        attrs8  = [NSDictionary dictionaryWithObjectsAndKeys:
            [NSFont systemFontOfSize:8.0f],  NSFontAttributeName, NULL];
    }
    attrs = (size <= 8.0f) ? attrs8 : attrs10;

    str = [NSString stringWithUTF8String:s];
    [str drawAtPoint:p withAttributes:attrs];
}

/* двойные оси: L-рамки + подписи X (GB) и Y списком наверх-вниз */
static void draw_pane(NSRect pv, double vmax, const char *ylab)
{
    NSBezierPath *bp = [NSBezierPath bezierPath];
    float x0 = pv.origin.x, y0 = pv.origin.y;
    float x1 = x0 + pv.size.width, y1 = y0 + pv.size.height;
    float gb, t;

    [[NSColor blackColor] set];
    [bp moveToPoint:NSMakePoint(x0, y0)];
    [bp lineToPoint:NSMakePoint(x0, y1)];       /* Y axis */
    [bp lineToPoint:NSMakePoint(x1, y1)];       /* top    */
    [bp setLineWidth:1.0f];
    [bp stroke];

    /* X ticks: раз в 8 GB */
    for (gb = 0.0; gb <= 64.0; gb += 8.0) {
        float x = x0 + (float)(gb * 1073741824.0 / g_maxoff) * pv.size.width;
        if (x > x1) break;
        NSBezierPath  *tick = [NSBezierPath bezierPath];
        [tick moveToPoint:NSMakePoint(x, y0)];
        [tick lineToPoint:NSMakePoint(x, y0 - 4.0f)];
        [tick stroke];
        {
            char lbl[24];
            snprintf(lbl, sizeof lbl, "%g", gb);
            draw_text(lbl, NSMakePoint(x - 6.0f, y0 - 13.0f), 8.0f);
        }
    }

    /* Y ticks: 5 делений от нуля до vmax */
    for (t = 0.0; t <= 5.0; t += 1.0) {
        float y   = y0 + (float)(t / 5.0) * pv.size.height;
        float val = (float)(vmax * (t / 5.0));
        char  lbl[32];
        NSBezierPath *tick = [NSBezierPath bezierPath];
        [tick moveToPoint:NSMakePoint(x0, y)];
        [tick lineToPoint:NSMakePoint(x0 - 4.0f, y)];
        [tick stroke];
        snprintf(lbl, sizeof lbl, "%g", val < 10.0 ? val : val);
        draw_text(lbl, NSMakePoint(x0 - 40.0f, y - 3.0f), 8.0f);
    }
    draw_text(ylab, NSMakePoint(x0 + 6.0f, y1 + 2.0f), 8.0f);
}

@interface GraphView : NSView
@end

@implementation GraphView

- (void)drawRect:(NSRect)dirty
{
    NSRect   f = [self frame];
    float    padl = 48.0f, padr = 14.0f;
    NSBezierPath *bp;
    double   topmax = 0.0, botmax = 0.0;
    int      i;

    [[NSColor whiteColor] set];
    NSRectFill(f);

    if (g_maxoff <= 0.0) {
        draw_text("no data - feed dbsweep.csv",
                  NSMakePoint(60.0f, NSHeight(f) * 0.5f), 10.0f);
        return;
    }

    /* autoscale: linear by max, random by near-max (сегменты и выбросы) */
    for (i = 0; i < g_lin_n; i++)
        if (g_lin[i].val > topmax) topmax = g_lin[i].val;
    for (i = 0; i < g_rnd_n; i++)
        if (g_rnd[i].val > botmax) botmax = g_rnd[i].val;
    if (topmax <= 0.0) topmax = 1.0;
    if (botmax <= 0.0) botmax = 1.0;
    topmax *= 1.10;
    botmax *= 1.10;

    /* панель 1: линейная полилиния (верхняя половина) */
    {
        NSRect pv = NSMakeRect(padl, NSHeight(f) * 0.5f + 18.0f,
                               NSWidth(f) - padl - padr,
                               NSHeight(f) * 0.5f - 46.0f);
        draw_pane(pv, topmax, "linear read  MB/s  vs  capacity (GB)");

        bp = [[NSBezierPath bezierPath] retain];   /* objc1: no C99 */
        [[NSColor blueColor] set];
        [bp setLineWidth:1.2f];
        for (i = 0; i < g_lin_n; i++) {
            NSPoint p = NSMakePoint(
                pv.origin.x + (float)(g_lin[i].off / g_maxoff) * pv.size.width,
                pv.origin.y + (float)(g_lin[i].val / topmax) * pv.size.height);
            if (i == 0)
                [bp moveToPoint:p];
            else
                [bp lineToPoint:p];
        }
        [bp stroke];
        [bp release];
    }

    /* панель 2: scatter random (нижняя половина) */
    {
        NSRect pv = NSMakeRect(padl, 10.0f,
                               NSWidth(f) - padl - padr,
                               NSHeight(f) * 0.5f - 42.0f);
        float x;

        draw_pane(pv, botmax, "random read  lag us  vs  capacity (GB)");

        /* точки: 2x2 rect fill - тысячи стоят бесплатно */
        [[NSColor blackColor] set];
        for (i = 0; i < g_rnd_n; i++) {
            x = pv.origin.x + (float)(g_rnd[i].off / g_maxoff) * pv.size.width;
            NSRectFill(NSMakeRect(x, pv.origin.y +
                 (float)(g_rnd[i].val / botmax) * pv.size.height,
                 2.0f, 2.0f));
        }

        /* ошибки = красные квадраты прямо по оси X */
        [[NSColor redColor] set];
        for (i = 0; i < g_err_n; i++) {
            x = pv.origin.x + (float)(g_err[i].off / g_maxoff) * pv.size.width;
            NSRectFill(NSMakeRect(x - 2.0f, pv.origin.y, 4.0f, 4.0f));
        }
        {
            char lbl[48];
            snprintf(lbl, sizeof lbl, "EIO marks: %d", g_err_n);
            draw_text(lbl, NSMakePoint(pv.origin.x + 6.0f,
                                       pv.origin.y - 2.0f), 8.0f);
        }
    }

    /* сводка сверху */
    {
        char lbl[160];
        snprintf(lbl, sizeof lbl,
                 "linear pts %d   random pts %d   errors %d",
                 g_lin_n, g_rnd_n, g_err_n);
        draw_text(lbl, NSMakePoint(padl, NSHeight(f) - 12.0f), 10.0f);
    }
}

- (void)dealloc
{
    [super dealloc];
}

@end

/* ------------------------------------------------------------ каркас */

@interface ProbeTarget : NSObject
- (void)quit:(id)sender;
@end

@implementation ProbeTarget

- (void)quit:(id)sender
{
    [NSApp terminate:sender];
}

@end

int main(int argc, char *argv[])
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    NSWindow          *win;
    NSRect             rc = NSMakeRect(0.0f, 0.0f, 760.0f, 460.0f);
    NSString          *csv = nil;
    ProbeTarget       *t;
    NSMenu            *menu, *submenu;
    NSMenuItem        *mi;

    (void)argc;
    {
        NSProcessInfo *pi = [NSProcessInfo processInfo];
        NSArray       *args = [pi arguments];
        NSString      *psntag = [NSString stringWithUTF8String:"-psn"];
        int            i;
        /* open рождает -psn_... - это НЕ путь к CSV, пропускать */
        for (i = 1; i < [args count]; i++) {
            NSString *a = [args objectAtIndex:i];
            if ([a hasPrefix:psntag])
                continue;
            csv = [a retain];
            break;
        }
    }

    if (csv == nil || !csv_load([csv UTF8String]))
        csv_load("/tmp/probe.csv");   /* каркас-дефолт пробника */

    [NSApplication sharedApplication];
    t = [[ProbeTarget alloc] init];

    /* меню с Quit (без nib - программно) */
    menu = [[NSMenu alloc] initWithTitle:
                [NSString stringWithUTF8String:"main"]];
    {
        NSMenuItem *appmi = [menu addItemWithTitle:
            [NSString stringWithUTF8String:"GraphProbe"]
            action:NULL keyEquivalent:
            [NSString stringWithUTF8String:""]];
        submenu = [[NSMenu alloc] initWithTitle:
            [NSString stringWithUTF8String:"GraphProbe"]];
        mi = [submenu addItemWithTitle:
            [NSString stringWithUTF8String:"Выход"]
            action:@selector(quit:)
            keyEquivalent:[NSString stringWithUTF8String:"q"]];
        [mi setTarget:t];
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
                    "graphprobe - dbsweep.csv view"]];
    [win setContentView:[[[GraphView alloc] initWithFrame:rc] autorelease]];
    [win center];
    [win makeKeyAndOrderFront:nil];

    [NSApp run];

    if (csv != nil) [csv release];
    [t release];
    [win release];
    [pool release];
    return 0;
}
