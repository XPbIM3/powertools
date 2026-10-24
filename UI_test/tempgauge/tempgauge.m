/*
 * tempgauge.m - CPU temperature gauge in the menu bar (tray) for
 * Mac OS X 10.5 (leopard), built NATIVELY on the very host it
 * monitors (intel-leopard, MacBookPro3,1).  Fourth member of the
 * park tray-skeleton family (carbon-probe -> traycpu -> diskgauge
 * -> tempgauge): same skeleton, new counter source.
 *
 * NO main window, NO Dock icon (Info.plist LSUIElement = 1).
 * One NSStatusItem shows BOTH available CPU thermal keys:
 *
 *     " CPU: D078 P065 C "
 *
 *   - Counter source: the AppleSMC user client.  Decode per the
 *     devnull/smcFanControl reference: bytes[0..1] big-endian
 *     sp78 -> (b0*256+b1) >> 2 / 64.0.
 *   - Per-core question, live recon (tmpprobe, this machine):
 *     MacBookPro3,1 SMC exposes exactly ONE core-diode key
 *     (TC0D) - no TC1D/TC2D/TCx pair in the key space, the chipset
 *     does not separate cores.  The second honest CPU thermal is
 *     the package proximity key TC0P.  So: D = MAX over diode
 *     candidates (covers multi-diode Macs of the park too),
 *     P = MAX over proximity candidates.
 *   - ANTI-DANCE (same lesson as diskgauge): whole degrees C in
 *     fixed 3-slot zero-padded fields, cap 199.  No arrows, no
 *     decimals, no mono font - constant-width formatting carries
 *     the fix.
 *   - Read failure or implausible value (<=0 C, >=200 C) keeps
 *     the previous title.
 *
 * Click opens a menu with ONE item: "Выход" -> [NSApp terminate:].
 * 1 s refresh via NSTimer in NSDefaultRunLoopMode.
 *
 * ObjC kept to legacy objc1-construct style (park convention).
 */
#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import <IOKit/IOKitLib.h>

typedef struct {
    char major;
    char minor;
    char build;
    char reserved[1];
    unsigned short release;
} SMC_vers_t;

typedef struct {
    unsigned short version;
    unsigned short length;
    unsigned int   cpuPLimit;
    unsigned int   gpuPLimit;
    unsigned int   memPLimit;
} SMC_plimit_t;

typedef struct {
    unsigned int  dataSize;
    unsigned int  dataType;
    char          dataAttributes;
} SMC_keyinfo_t;

typedef unsigned char SMCBytes_t[32];

typedef struct {
    unsigned int   key;
    SMC_vers_t     vers;
    SMC_plimit_t   pLimitData;
    SMC_keyinfo_t  keyInfo;
    char           result;
    char           status;
    char           data8;
    unsigned int   data32;
    SMCBytes_t     bytes;
} SMCKeyData_t;

/* de-facto AppleSMC user client command set (devnull/smcFanControl) */
#define KERNCMD_SMC          2
#define SMC_CMD_READ_BYTES   5
#define SMC_CMD_READ_KEYINFO 9

static io_connect_t  g_conn    = 0;
static int           g_have_v  = 0;   /* have last plausible values */
static int           g_d_val   = 0;   /* diode max */
static int           g_p_val   = 0;   /* proximity max */
static NSStatusItem *g_item    = NULL;

static unsigned int key_code(const char *s)
{
    unsigned int v = 0;
    int i;
    for (i = 0; i < 4; i++)
        v = (v << 8) | (unsigned char)(s[i] & 0xff);
    return v;
}

static kern_return_t smc_call(int cmd, SMCKeyData_t *in, SMCKeyData_t *out)
{
    size_t in_size  = sizeof(SMCKeyData_t);
    size_t out_size = sizeof(SMCKeyData_t);
    return IOConnectCallStructMethod(g_conn, cmd, in, in_size,
                                     out, &out_size);
}

static int smc_open(void)
{
    CFMutableDictionaryRef match;
    io_iterator_t          it = 0;
    io_object_t            dev;
    kern_return_t          kr;

    match = IOServiceMatching("AppleSMC");
    if (match == NULL)
        return 1;
    kr = IOServiceGetMatchingServices(kIOMasterPortDefault, match, &it);
    if (kr != KERN_SUCCESS)
        return 1;
    dev = IOIteratorNext(it);
    IOObjectRelease(it);
    if (dev == 0)
        return 1;
    kr = IOServiceOpen(dev, mach_task_self(), 0, &g_conn);
    IOObjectRelease(dev);
    return (kr == KERN_SUCCESS) ? 0 : 1;
}

/* one SMC key read; type four-char in tstr; decode sp78 only */
static kern_return_t read_key(const char *keyc, int *out_deg)
{
    SMCKeyData_t  in, out;
    kern_return_t kr;
    unsigned int  size;
    char          t[5];

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    in.key    = key_code(keyc);
    in.data8  = SMC_CMD_READ_KEYINFO;
    kr = smc_call(KERNCMD_SMC, &in, &out);
    if (kr != KERN_SUCCESS)
        return kr;
    size = out.keyInfo.dataSize;
    t[0] = (char)((out.keyInfo.dataType >> 24) & 0xff);
    t[1] = (char)((out.keyInfo.dataType >> 16) & 0xff);
    t[2] = (char)((out.keyInfo.dataType >> 8) & 0xff);
    t[3] = (char)(out.keyInfo.dataType & 0xff);
    t[4] = 0;
    if (size != 2 || strncmp(t, "sp78", 4) != 0)
        return kIOReturnUnsupported;             /* not a temp we read */

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    in.key             = key_code(keyc);
    in.keyInfo.dataSize = 2;
    in.data8           = SMC_CMD_READ_BYTES;
    kr = smc_call(KERNCMD_SMC, &in, &out);
    if (kr != KERN_SUCCESS)
        return kr;

    /* big-endian sp78 -> degrees, per the classic decode */
    *out_deg = (int)(((out.bytes[0] * 256 + out.bytes[1]) >> 2) / 64.0 + 0.5);
    return KERN_SUCCESS;
}

/*
 * Candidate walk with a MAX: the gauge is a recipe, not a hardcode -
 * bigger Macs carry knobs (TC1D/TC2D diodes, TCAH/TCBH proximities)
 * this MBP3,1 does not; ask all that the machine may have.
 */
static const char *g_d_cand[] = { "TC0D", "TC1D", "TC2D", "TC3D",
                                  "TCAH", "TCBH", NULL };
static const char *g_p_cand[] = { "TC0P", "TC1P", "TCAH", "TCBH", NULL };

static int read_group(const char *cands[])       /* MAX or -1 */
{
    int deg = 0, best = -1, i;
    for (i = 0; cands[i] != NULL; i++) {
        if (read_key(cands[i], &deg) == KERN_SUCCESS && deg > best)
            best = deg;
    }
    return best;
}

/* whole degrees in a fixed 3-slot zero-padded field, cap 199 */
static int temp3(int deg)
{
    if (deg < 0)   deg = 0;
    if (deg > 199) deg = 199;
    return deg;
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
    int d = read_group(g_d_cand);
    int p = read_group(g_p_cand);

    if (d > 0 && d < 200) {
        g_d_val = temp3(d);
        g_have_v = 1;
    }
    if (p > 0 && p < 200) {
        g_p_val = temp3(p);
        g_have_v = 1;
    }                                       /* else keep previous */

    if (g_item != nil && g_have_v)
        [g_item setTitle:[NSString stringWithFormat:
             @" CPU: D%03d P%03d C ", g_d_val, g_p_val]];
}

@end

/* Fixed item length: measured with THE DEFAULT SYSTEM FONT
 * (systemFontOfSize:0 = 13 pt face) + 50 pt margin.  Lesson of the
 * live runs: measuring the small font under-or-eats on BOTH panther
 * and leopard (+50 was still clipped) - the bar renders with a
 * roomier face than smallSystemFontSize.  Measuring a face that is
 * BIGGER than any render makes the margin irrelevant: guaranteed
 * non-clip, and constant width (no jitter) by the format anyway. */
static float gauge_length(void)
{
    NSString *widest = [NSString stringWithUTF8String:" CPU: D999 P999 C "];
    NSDictionary *attrs = [NSDictionary dictionaryWithObject:
        [NSFont systemFontOfSize:0]                  /* default face */
                         forKey:NSFontAttributeName];
    NSSize sz = [widest sizeWithAttributes:attrs];
    float  len = (float)ceilf(sz.width) + 50.0f;

    if (len < 180.0f) len = 180.0f;              /* sanity floor */
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

    if (smc_open() != 0)
        return 1;                                /* no SMC, no gauge */

    g_item = [sb statusItemWithLength:gauge_length()];
    if (g_item != nil) {
        [g_item setTitle:[NSString stringWithUTF8String:" CPU: D--- P--- C "]];

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
