/*
 * diskgraph.m - оконный UI бенчмарка поверх dbsweep для 10.2..10.5 PPC
 *
 * Ступень 3 (рецепт каркаса: carbon-probe окно -> traycpu трей ->
 * diskgauge боевая).  Окно: popup девайсов (IOMedia Whole) + слайдер
 * duty 10..100 с ростом оценки времени + поле random-секунд +
 * Start/Stop + график в две панели:
 *   верх - линейный пунктирный свип вдоль всей ёмкости (MB/s)
 *   низ  - scatter: случайные 4K-чтения (X = ёмкость, Y = лаг µs)
 *
 * Движок (dbsweep) живёт в Resources бандла и запускается под root
 * через AuthorizationExecuteWithPrivileges (password-диалог системы,
 * API с 10.2).  stdout движка не читаем - UI TAIL'ит его CSV
 * (/tmp/diskgraph.csv) таймером: fopen, fseek(сохр. офсет), письма
 * до EOF, прирост -> setNeedsDisplay.  Никаких потоков.
 *
 * Стоп: killall dbsweep под root (движок ловит SIGTERM и аккуратно
 * фиксирует CSV: fflush в csv_line - строки не теряются).
 *
 * Raw-ноды root:operator 640 - движок под root обязателен.
 *
 * ObjC строго objc1 (Jaguar..Leopard runtime): без @"-литералов,
 * без свойств и fast enumeration - стиль diskgauge/graphprobe.
 */
#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import <Security/Authorization.h>
#import <IOKit/IOKitLib.h>       /* leopard host: no umbrella IOKit.h */

#define CSV_PATH "/tmp/diskgraph.csv"

/* ------------------------------------------------------------ данные */

/* чистые глобалы логики */

typedef struct {
    double off;   /* Позиция по ёмкости, байты */
    double val;   /* MB/s (L) или лаг us (R)    */
} dpt;

static dpt  *g_lin = NULL;   static int   g_lin_n   = 0;
static dpt  *g_rnd = NULL;   static int   g_rnd_n   = 0;
static dpt  *g_err = NULL;   static int   g_err_n   = 0;
static double g_maxoff = 0.0;

static long long  g_tofs     = 0;    /* tail-офсет CSV            */
static int        g_running  = 0;
static double     g_linspeed = 0.0;  /* MB/s на текущей ходке     */
static uint64_t   g_linbytes = 0;
static double     g_startt   = 0.0;  /* us запуска ходки          */
static double     g_devbytes = 0.0;  /* ёмкость выбранного        */

static int   g_duty    = 33;
static int   g_rndsecs = 30;

/* перечень девайсов */
static char  **g_devnames = NULL;    /* "disk0"                  */
static double *g_devsizes = NULL;    /* байты                    */
static int     g_ndev     = 0;
static int     g_curdev   = 0;

@class GraphView;

static NSPopUpButton *g_popup    = NULL;
static NSTextField   *g_dutylab  = NULL;
static NSTextField   *g_status   = NULL;
static NSButton      *g_startbtn = NULL;
static GraphView     *g_view     = NULL;

/* утилиты времени (mach_absolute_time, как в dbsweep) */
#include <mach/mach_time.h>
static double g_ns_per_tick = 1.0;

static uint64_t now_us(void)
{
    return (uint64_t)((double)mach_absolute_time() * g_ns_per_tick /
                      1000.0);
}

static void clock_init(void)
{
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    g_ns_per_tick = (double)tb.numer / (double)tb.denom;
}

/* ------------------------------------------------------------ csv util */

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

static void data_reset(void)
{
    g_lin_n = g_rnd_n = g_err_n = 0;
    g_maxoff = 0.0;
    g_tofs = 0;
    g_linspeed = 0.0;
    g_linbytes = 0;
    g_startt = now_us();
}

/* прирост CSV; 1 = увидели сводку движка (финиш) */
static int tail_tick(void)
{
    FILE *f;
    char  line[512];
    int   done = 0;

    f = fopen(CSV_PATH, "r");
    if (f == NULL)
        return 0;
    if (fseek(f, g_tofs, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }
    while (fgets(line, sizeof line, f) != NULL) {
        unsigned long long off = 0, bytes = 0, us = 0;
        double mbs;

        if (line[0] == '\n' || line[0] == '\0')
            continue;

        if (sscanf(line, "L,%llu,%llu,%llu", &off, &bytes, &us) == 3) {
            if (us <= 0)
                continue;
            mbs = (double)bytes / ((double)us / 1e6) / 1048576.0;
            if (pts_add(&g_lin, &g_lin_n, (double)off, mbs)) {
                if ((double)off > g_maxoff) g_maxoff = (double)off;
                g_linbytes += (uint64_t)bytes;
            }
        } else if (sscanf(line, "R,%llu,%llu,%llu",
                          &off, &bytes, &us) == 3) {
            if (us <= 0)
                continue;
            if (pts_add(&g_rnd, &g_rnd_n, (double)off, (double)us) &&
                (double)off > g_maxoff)
                g_maxoff = (double)off;
        } else if (sscanf(line, "E,%llu", &off) == 1) {
            if (pts_add(&g_err, &g_err_n, (double)off, 0.0) &&
                (double)off > g_maxoff)
                g_maxoff = (double)off;
        } else if (strncmp(line, "# lin points=", 13) == 0 ||
                   strncmp(line, "# rnd ops=", 10) == 0) {
            done = 1;            /* движок дописал сводку */
        }
    }
    g_tofs = ftell(f);
    fclose(f);
    return done;
}

/* ------------------------------------------------------- рисование */

static void draw_text(NSString *str, NSPoint p, float size)
{
    static NSDictionary *attrs10 = NULL;
    static NSDictionary *attrs8  = NULL;
    NSDictionary        *attrs;

    if (attrs10 == NULL) {
        attrs10 = [NSDictionary dictionaryWithObjectsAndKeys:
            [NSFont systemFontOfSize:10.0f], NSFontAttributeName, NULL];
        attrs8  = [NSDictionary dictionaryWithObjectsAndKeys:
            [NSFont systemFontOfSize:8.0f],  NSFontAttributeName, NULL];
    }
    attrs = (size <= 8.0f) ? attrs8 : attrs10;
    [str drawAtPoint:p withAttributes:attrs];
}

/* оси, X-tики раз в 8 GB, Y-tики пятью делениями */
static void draw_pane(NSRect pv, double vmax, NSString *ylab)
{
    NSBezierPath *bp = [NSBezierPath bezierPath];
    float x0 = pv.origin.x, y0 = pv.origin.y;
    float x1 = x0 + pv.size.width, y1 = y0 + pv.size.height;
    float gb, t;
    float gbspan = (float)(g_maxoff / 1073741824.0 + 8.9);

    [[NSColor blackColor] set];
    [bp moveToPoint:NSMakePoint(x0, y0)];
    [bp lineToPoint:NSMakePoint(x0, y1)];
    [bp lineToPoint:NSMakePoint(x1, y1)];
    [bp setLineWidth:1.0f];
    [bp stroke];

    for (gb = 0.0; gb <= gbspan; gb += 8.0) {
        float x = x0 + (float)(gb * 1073741824.0 / g_maxoff) *
                  pv.size.width;
        if (x > x1) break;
        {
            NSBezierPath *tick = [NSBezierPath bezierPath];
            char lbl[24];
            [tick moveToPoint:NSMakePoint(x, y0)];
            [tick lineToPoint:NSMakePoint(x, y0 - 4.0f)];
            [tick stroke];
            snprintf(lbl, sizeof lbl, "%g", gb);
            draw_text([NSString stringWithUTF8String:lbl],
                      NSMakePoint(x - 6.0f, y0 - 13.0f), 8.0f);
        }
    }

    for (t = 0.0; t <= 5.0; t += 1.0) {
        float y   = y0 + (float)(t / 5.0) * pv.size.height;
        float val = (float)(vmax * (t / 5.0));
        char  lbl[32];
        NSBezierPath *tick = [NSBezierPath bezierPath];
        [tick moveToPoint:NSMakePoint(x0, y)];
        [tick lineToPoint:NSMakePoint(x0 - 4.0f, y)];
        [tick stroke];
        snprintf(lbl, sizeof lbl, "%g", val);
        draw_text([NSString stringWithUTF8String:lbl],
                  NSMakePoint(x0 - 40.0f, y - 3.0f), 8.0f);
    }
    draw_text(ylab, NSMakePoint(x0 + 6.0f, y1 + 2.0f), 8.0f);
}

@interface GraphView : NSView
- (void)refresh;
@end

@implementation GraphView

- (void)refresh
{
    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)dirty
{
    NSRect        f = [self frame];
    float         padl = 48.0f, padr = 14.0f;
    NSBezierPath *bp;
    double        topmax = 0.0, botmax = 0.0;
    int           i;

    [[NSColor whiteColor] set];
    NSRectFill(f);

    if (g_maxoff <= 0.0) {
        draw_text([NSString stringWithUTF8String:
                    "выберите девайс, duty и Start"],
                  NSMakePoint(60.0f, NSHeight(f) * 0.5f), 10.0f);
        return;
    }

    for (i = 0; i < g_lin_n; i++)
        if (g_lin[i].val > topmax) topmax = g_lin[i].val;
    for (i = 0; i < g_rnd_n; i++)
        if (g_rnd[i].val > botmax) botmax = g_rnd[i].val;
    if (topmax <= 0.0) topmax = 1.0;
    if (botmax <= 0.0) botmax = 1.0;
    topmax *= 1.10;
    botmax *= 1.10;

    /* панель 1: линейная полилиния */
    {
        NSRect pv = NSMakeRect(padl, NSHeight(f) * 0.5f + 24.0f,
                               NSWidth(f) - padl - padr,
                               NSHeight(f) * 0.5f - 52.0f);
        draw_pane(pv, topmax,
                  [NSString stringWithUTF8String:
                      "linear read  MB/s  vs  capacity (GB)"]);

        bp = [[NSBezierPath bezierPath] retain];
        [[NSColor blueColor] set];
        [bp setLineWidth:1.2f];
        for (i = 0; i < g_lin_n; i++) {
            NSPoint p = NSMakePoint(
                pv.origin.x +
                (float)(g_lin[i].off / g_maxoff) * pv.size.width,
                pv.origin.y +
                (float)(g_lin[i].val / topmax) * pv.size.height);
            if (i == 0)
                [bp moveToPoint:p];
            else
                [bp lineToPoint:p];
        }
        [bp stroke];
        [bp release];
    }

    /* панель 2: scatter random */
    {
        NSRect pv = NSMakeRect(padl, 10.0f,
                               NSWidth(f) - padl - padr,
                               NSHeight(f) * 0.5f - 48.0f);
        float x;

        draw_pane(pv, botmax,
                  [NSString stringWithUTF8String:
                      "random read  lag us  vs  capacity (GB)"]);

        [[NSColor blackColor] set];
        for (i = 0; i < g_rnd_n; i++) {
            x = pv.origin.x +
                (float)(g_rnd[i].off / g_maxoff) * pv.size.width;
            NSRectFill(NSMakeRect(x,
                pv.origin.y +
                (float)(g_rnd[i].val / botmax) * pv.size.height,
                2.0f, 2.0f));
        }

        [[NSColor redColor] set];
        for (i = 0; i < g_err_n; i++) {
            x = pv.origin.x +
                (float)(g_err[i].off / g_maxoff) * pv.size.width;
            NSRectFill(NSMakeRect(x - 2.0f, pv.origin.y, 4.0f, 4.0f));
        }
        {
            char lbl[64];
            snprintf(lbl, sizeof lbl, "EIO marks: %d", g_err_n);
            draw_text([NSString stringWithUTF8String:lbl],
                      NSMakePoint(pv.origin.x + 6.0f,
                                  pv.origin.y - 2.0f), 8.0f);
        }
    }

    {
        char lbl[192];
        snprintf(lbl, sizeof lbl,
                 "lin pts %d (%.1f MB/s)   rnd pts %d   errors %d",
                 g_lin_n, g_linspeed, g_rnd_n, g_err_n);
        draw_text([NSString stringWithUTF8String:lbl],
                  NSMakePoint(padl, NSHeight(f) - 12.0f), 10.0f);
    }
}

- (void)dealloc
{
    [super dealloc];
}

@end

/* -------------------------------------------------- список девайсов */

static void devices_scan(void)
{
    CFMutableDictionaryRef match;
    io_iterator_t it;
    io_object_t   svc;

    match = IOServiceMatching("IOMedia");
    if (match == NULL)
        return;
    if (IOServiceGetMatchingServices(kIOMasterPortDefault, match, &it)
        != KERN_SUCCESS)
        return;

    while ((svc = IOIteratorNext(it)) != 0) {
        CFTypeRef v;
        int       whole = 0;
        long long size  = 0;
        char      name[64];

        v = IORegistryEntryCreateCFProperty(svc, CFSTR("Whole"),
                                            kCFAllocatorDefault, 0);
        if (v != NULL) {
            whole = (v == kCFBooleanTrue);
            CFRelease(v);
        }
        if (!whole) {
            IOObjectRelease(svc);
            continue;
        }

        v = IORegistryEntryCreateCFProperty(svc, CFSTR("BSD Name"),
                                            kCFAllocatorDefault, 0);
        if (v == NULL) {
            IOObjectRelease(svc);
            continue;
        }
        if (!CFStringGetCString((CFStringRef)v, name, sizeof name,
                                kCFStringEncodingUTF8)) {
            CFRelease(v);
            IOObjectRelease(svc);
            continue;
        }
        CFRelease(v);

        v = IORegistryEntryCreateCFProperty(svc, CFSTR("Size"),
                                            kCFAllocatorDefault, 0);
        if (v != NULL) {
            CFNumberGetValue((CFNumberRef)v, kCFNumberLongLongType,
                             &size);
            CFRelease(v);
        }
        IOObjectRelease(svc);

        {
            char *np = (char *)malloc(strlen(name) + 1);
            strcpy(np, name);
            g_devnames = (char **)realloc(g_devnames,
                (size_t)(g_ndev + 1) * sizeof(char *));
            g_devsizes = (double *)realloc(g_devsizes,
                (size_t)(g_ndev + 1) * sizeof(double));
            g_devnames[g_ndev] = np;
            g_devsizes[g_ndev] = (double)size;
            g_ndev++;
        }
    }
    IOObjectRelease(it);
}

/* -------------------------------------------------------- авторизация */

static OSStatus run_privileged(const char *tool, char *const args[],
                               FILE **outp)
{
    AuthorizationRef auth = NULL;
    OSStatus         rc;

    rc = AuthorizationCreate(NULL, NULL, kAuthorizationFlagDefaults,
                             &auth);
    if (rc != errAuthorizationSuccess)
        return rc;
    /* Tiger header: ExecuteWithPrivileges options "reserved, MUST be
     * zero" - любые биты дают -60011 invalid flags (ловили).
     * Password-диалог SecurityAgent оно показывает само. */
    rc = AuthorizationExecuteWithPrivileges(auth, tool,
        kAuthorizationFlagDefaults,
        args, outp);
    AuthorizationFree(auth, kAuthorizationFlagDefaults);
    return rc;
}

/* ------------------------------------------------------------ каркас */

@interface Controller : NSObject
- (void)start:(id)sender;
- (void)stop:(id)sender;
- (void)dutyChanged:(id)sender;
- (void)secsChanged:(id)sender;
- (void)tick:(NSTimer *)timer;
@end

@implementation Controller

- (void)start:(id)sender
{
    NSString *engine;
    char     devpath[80];
    char    *argv[16];
    OSStatus rc;
    FILE    *fp = NULL;
    char     status[256];

    (void)sender;
    if (g_running)
        return;

    g_curdev = [g_popup indexOfSelectedItem];
    if (g_curdev < 0 || g_curdev >= g_ndev) {
        [g_status setStringValue:
            [NSString stringWithUTF8String:"девайс не выбран"]];
        return;
    }
    g_devbytes = g_devsizes[g_curdev];

    engine = [[NSBundle mainBundle] pathForResource:
                  [NSString stringWithUTF8String:"dbsweep"] ofType:nil];
    if (engine == nil) {
        [g_status setStringValue:
            [NSString stringWithUTF8String:"движок dbsweep не найден "
                                           "в бандле"]];
        return;
    }

    data_reset();

    snprintf(devpath, sizeof devpath, "/dev/%s", g_devnames[g_curdev]);

    {
        static char dDuty[16], dT[16];
        snprintf(dDuty, sizeof dDuty, "%d", g_duty);
        snprintf(dT,    sizeof dT,    "%d", g_rndsecs);

        argv[0]  = (char *)"-d";  argv[1]  = devpath;
        argv[2]  = (char *)"-D";  argv[3]  = dDuty;
        argv[4]  = (char *)"-T";  argv[5]  = dT;
        argv[6]  = (char *)"-c";  argv[7]  = (char *)"32M";
        argv[8]  = (char *)"-b";  argv[9]  = (char *)"1M";
        argv[10] = (char *)"-B";  argv[11] = (char *)"4K";
        argv[12] = (char *)"-o";  argv[13] = (char *)CSV_PATH;
        argv[14] = NULL;
    }

    rc = run_privileged([engine fileSystemRepresentation], argv, &fp);
    if (fp != NULL)
        fclose(fp);              /* stdout движка не слушаем - tail */

    if (rc != errAuthorizationSuccess) {
        snprintf(status, sizeof status,
                 "авторизация не удалась (OSStatus %d)", (int)rc);
        [g_status setStringValue:[NSString stringWithUTF8String:status]];
        return;
    }

    g_running = 1;
    [g_startbtn setEnabled:NO];
    {
        char head[128];
        double eta_min = g_devbytes * ((double)g_duty / 100.0) /
                         1048576.0 / 30.0 / 60.0;
        snprintf(head, sizeof head,
                 "ход: /dev/%s  duty %d%%  random %d s  ETA ~%.0f мин",
                 g_devnames[g_curdev], g_duty, g_rndsecs, eta_min);
        [g_status setStringValue:[NSString stringWithUTF8String:head]];
    }
}

- (void)stop:(id)sender
{
    char *argv[3];

    (void)sender;
    if (!g_running)
        return;
    argv[0] = (char *)"dbsweep";
    argv[1] = NULL;
    run_privileged("/usr/bin/killall", argv, NULL);
    [g_status setStringValue:
        [NSString stringWithUTF8String:"стоп отправлен..."]];
}

- (void)dutyChanged:(id)sender
{
    char lbl[64];

    g_duty = [sender intValue];
    snprintf(lbl, sizeof lbl, "duty %d%%", g_duty);
    [g_dutylab setStringValue:[NSString stringWithUTF8String:lbl]];

    if (g_curdev >= 0 && g_curdev < g_ndev && g_linspeed == 0.0) {
        double eta_min = g_devsizes[g_curdev] *
                         ((double)g_duty / 100.0) /
                         1048576.0 / 30.0 / 60.0;
        snprintf(lbl, sizeof lbl,
                 "duty %d%%  ETA ~%.0f мин (при 30 MB/s)", g_duty,
                 eta_min);
        [g_status setStringValue:[NSString stringWithUTF8String:lbl]];
    }
}

- (void)secsChanged:(id)sender
{
    g_rndsecs = [sender intValue];
}

- (void)tick:(NSTimer *)timer
{
    (void)timer;
    if (!g_running)
        return;

    if (tail_tick()) {
        g_running = 0;
        [g_startbtn setEnabled:YES];
        [g_status setStringValue:
            [NSString stringWithUTF8String:"ход завершён"]];
    } else if (g_lin_n > 0) {
        double el = (now_us() - g_startt) / 1e6;
        if (el > 0.0)
            g_linspeed = (double)g_linbytes / el / 1048576.0;
        if (g_linspeed > 0.0 && g_devbytes > 0.0) {
            char eta[128];
            double remain = g_devbytes * ((double)g_duty / 100.0) -
                            (double)g_linbytes;
            if (remain < 0.0) remain = 0.0;
            snprintf(eta, sizeof eta,
                     "ход: eta %.0f мин (%.1f MB/s)", remain /
                     (g_linspeed * 1048576.0) / 60.0, g_linspeed);
            [g_status setStringValue:
                [NSString stringWithUTF8String:eta]];
        }
    }
    [g_view refresh];
}

@end

int main(void)
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    NSWindow          *win;
    NSRect             rc = NSMakeRect(0.0f, 0.0f, 780.0f, 640.0f);
    NSView            *root;
    Controller        *ctl;
    NSMenu            *menu, *submenu;
    NSMenuItem        *mi;
    NSRect             bar, pv;
    NSButton          *b;
    NSTextField       *tf;
    NSSlider          *sl;
    NSTimer           *timer;

    [NSApplication sharedApplication];
    clock_init();
    devices_scan();

    ctl = [[Controller alloc] init];

    menu = [[NSMenu alloc] initWithTitle:
                [NSString stringWithUTF8String:"main"]];
    {
        NSMenuItem *appmi = [menu addItemWithTitle:
            [NSString stringWithUTF8String:"DiskGraph"]
            action:NULL keyEquivalent:
            [NSString stringWithUTF8String:""]];
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
    [win setTitle:[NSString stringWithUTF8String:"diskgraph - dbsweep UI"]];
    root = [[[NSView alloc] initWithFrame:rc] autorelease];
    [win setContentView:root];

    bar = NSMakeRect(0.0f, NSHeight(rc) - 66.0f, NSWidth(rc), 64.0f);
    /* панель графиков до статус-строки: статус живёт в нижней строке
     * бара (556..574), панель до неё - owerлапа с белой заливкой нет */
    pv  = NSMakeRect(0.0f, 0.0f, NSWidth(rc), NSHeight(rc) - 92.0f);

    /* popup девайсов */
    g_popup = [[NSPopUpButton alloc] initWithFrame:
        NSMakeRect(12.0f, NSHeight(rc) - 52.0f, 190.0f, 26.0f)];
    if (g_ndev == 0) {
        [g_popup addItemWithTitle:
            [NSString stringWithUTF8String:"(девайсы не найдены)"]];
    } else {
        int i;
        for (i = 0; i < g_ndev; i++) {
            char itmlbl[96];
            snprintf(itmlbl, sizeof itmlbl, "%s — %.1f GB",
                     g_devnames[i], g_devsizes[i] / 1073741824.0);
            [g_popup addItemWithTitle:
                [NSString stringWithUTF8String:itmlbl]];
        }
    }
    [root addSubview:g_popup];

    /* duty label + slider */
    g_dutylab = [[NSTextField alloc] initWithFrame:
        NSMakeRect(214.0f, NSHeight(rc) - 48.0f, 96.0f, 20.0f)];
    [g_dutylab setStringValue:
        [NSString stringWithUTF8String:"duty 33%"]];
    [g_dutylab setBezeled:NO];
    [g_dutylab setEditable:NO];
    [g_dutylab setDrawsBackground:NO];
    [root addSubview:g_dutylab];

    sl = [[NSSlider alloc] initWithFrame:
        NSMakeRect(310.0f, NSHeight(rc) - 50.0f, 170.0f, 22.0f)];
    [sl setMinValue:10.0f];
    [sl setMaxValue:100.0f];
    [sl setIntValue:33];
    [sl setTarget:ctl];
    [sl setAction:@selector(dutyChanged:)];
    [root addSubview:sl];
    [sl release];

    /* random secs */
    tf = [[NSTextField alloc] initWithFrame:
        NSMakeRect(492.0f, NSHeight(rc) - 50.0f, 52.0f, 22.0f)];
    [tf setStringValue:[NSString stringWithUTF8String:"30"]];
    [tf setTarget:ctl];
    [tf setAction:@selector(secsChanged:)];
    [root addSubview:tf];
    [tf release];

    {
        NSTextField *lbl = [[NSTextField alloc] initWithFrame:
            NSMakeRect(492.0f, NSHeight(rc) - 66.0f, 70.0f, 16.0f)];
        [lbl setStringValue:
            [NSString stringWithUTF8String:"rnd sec"]];
        [lbl setBezeled:NO];
        [lbl setEditable:NO];
        [lbl setDrawsBackground:NO];
        [root addSubview:lbl];
        [lbl release];
    }

    /* Start / Stop */
    g_startbtn = [[NSButton alloc] initWithFrame:
        NSMakeRect(560.0f, NSHeight(rc) - 54.0f, 92.0f, 30.0f)];
    [g_startbtn setBezelStyle:NSRoundedBezelStyle];
    [g_startbtn setTitle:[NSString stringWithUTF8String:"Старт"]];
    [g_startbtn setTarget:ctl];
    [g_startbtn setAction:@selector(start:)];
    [root addSubview:g_startbtn];
    [g_startbtn release];

    b = [[NSButton alloc] initWithFrame:
        NSMakeRect(662.0f, NSHeight(rc) - 54.0f, 92.0f, 30.0f)];
    [b setBezelStyle:NSRoundedBezelStyle];
    [b setTitle:[NSString stringWithUTF8String:"Стоп"]];
    [b setTarget:ctl];
    [b setAction:@selector(stop:)];
    [root addSubview:b];
    [b release];

    /* status line */
    g_status = [[NSTextField alloc] initWithFrame:
        NSMakeRect(12.0f, NSHeight(rc) - 84.0f, 500.0f, 18.0f)];
    [g_status setStringValue:
        [NSString stringWithUTF8String:"готов"]];
    [g_status setBezeled:NO];
    [g_status setEditable:NO];
    [g_status setDrawsBackground:NO];
    [root addSubview:g_status];
    [g_status release];

    /* график */
    g_view = [[GraphView alloc] initWithFrame:pv];
    [root addSubview:g_view];

    [win center];
    [win makeKeyAndOrderFront:nil];

    timer = [NSTimer timerWithTimeInterval:0.7
                                    target:ctl
                                   selector:@selector(tick:)
                                   userInfo:nil
                                    repeats:YES];
    [[NSRunLoop currentRunLoop] addTimer:timer
                    forMode:NSDefaultRunLoopMode];

    [NSApp run];

    [g_view release];
    [g_popup release];
    [g_dutylab release];
    [ctl release];
    [win release];
    [pool release];
    return 0;
}
