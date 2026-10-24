/*
 * uitest.m - universal tray skeleton SMOKE: no hardware layer at
 * all.  One NSStatusItem showing a constant "[test]" - click opens
 * the menu with the single "Выход" item, as in every park tray.
 * Purpose of the build: verify the UI layer ONLY, on multiple
 * machines/OS X versions - currently (25.10.2026) powermac-g4-400
 * (Panther 10.3.9, PPC) and intel-leopard (Leopard 10.5, i386),
 * from THE SAME fat binary.
 *
 * The display is a CONSTANT string, so neither the anti-dance
 * formatting nor any counter plumbing matters here.  Item length
 * uses the park lesson (measure with the default system font +
 * margin; the small-font measure under-eats on both panther and
 * leopard).
 *
 * Flag pairs, both built from this one source on the 10.5 intel
 * host (macbookpro3,1), then merged into a fat binary:
 *   PPC slice (runs 10.2-10.4 G3/G4):
 *     gcc -arch ppc -mcpu=G3 -Wl,-force_cpusubtype_ALL \
 *         -mmacosx-version-min=10.3 -std=gnu99 -O1 -Wall \
 *         -fobjc-abi-version=1 -x objective-c uitest.m \
 *         -o uitest-ppc -framework Foundation -framework AppKit
 *   i386 slice (runs 10.5 intel):
 *     gcc -arch i386 -mmacosx-version-min=10.5 -std=gnu99 -O1 -Wall \
 *         -x objective-c uitest.m -o uitest-i386 \
 *         -framework Foundation -framework AppKit
 *   lipo -create -output uitest uitest-ppc uitest-i386
 *
 * ObjC kept to legacy objc1 constructs (park convention, works on
 * every OS X of the 10.2-10.5 range).
 */
#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>

static NSStatusItem *g_item = NULL;

@interface ProbeTarget : NSObject
- (void)quit:(id)sender;
@end

@implementation ProbeTarget

- (void)quit:(id)sender
{
    [NSApp terminate:sender];
}

@end

/* Item length: constant string, measured with the default system
 * font (13 pt face, wider than any bar render on panther/leopard
 * both) + the park margin. */
static float gauge_length(void)
{
    NSString *widest = [NSString stringWithUTF8String:" [test] "];
    NSDictionary *attrs = [NSDictionary dictionaryWithObject:
        [NSFont systemFontOfSize:0]
                         forKey:NSFontAttributeName];
    NSSize sz = [widest sizeWithAttributes:attrs];
    float  len = (float)ceilf(sz.width) + 50.0f;

    if (len < 90.0f) len = 90.0f;                /* sanity floor */
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

    g_item = [sb statusItemWithLength:gauge_length()];
    if (g_item != nil) {
        [g_item setTitle:[NSString stringWithUTF8String:" [test] "]];

        menu = [[NSMenu alloc] initWithTitle:@""];
        mi   = [menu addItemWithTitle:[NSString stringWithUTF8String:"Выход"]
                               action:@selector(quit:)
                        keyEquivalent:@""];
        [mi setTarget:t];
        [g_item setMenu:menu];
        [menu release];
    }

    [NSApp run];

    [sb removeStatusItem:g_item];
    [pool release];
    return 0;
}
