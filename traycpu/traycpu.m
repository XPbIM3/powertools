/*
 * traycpu.m - CPU usage in the menu bar (tray) for Mac OS X 10.3.9
 * (panther), cross-built on an intel Leopard host (MacBookPro3,1).
 *
 * NO main window. One NSStatusItem shows system-wide CPU busy as
 * " CPU: NN% ", averaged over the last second:
 *
 * NOTE width: the Panther status item does NOT auto-resize on
 * setTitle, so a clipped " CPU: 1 " is what a too-short length
 * gives. Fixed generous length 86 pt fits " CPU: 100% " with a
 * right margin (76 and 86 pt both clipped on the '%' at 90-100% busy -
 * measured on the live machine); leading and trailing spaces damp
 * the font-width jitter. (If jitter still bothers: next iteration
 * = custom NSView in the item drawing a monospaced face - Monaco
 * exists on Panther.)
 * Info.plist LSUIElement = 1: agent app, no Dock icon.
 *
 *   - We do NOT sample load. host_statistics(HOST_CPU_LOAD_INFO)
 *     returns the kernel's cumulative tick counters (user/system/
 *     nice/idle, summed since boot, Mach API alive since 10.0).
 *     The delta between two snapshots one second apart IS the
 *     finished average for that second - the kernel integrated
 *     load between visits with machine precision.
 *   - busy% = 100 * (1 - idle_delta / total_delta)
 *     ratio of tick deltas, so tick units do not matter; unsigned
 *     delta survives uint32 wrap. Ticks are scheduler-quantum
 *     granular (~10 ms) giving ~1% resolution per second window.
 *   - If total_delta == 0 (asleep/quiet anomaly) keep previous value.
 *
 * Click on the status item opens a menu with ONE item: "Выход".
 * Choosing it terminates the app ([NSApp terminate:] via our target).
 *
 * The 1 s refresh is an NSTimer on the default run loop; while a
 * menu is open (event tracking mode) it pauses - cosmetic only.
 *
 * Font note: the Panther status item has visible width jitter with
 * proportional system font. Padded " CPU: NN% " + fixed 96 pt helps;
 * if jitter still bothers: custom NSView in the item drawing a
 * monospaced face (Monaco exists on Panther).
 *
 * ObjC kept to legacy objc1 constructs (Panther runtime):
 * one NSObject subclass, plain methods, no properties/blocks.
 */
#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import <mach/mach.h>

static host_cpu_load_info_data_t g_prev;
static int                       g_have_prev = 0;
static NSStatusItem             *g_item = NULL;

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
    host_cpu_load_info_data_t now;
    natural_t                 cnt = HOST_CPU_LOAD_INFO_COUNT;
    unsigned int             *a, *b;
    unsigned int              i, d[CPU_STATE_MAX];
    unsigned long long        total = 0, idle = 0;
    int                       pct = 0;

    if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO,
                        (host_info_t)&now, &cnt) != KERN_SUCCESS)
        return;                                  /* keep previous value */

    if (g_have_prev) {
        a = &now.cpu_ticks[0];
        b = &g_prev.cpu_ticks[0];
        for (i = 0; i < CPU_STATE_MAX; i++) {
            d[i] = (unsigned int)(a[i] - b[i]);
            total += d[i];
        }
        /* tick order is user, system, idle, nice */
        idle = d[CPU_STATE_IDLE];

        if (total != 0) {
            pct = (int)(100.0 * (1.0 - (double)idle / (double)total) + 0.5);
            if (pct < 0)   pct = 0;
            if (pct > 100) pct = 100;
        } /* else: machine slept or no scheduling happened - keep previous */
    }
    else
        g_have_prev = 1;

    g_prev = now;                                /* struct copy */

    if (g_item != nil && total != 0)
        [g_item setTitle:[NSString stringWithFormat:@" CPU: %d%% ", pct]];
}

@end

int main(void)
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    [NSApplication sharedApplication];

    ProbeTarget    *t = [[ProbeTarget alloc] init];
    NSStatusBar    *sb = [NSStatusBar systemStatusBar];
    NSMenu         *menu;
    NSMenuItem     *mi;
    NSTimer        *timer;

    g_item = [sb statusItemWithLength:96.0];
    if (g_item != nil) {
        [g_item setTitle:[NSString stringWithUTF8String:" CPU: --% "]];

        menu = [[NSMenu alloc] initWithTitle:@""];
        mi   = [menu addItemWithTitle:[NSString stringWithUTF8String:"Выход"]
                               action:@selector(quit:)
                        keyEquivalent:@""];
        [mi setTarget:t];
        [g_item setMenu:menu];
        [menu release];
    }

    /* snapshot base ticks now; first real number appears after 1 s */
    {
        natural_t cnt = HOST_CPU_LOAD_INFO_COUNT;
        host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO,
                        (host_info_t)&g_prev, &cnt);
        g_have_prev = 0;
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
