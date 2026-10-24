/*
 * diskgauge.m - live disk I/O gauge in the menu bar (tray) for
 * Mac OS X 10.3.9 (panther), cross-built on an intel Leopard host
 * (MacBookPro3,1).  Sibling of traycpu.m - same skeleton, different
 * counter source.
 *
 * NO main window, NO Dock icon (Info.plist LSUIElement = 1).
 * One NSStatusItem shows system-wide instantaneous disk throughput:
 *
 *     " HDD: W: 050 R: 003 MB/s "
 *
 *   - We do NOT sample throughput. The IOKit block storage driver
 *     (IOBlockStorageDriver) publishes cumulative byte counters in
 *     its registry entry property "Statistics" - keys
 *     "Bytes (Read)" / "Bytes (Write)", summed since boot.  The
 *     delta between two snapshots one second apart IS the
 *     instantaneous rate the whole system read/wrote during that
 *     second - the kernel integrated the I/O for us with machine
 *     precision.  Reading the registry costs zero disk I/O.
 *   - ANTI-DANCE (the design core, by user's spec): no trend
 *     arrows, no mono font, NO decimals. The rate is rendered as
 *     an integer MB/s in a FIXED 3-slot field with leading zeros
 *     ("%03d"), capped at 999:  "007", "050", "119", "999".  Three
 *     slots always - the string width cannot dance.  The panther
 *     status item does NOT auto-resize on setTitle, so the item
 *     length stays fixed anyway; it is derived from the WIDEST
 *     plausible string (999 field) measured with the small system
 *     font + margin.  Live-Panther margins history: +10 pt ate
 *     pixels at the right edge (bar renders heavier than the
 *     measurement face); +30 pt still ate the right edge; +50 pt deployed.
 *   - If the driver query fails or a counter moved backwards,
 *     keep the previous title.  First second after launch shows
 *     the placeholder " W: -- R: -- " (needs one base snapshot).
 *
 * Launch/deploy: the tray-probe protocol (bundle + open over ssh).
 *
 * ObjC kept to legacy objc1 constructs (Panther runtime).
 */
#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import <IOKit/IOKitLib.h>       /* leopard host: no umbrella IOKit.h */

static long long    g_prev_rd_bytes = 0;     /* cumulative snapshots */
static long long    g_prev_wr_bytes = 0;
static int          g_have_base     = 0;
static NSStatusItem *g_item         = NULL;

/*
 * Sum the cumulative read/written byte counters over all block
 * storage driver instances (here: one IDE drive). Returns 1 if at
 * least one driver answered, 0 if the IOKit walk gave nothing.
 * NOTE live Panther 10.3.9 finding (ioreg): the write key is
 * asymmetric - "Bytes (Write)", no trailing 'n'; read key is
 * "Bytes (Read)".  We try both spellings, whichever the driver
 * carries.
 */
static int disk_bytes(long long *out_r, long long *out_w)
{
    CFMutableDictionaryRef match;
    io_iterator_t          it;
    io_object_t            svc;
    long long              r = 0, w = 0;
    int                    ok = 0;

    match = IOServiceMatching("IOBlockStorageDriver");
    if (match == NULL)
        return 0;
    if (IOServiceGetMatchingServices(kIOMasterPortDefault, match,
                                     &it) != KERN_SUCCESS)
        return 0;                                /* match dict consumed */

    while ((svc = IOIteratorNext(it)) != 0) {
        CFDictionaryRef st;
        CFNumberRef     n;
        long long       v = 0;

        st = (CFDictionaryRef)IORegistryEntryCreateCFProperty(
                 svc, CFSTR("Statistics"), kCFAllocatorDefault, 0);
        if (st != NULL) {
            n = (CFNumberRef)CFDictionaryGetValue(st, CFSTR("Bytes (Read)"));
            if (n != NULL && CFNumberGetValue(n, kCFNumberLongLongType, &v))
                r += v;
            /* Panther key is "Bytes (Write)": modern is "Bytes (Written)" */
            n = (CFNumberRef)CFDictionaryGetValue(st, CFSTR("Bytes (Write)"));
            if (n == NULL)
                n = (CFNumberRef)CFDictionaryGetValue(
                        st, CFSTR("Bytes (Written)"));
            if (n != NULL && CFNumberGetValue(n, kCFNumberLongLongType, &v))
                w += v;
            CFRelease(st);
            ok = 1;
        }
        IOObjectRelease(svc);
    }
    IOObjectRelease(it);

    *out_r = r;
    *out_w = w;
    return ok;
}

/* rate in whole MB/s, fixed 3-slot zero-padded field, cap 999 */
static int rate3(double mb)
{
    int v = (int)(mb + 0.5);
    if (v < 0)     v = 0;
    if (v > 999)   v = 999;
    return v;
}

@interface ProbeTarget : NSObject
- (void)quit:(id)sender;
- (void)tick:(NSTimer *)timer;
@end

@implementation ProbeTarget

- (void)quit:(id)sender
{
    [NSApp terminate:sender];
}

- (void)tick:(NSTimer *)timer
{
    long long cur_r = 0, cur_w = 0;
    double    dr, dw, rr, wr;

    if (!disk_bytes(&cur_r, &cur_w))
        return;                                  /* keep previous title */

    if (!g_have_base) {
        g_prev_rd_bytes = cur_r;
        g_prev_wr_bytes = cur_w;
        g_have_base     = 1;
        return;                                  /* "--" for first sec */
    }

    dr = (double)(cur_r - g_prev_rd_bytes);      /* bytes in this sec */
    dw = (double)(cur_w - g_prev_wr_bytes);
    if (dr < 0.0) dr = 0.0;                      /* counter moved back */
    if (dw < 0.0) dw = 0.0;
    rr = dr / (1024.0 * 1024.0);
    wr = dw / (1024.0 * 1024.0);

    g_prev_rd_bytes = cur_r;
    g_prev_wr_bytes = cur_w;

    if (g_item != nil)
        [g_item setTitle:[NSString stringWithFormat:
             @" HDD: W: %03d R: %03d MB/s ", rate3(wr), rate3(rr)]];
}

@end

/* Fixed item length from the widest plausible string (999-field),
 * measured once with the small system font, +30 pt margin (the
 * live-machine margin that stopped the pixel eating). */
static float gauge_length(void)
{
    NSString *widest = [NSString stringWithUTF8String:
        " HDD: W: 999 R: 999 MB/s "];
    NSDictionary *attrs = [NSDictionary dictionaryWithObject:
        [NSFont systemFontOfSize:[NSFont smallSystemFontSize]]
                         forKey:NSFontAttributeName];
    NSSize sz = [widest sizeWithAttributes:attrs];
    float  len = (float)ceilf(sz.width) + 50.0f;

    if (len < 170.0f) len = 170.0f;              /* sanity floor */
    if (len > 380.0f) len = 380.0f;              /* sanity ceiling */
    return len;
}

int main(void)
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    [NSApplication sharedApplication];

    ProbeTarget    *t = [[ProbeTarget alloc] init];
    NSStatusBar    *sb = [NSStatusBar systemStatusBar];
    NSMenu         *menu;
    NSMenuItem     *mi;
    NSTimer        *timer;

    g_item = [sb statusItemWithLength:gauge_length()];
    if (g_item != nil) {
        [g_item setTitle:[NSString stringWithUTF8String:
            " HDD: W: -- R: -- MB/s "]];

        menu = [[NSMenu alloc] initWithTitle:@""];
        mi   = [menu addItemWithTitle:[NSString stringWithUTF8String:"Выход"]
                               action:@selector(quit:)
                        keyEquivalent:@""];
        [mi setTarget:t];
        [g_item setMenu:menu];
        [menu release];
    }

    timer = [NSTimer timerWithTimeInterval:1.0
                                    target:t
                                  selector:@selector(tick:)
                                  userInfo:nil
                                   repeats:YES];
    [[NSRunLoop currentRunLoop] addTimer:timer forMode:NSDefaultRunLoopMode];
    /* NSTimer retains its target (t) while scheduled */

    [NSApp run];

    [sb removeStatusItem:g_item];
    [pool release];
    return 0;
}
