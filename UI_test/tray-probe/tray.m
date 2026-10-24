/*
 * tray.m - trayprobe: menu-bar-only carbon/cocoa probe for Mac OS X 10.3.9
 * (panther), cross-built on an intel Leopard host (MacBookPro3,1).
 *
 * NO main window. Two status items on the right of the menu bar:
 *   "20с"    (fictional temperature)
 *   "2000об" (fictional fan rpm)
 * Click on either item quits the app (setTarget/setAction, no menus).
 *
 * Panther-era API only:
 *   NSStatusBar/NSStatusItem        10.0+
 *   setTitle:/setTarget:/setAction: 10.0+
 *   NSString stringWithUTF8String   10.0+
 * ObjC kept to legacy objc1 constructs: one NSObject subclass, simple
 * methods - no properties, no blocks (Panther runtime).
 */
#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>

@interface ProbeTarget : NSObject
- (void)quit:(id)sender;
@end

@implementation ProbeTarget

- (void)quit:(id)sender
{
    [NSApp terminate:sender];
}

@end

int main(void)
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    [NSApplication sharedApplication];

    ProbeTarget    *t = [[ProbeTarget alloc] init];
    NSStatusBar    *sb = [NSStatusBar systemStatusBar];
    NSStatusItem   *i1, *i2;

    i1 = [sb statusItemWithLength:34.0];
    if (i1 != nil) {
        [i1 setTitle:[NSString stringWithUTF8String:"20с"]];
        [i1 setTarget:t];
        [i1 setAction:@selector(quit:)];
    }

    i2 = [sb statusItemWithLength:56.0];
    if (i2 != nil) {
        [i2 setTitle:[NSString stringWithUTF8String:"2000об"]];
        [i2 setTarget:t];
        [i2 setAction:@selector(quit:)];
    }

    [NSApp run];

    [t release];
    [pool release];
    return 0;
}
