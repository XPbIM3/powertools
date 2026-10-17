/*
 * batinfo.c - battery status monitor for PowerPC Macs (Mac OS X 10.4/10.5)
 *
 * Pure public IOKit registry access, no private APIs.
 *
 * Data paths (auto-detected, both are public registry conventions):
 *
 *   PMU-era (G3/G4 laptops): entries with "IOBatteryInfo" (AppleMacIODevice
 *   "battery" nubs). Fields (units as published by ApplePMU): Voltage mV,
 *   Current mAh (remaining), Capacity mAh (full-charge), Amperage mA,
 *   "Cycle Count", Flags, AbsoluteMaxCapacity mAh (design).
 *
 *   Smart-battery style (newer stacks): entries with "MaxCapacity" +
 *   "Voltage".
 *
 * Also lists IOHWSensor service nodes located in the battery
 * (temperature/voltage/current - e.g. "BATTERY" temp on iBook G4).
 *
 * Derived values are marked as derived: charge %, health %, the 3S/4S
 * series-count guess (heuristic, NOT a hardware fact) and ~V per cell.
 *
 * Watch mode (-w N): the whole frame is rendered into an in-memory buffer
 * first (registry walking is slow - doing it after clear would leave the
 * screen blank and visibly flicker), then redrawn in place: cursor home,
 * overwrite, "\033[K" per line, "\033[J" below the frame. No clear-screen
 * phase, no flicker.
 *
 * Usage: batinfo [-w SECONDS] [-h]
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ------------------------------------------------------------ helpers */

static int props_of(io_object_t entry, CFMutableDictionaryRef *out)
{
    return IORegistryEntryCreateCFProperties(entry, out,
                                             kCFAllocatorDefault, 0);
}

static int get_num(CFDictionaryRef d, CFStringRef key, SInt64 *out)
{
    CFTypeRef v = CFDictionaryGetValue(d, key);
    if (!v || CFGetTypeID(v) != CFNumberGetTypeID())
        return 0;
    return CFNumberGetValue((CFNumberRef)v, kCFNumberSInt64Type, out);
}

static int get_str(CFDictionaryRef d, CFStringRef key, char *buf, size_t n)
{
    CFTypeRef v = CFDictionaryGetValue(d, key);
    if (!v || CFGetTypeID(v) != CFStringGetTypeID())
        return 0;
    return CFStringGetCString((CFStringRef)v, buf, (CFIndex)n,
                              kCFStringEncodingUTF8);
}

static CFArrayRef get_array(CFDictionaryRef d, CFStringRef key)
{
    CFTypeRef v = CFDictionaryGetValue(d, key);
    if (!v || CFGetTypeID(v) != CFArrayGetTypeID())
        return NULL;
    return (CFArrayRef)v;
}

/* like get_str, but also accepts CFData (device-tree strings are often
   published as data, e.g. "device_type" = <"battery"> on PMU nubs)   */
static int get_prop_str(CFDictionaryRef d, CFStringRef key,
                        char *buf, size_t n)
{
    CFTypeRef v = CFDictionaryGetValue(d, key);
    if (!v)
        return 0;
    if (CFGetTypeID(v) == CFStringGetTypeID())
        return CFStringGetCString((CFStringRef)v, buf, (CFIndex)n,
                                  kCFStringEncodingUTF8);
    if (CFGetTypeID(v) == CFDataGetTypeID()) {
        CFIndex len = CFDataGetLength((CFDataRef)v);
        if (len >= (CFIndex)n)
            len = (CFIndex)n - 1;
        if (len < 0)
            return 0;
        memcpy(buf, CFDataGetBytePtr((CFDataRef)v), (size_t)len);
        buf[len] = 0;
        return 1;
    }
    return 0;
}

static void bits_list(unsigned long long flags, char *buf, size_t n)
{
    int    first = 1;
    int    b;
    size_t used = 0;

    buf[0] = 0;
    for (b = 0; b < 64 && used + 10 < n; b++)
        if (flags & ((unsigned long long)1 << b)) {
            used += (size_t)snprintf(buf + used, n - used, "%s%d",
                                     first ? "" : ",", b);
            first = 0;
        }
}

/* ------------------------------------------------------------- output */

/* one-frame text buffer: everything is rendered here BEFORE any terminal
   output, so the redraw itself is a single fast pass */

static char   obuf[16384];
static size_t olen;

static void obuf_reset(void)
{
    olen = 0;
    obuf[0] = 0;
}

static void addf(const char *fmt, ...)
{
    va_list ap;
    int     n;

    if (olen >= sizeof obuf - 1)
        return;
    va_start(ap, fmt);
    n = vsnprintf(obuf + olen, sizeof obuf - olen, fmt, ap);
    va_end(ap);
    if (n > 0) {
        olen += (size_t)n;
        if (olen >= sizeof obuf)
            olen = sizeof obuf - 1;
    }
}

/* flicker-free redraw: cursor home without clearing, overwrite every
   line (clearing its old tail), erase everything below the new frame */
static void redraw(void)
{
    const char *p;

    printf("\033[H");
    for (p = obuf; *p; p++) {
        if (*p == '\n')
            fputs("\033[K", stdout);
        putchar(*p);
    }
    fputs("\033[J", stdout);
}

/* ------------------------------------------------------------ battery */

static void print_battery_dict(CFDictionaryRef item, int idx)
{
    SInt64 voltage = 0, current = 0, capacity = 0, design = 0;
    SInt64 amperage = 0, cycles = 0, flags = 0;
    int    have_v, have_c, have_cap, have_des, have_amp, have_f;
    double rem_pct = -1.0;
    char   bitbuf[160];
    char   vcell[128];

    have_v   = get_num(item, CFSTR("Voltage"), &voltage);
    have_c   = get_num(item, CFSTR("Current"), &current);
    have_cap = get_num(item, CFSTR("Capacity"), &capacity);
    have_des = get_num(item, CFSTR("AbsoluteMaxCapacity"), &design);
    have_amp = get_num(item, CFSTR("Amperage"), &amperage);
    (void)get_num(item, CFSTR("Cycle Count"), &cycles);
    have_f   = get_num(item, CFSTR("Flags"), &flags);

    addf("BATTERY %d\n", idx);

    if (have_c && have_cap && capacity > 0) {
        rem_pct = 100.0 * (double)current / (double)capacity;
        addf("  Remaining        %7lld mAh  (%.1f %% of full charge,"
             " derived)\n", current, rem_pct);
    } else if (have_c) {
        addf("  Remaining        %7lld mAh\n", current);
    }
    if (have_cap)
        addf("  Full charge cap. %7lld mAh\n", capacity);
    if (have_des) {
        if (capacity > 0)
            addf("  Design cap.      %7lld mAh  (health %.1f %%,"
                 " derived)\n", design,
                 100.0 * (double)capacity / (double)design);
        else
            addf("  Design cap.      %7lld mAh\n", design);
    }
    if (cycles)
        addf("  Cycle count      %7lld\n", cycles);

    /* series-count guess: only credible when the pack is charged above
       50% - the 3S/4S voltage ranges unambiguously overlap below that.
       Voltage is published in mV - convert before comparing to volts. */
    vcell[0] = 0;
    if (have_v && rem_pct > 50.0) {
        double vv = (double)voltage / 1000.0;
        double p3 = vv / 3.0;
        double p4 = vv / 4.0;
        if (p3 >= 3.3 && p3 <= 4.3)
            snprintf(vcell, sizeof vcell,
                     "(~3S: ~%.2f V/cell, derived)", p3);
        else if (p4 >= 3.3 && p4 <= 4.3)
            snprintf(vcell, sizeof vcell,
                     "(~4S: ~%.2f V/cell, derived)", p4);
    }
    if (have_v)
        addf("  Pack voltage     %7lld mV   (%.2f V) %s\n",
             voltage, (double)voltage / 1000.0, vcell);

    if (have_amp) {
        const char *note;
        if (amperage > 0)
            note = "(>0 = charging; PMU-era sign convention)";
        else if (amperage < 0)
            note = "(<0 = discharging)";
        else
            note = "(no charge/disch. current - on AC)";
        addf("  Amperage         %7lld mA   %s\n", amperage, note);
    }
    if (have_f) {
        addf("  Flags            0x%llx / %lld\n", flags, flags);
        bits_list((unsigned long long)flags, bitbuf, sizeof bitbuf);
        addf("                   set bits: %s\n", bitbuf);
    }
}

static int print_pmu_batteries(void)
{
    io_iterator_t it = 0;
    int           found = 0, idx = 0;

    if (IORegistryCreateIterator(kIOMasterPortDefault, kIOServicePlane,
                                 kIORegistryIterateRecursively, &it)
            != KERN_SUCCESS)
        return 0;

    for (;;) {
        io_object_t            e;
        CFMutableDictionaryRef d = NULL;
        CFArrayRef             batt;

        e = IOIteratorNext(it);
        if (!e)
            break;
        if (props_of(e, &d) == KERN_SUCCESS && d) {
            char dt[32];
            /* take only dedicated battery nubs, skip PMU's own copy
               (it publishes the same dict without device_type=battery);
               device_type is CFData on these nubs - use get_prop_str */
            if (!get_prop_str(d, CFSTR("device_type"), dt, sizeof dt) ||
                strcmp(dt, "battery") != 0) {
                CFRelease(d);
                d = NULL;
            }
            if (d) {
                batt = get_array(d, CFSTR("IOBatteryInfo"));
                if (batt) {
                    CFIndex i, n = CFArrayGetCount(batt);
                    for (i = 0; i < n; i++) {
                        CFTypeRef item = CFArrayGetValueAtIndex(batt, i);
                        if (item &&
                            CFGetTypeID(item) == CFDictionaryGetTypeID())
                            print_battery_dict((CFDictionaryRef)item, idx++);
                    }
                    found = 1;
                }
                CFRelease(d);
            }
        }
        IOObjectRelease(e);
    }
    IOObjectRelease(it);
    return found;
}

/* smart-battery style: keys MaxCapacity / CurrentCapacity / Voltage */
static int print_smart_batteries(void)
{
    io_iterator_t it = 0;
    int           found = 0, idx = 0;

    if (IORegistryCreateIterator(kIOMasterPortDefault, kIOServicePlane,
                                 kIORegistryIterateRecursively, &it)
            != KERN_SUCCESS)
        return 0;

    for (;;) {
        io_object_t            e;
        CFMutableDictionaryRef d = NULL;
        SInt64 v = 0, maxc = 0, currc = 0, desgc = 0, cyc = 0, amp = 0;

        e = IOIteratorNext(it);
        if (!e)
            break;
        if (props_of(e, &d) == KERN_SUCCESS && d &&
            get_num(d, CFSTR("MaxCapacity"), &maxc) &&
            get_num(d, CFSTR("Voltage"), &v) &&
            maxc > 0 && v > 0) {          /* skip present-but-empty stubs */
            get_num(d, CFSTR("CurrentCapacity"), &currc);
            get_num(d, CFSTR("DesignCapacity"), &desgc);
            get_num(d, CFSTR("CycleCount"), &cyc);
            if (!get_num(d, CFSTR("InstantAmperage"), &amp))
                get_num(d, CFSTR("Amperage"), &amp);

            addf("BATTERY %d (smart-battery style)\n", idx++);
            addf("  Remaining        %7lld mAh", currc);
            if (maxc > 0)
                addf("  (%.1f %%, derived)",
                     100.0 * (double)currc / (double)maxc);
            addf("\n  Full charge cap. %7lld mAh\n", maxc);
            if (desgc > 0) {
                addf("  Design cap.      %7lld mAh", desgc);
                if (maxc > 0)
                    addf("  (health %.1f %%, derived)",
                         100.0 * (double)maxc / (double)desgc);
                addf("\n");
            }
            addf("  Pack voltage     %7lld mV   (%.2f V)\n", v,
                 (double)v / 1000.0);
            addf("  Amperage         %7lld mA\n", amp);
            if (cyc)
                addf("  Cycle count      %7lld\n", cyc);
            found = 1;
        }
        if (d)
            CFRelease(d);
        IOObjectRelease(e);
    }
    IOObjectRelease(it);
    return found;
}

/* --------------------------------------------------- battery sensors */

static int print_battery_sensors(void)
{
    io_iterator_t it = 0;
    int           found = 0;

    if (IORegistryCreateIterator(kIOMasterPortDefault, kIOServicePlane,
                                 kIORegistryIterateRecursively, &it)
            != KERN_SUCCESS)
        return 0;

    for (;;) {
        io_object_t            e;
        CFMutableDictionaryRef d = NULL;
        char cls[64], type[16], loc[64];
        SInt64 cur = 0;

        e = IOIteratorNext(it);
        if (!e)
            break;
        if (props_of(e, &d) == KERN_SUCCESS && d &&
            get_num(d, CFSTR("current-value"), &cur) &&
            get_str(d, CFSTR("type"), type, sizeof type) &&
            get_str(d, CFSTR("location"), loc, sizeof loc) &&
            IOObjectGetClass(e, cls) == KERN_SUCCESS &&
            strcmp(cls, "IOHWSensor") == 0 &&
            strstr(loc, "BATTERY") != NULL) {
            if (!found) {
                addf("BATTERY SENSORS\n");
                found = 1;
            }
            if (strcmp(type, "temperature") == 0 ||
                strcmp(type, "temp") == 0)
                addf("  %-13s %7.1f C\n", loc, (double)cur / 65536.0);
            else if (strcmp(type, "voltage") == 0)
                addf("  %-13s %7.2f V\n", loc, (double)cur / 65536.0);
            else if (strcmp(type, "current") == 0)
                addf("  %-13s %7.2f A\n", loc, (double)cur / 65536.0);
        }
        if (d)
            CFRelease(d);
        IOObjectRelease(e);
    }
    IOObjectRelease(it);
    return found;
}

/* --------------------------------------------------------------- main */

static void usage(void)
{
    printf("batinfo - battery status for PowerPC Macs (10.4/10.5)\n\n"
           "usage: batinfo [-w SECONDS] [-h]\n"
           "  -w N  watch mode: refresh every N seconds\n"
           "  -h    this help\n");
}

/* render one full frame into obuf; returns # of battery sets found */
static int snapshot(int watch)
{
    int pmu, sb;

    obuf_reset();
    addf("batinfo (PowerPC Mac OS X 10.4/10.5, public IOKit registry)\n");
    if (watch)
        addf("press Ctrl-C to exit watch mode\n");
    addf("\n");

    pmu = print_pmu_batteries();
    sb  = print_smart_batteries();
    print_battery_sensors();
    return pmu + sb;
}

int main(int argc, char **argv)
{
    int    watch = 0;
    double interval = 0;
    int    ch;

    while ((ch = getopt(argc, argv, "w:h")) != -1) {
        switch (ch) {
        case 'w': watch = 1; interval = atof(optarg); break;
        case 'h': usage(); return 0;
        default:  usage(); return 1;
        }
    }
    if (watch && interval <= 0) {
        fprintf(stderr, "batinfo: bad -w interval\n");
        return 1;
    }

    if (!watch) {
        if (snapshot(0) == 0) {
            fprintf(stderr, "batinfo: no battery entries found"
                            " (desktop machine? unsupported battery path?)\n");
            return 1;
        }
        fputs(obuf, stdout);
        return 0;
    }

    while (1) {
        if (snapshot(1) == 0)
            addf("no battery entries found"
                 " (desktop machine? unsupported battery path?)\n");
        redraw();              /* single fast pass, no clear-screen */
        fflush(stdout);
        usleep((useconds_t)(interval * 1000000.0));
    }
}
