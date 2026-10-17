/*
 * fanmon.c - console fans/temperature monitor for Mac OS X Tiger/Leopard (PowerPC)
 *
 * Reads the "IOHWControls" and "IOHWSensors" dictionaries published in the
 * IORegistry by the platform thermal driver (AppleSMU on iMac G5 / SMU-based
 * machines). No private APIs - pure IOKit public registry access.
 *
 * Scaling notes:
 *   - temperatures / power / current / voltage are stored as 16.16 fixed
 *     point (divide by 65536). Verified against the internal PID profile:
 *     HD input-target 3801088 => 58.0 C, power-max 2949120 => 45 W (G5 TDP).
 *   - fan controllers of type "fan-rpm" expose plain RPM.
 *
 * Usage: fanmon [-w SECONDS]
 *   -w N   watch mode: refresh every N seconds (default: single run)
 *   -h     help
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

/* find the registry node that owns the IOHW* arrays (AppleSMU family) */
static io_object_t find_owner(void)
{
    io_iterator_t it = 0;
    io_object_t   found = 0;

    if (IORegistryCreateIterator(kIOMasterPortDefault, kIOServicePlane,
                                 kIORegistryIterateRecursively, &it)
            != KERN_SUCCESS)
        return 0;

    for (;;) {
        io_object_t             e;
        CFMutableDictionaryRef  d = NULL;

        e = IOIteratorNext(it);
        if (!e)
            break;
        if (props_of(e, &d) == KERN_SUCCESS && d) {
            if (get_array(d, CFSTR("IOHWSensors")) &&
                get_array(d, CFSTR("IOHWControls"))) {
                IOObjectRetain(e);
                found = e;
            }
            CFRelease(d);
        }
        IOObjectRelease(e);
        if (found)
            break;
    }
    IOObjectRelease(it);
    return found;
}

/* ------------------------------------------------------------ display */

static const char *pad(const char *s, size_t width)
{
    static char buf[64];
    snprintf(buf, sizeof buf, "%-*s", (int)width, s);
    return buf;
}

static void print_snapshot(io_object_t owner)
{
    CFMutableDictionaryRef  d = NULL;
    CFArrayRef              sensors, controls;
    CFIndex                 i, n;
    char            loc[64], type[64];
    SInt64          cur, mn, mx, tgt;
    int             have;

    if (props_of(owner, &d) != KERN_SUCCESS || !d) {
        printf("fanmon: cannot read registry properties\n");
        return;
    }

    /* ---- fans (IOHWControls, type fan-rpm) ---- */
    controls = get_array(d, CFSTR("IOHWControls"));
    n = controls ? CFArrayGetCount(controls) : 0;
    printf("FANS\n");
    for (i = 0; i < n; i++) {
        CFTypeRef item = CFArrayGetValueAtIndex(controls, i);

        if (!item || CFGetTypeID(item) != CFDictionaryGetTypeID())
            continue;
        if (!get_str((CFDictionaryRef)item, CFSTR("type"), type, sizeof type))
            continue;
        if (strcmp(type, "fan-rpm") != 0)
            continue;
        if (!get_str((CFDictionaryRef)item, CFSTR("location"), loc, sizeof loc))
            snprintf(loc, sizeof loc, "fan #%ld", (long)i);
        have = get_num((CFDictionaryRef)item, CFSTR("current-value"), &cur);
        if (!have)
            continue;
        printf("  %s %6.0f rpm", pad(loc, 13), (double)cur);
        if (get_num((CFDictionaryRef)item, CFSTR("min-value"), &mn) &&
            get_num((CFDictionaryRef)item, CFSTR("max-value"), &mx))
            printf("  (%ld..%ld)", (long)mn, (long)mx);
        if (get_num((CFDictionaryRef)item, CFSTR("target-value"), &tgt))
            printf("  target %ld", (long)tgt);
        printf("\n");
    }

    /* ---- sensors (IOHWSensors) ---- */
    sensors = get_array(d, CFSTR("IOHWSensors"));
    n = sensors ? CFArrayGetCount(sensors) : 0;
    printf("TEMPERATURES\n");
    for (i = 0; i < n; i++) {
        CFTypeRef item = CFArrayGetValueAtIndex(sensors, i);

        if (!item || CFGetTypeID(item) != CFDictionaryGetTypeID())
            continue;
        if (!get_str((CFDictionaryRef)item, CFSTR("type"), type, sizeof type))
            continue;
        if (!get_str((CFDictionaryRef)item, CFSTR("location"), loc, sizeof loc))
            snprintf(loc, sizeof loc, "sensor #%ld", (long)i);
        if (!get_num((CFDictionaryRef)item, CFSTR("current-value"), &cur))
            continue;
        if (strcmp(type, "temp") == 0 || strcmp(type, "temperature") == 0)
            printf("  %s %7.1f C\n", pad(loc, 13), (double)cur / 65536.0);
    }
    printf("ELECTRICAL\n");
    for (i = 0; i < n; i++) {
        CFTypeRef item = CFArrayGetValueAtIndex(sensors, i);
        const char *unit;

        if (!item || CFGetTypeID(item) != CFDictionaryGetTypeID())
            continue;
        if (!get_str((CFDictionaryRef)item, CFSTR("type"), type, sizeof type))
            continue;
        if (!get_str((CFDictionaryRef)item, CFSTR("location"), loc, sizeof loc))
            snprintf(loc, sizeof loc, "sensor #%ld", (long)i);
        if (!get_num((CFDictionaryRef)item, CFSTR("current-value"), &cur))
            continue;
        if (strcmp(type, "power") == 0)
           { unit = "W"; printf("  %s %7.2f %s\n", pad(loc, 13), (double)cur / 65536.0, unit); }
        else if (strcmp(type, "current") == 0)
            { unit = "A"; printf("  %s %7.2f %s\n", pad(loc, 13), (double)cur / 65536.0, unit); }
        else if (strcmp(type, "voltage") == 0)
            { unit = "V"; printf("  %s %7.2f %s\n", pad(loc, 13), (double)cur / 65536.0, unit); }
    }

    CFRelease(d);
}

/* --------------------------------------------------------------- main */

static void usage(void)
{
    printf("fanmon - fans & temperature monitor for Mac OS X Tiger (PPC)\n\n"
           "usage: fanmon [-w SECONDS] [-h]\n"
           "  -w N  watch mode: refresh every N seconds\n"
           "  -h    this help\n");
}

int main(int argc, char **argv)
{
    int         watch = 0;
    double      interval = 0;
    int         ch;
    io_object_t owner;

    while ((ch = getopt(argc, argv, "w:h")) != -1) {
        switch (ch) {
        case 'w': watch = 1; interval = atof(optarg); break;
        case 'h': usage(); return 0;
        default:  usage(); return 1;
        }
    }
    if (watch && interval <= 0) {
        fprintf(stderr, "fanmon: bad -w interval\n");
        return 1;
    }

    owner = find_owner();
    if (!owner) {
        fprintf(stderr, "fanmon: no IOHWSensors/IOHWControls owner found"
                        " (unsupported machine?)\n");
        return 1;
    }

    if (!watch) {
        print_snapshot(owner);
        IOObjectRelease(owner);
        return 0;
    }

    while (1) {
        printf("\033[H\033[2J");
        print_snapshot(owner);
        fflush(stdout);
        usleep((useconds_t)(interval * 1000000.0));
    }
}
