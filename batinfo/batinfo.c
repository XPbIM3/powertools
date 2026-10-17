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
 * Usage: batinfo [-h]
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
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

static void bits_list(unsigned long long flags, char *buf, size_t n){
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

    printf("BATTERY %d\n", idx);

    if (have_c && have_cap && capacity > 0) {
        rem_pct = 100.0 * (double)current / (double)capacity;
        printf("  Remaining        %7lld mAh  (%.1f %% of full charge,"
               " derived)\n", current, rem_pct);
    } else if (have_c) {
        printf("  Remaining        %7lld mAh\n", current);
    }
    if (have_cap)
        printf("  Full charge cap. %7lld mAh\n", capacity);
    if (have_des) {
        if (capacity > 0)
            printf("  Design cap.      %7lld mAh  (health %.1f %%,"
                   " derived)\n", design,
                   100.0 * (double)capacity / (double)design);
        else
            printf("  Design cap.      %7lld mAh\n", design);
    }
    if (cycles)
        printf("  Cycle count      %7lld\n", cycles);

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
        printf("  Pack voltage     %7lld mV   (%.2f V) %s\n",
               voltage, (double)voltage / 1000.0, vcell);

    if (have_amp) {
        const char *note;
        if (amperage > 0)
            note = "(>0 = charging; PMU-era sign convention)";
        else if (amperage < 0)
            note = "(<0 = discharging)";
        else
            note = "(no charge/disch. current - on AC)";
        printf("  Amperage         %7lld mA   %s\n", amperage, note);
    }
    if (have_f) {
        printf("  Flags            0x%llx / %lld\n",
               flags, flags);
        bits_list((unsigned long long)flags, bitbuf, sizeof bitbuf);
        printf("                   set bits: %s\n", bitbuf);
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

            printf("BATTERY %d (smart-battery style)\n", idx++);
            printf("  Remaining        %7lld mAh", currc);
            if (maxc > 0)
                printf("  (%.1f %%, derived)",
                       100.0 * (double)currc / (double)maxc);
            printf("\n  Full charge cap. %7lld mAh\n", maxc);
            if (desgc > 0) {
                printf("  Design cap.      %7lld mAh", desgc);
                if (maxc > 0)
                    printf("  (health %.1f %%, derived)",
                           100.0 * (double)maxc / (double)desgc);
                printf("\n");
            }
            printf("  Pack voltage     %7lld mV   (%.2f V)\n", v,
                   (double)v / 1000.0);
            printf("  Amperage         %7lld mA\n", amp);
            if (cyc)
                printf("  Cycle count      %7lld\n", cyc);
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
                printf("BATTERY SENSORS\n");
                found = 1;
            }
            if (strcmp(type, "temperature") == 0 ||
                strcmp(type, "temp") == 0)
                printf("  %-13s %7.1f C\n", loc, (double)cur / 65536.0);
            else if (strcmp(type, "voltage") == 0)
                printf("  %-13s %7.2f V\n", loc, (double)cur / 65536.0);
            else if (strcmp(type, "current") == 0)
                printf("  %-13s %7.2f A\n", loc, (double)cur / 65536.0);
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

/* one full snapshot; returns 0 if nothing battery-like was found */
static int snapshot(void)
{
    int pmu, sb;

    printf("batinfo (PowerPC Mac OS X 10.4/10.5, public IOKit registry)\n"
           "press Ctrl-C to exit watch mode\n\n");

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
        if (snapshot())
            return 0;
        fprintf(stderr, "batinfo: no battery entries found"
                        " (desktop machine? unsupported battery path?)\n");
        return 1;
    }

    while (1) {
        printf("\033[H\033[2J");
        if (!snapshot())
            fprintf(stderr, "batinfo: no battery entries found"
                            " (desktop machine? unsupported battery path?)\n");
        fflush(stdout);
        usleep((useconds_t)(interval * 1000000.0));
    }
}
