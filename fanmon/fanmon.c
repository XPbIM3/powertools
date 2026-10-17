/*
 * fanmon.c - console fans/temperature monitor for Mac OS X Tiger/Leopard
 *             (PowerPC G4/G5)
 *
 * Two data paths, auto-detected (no private APIs - pure IOKit public
 * registry access):
 *
 *   SMU (G5: iMac G5 / Power Mac G5): reads the "IOHWControls" and
 *   "IOHWSensors" dictionaries published by the platform thermal driver.
 *
 *   PMU (G4: iBook/PowerBook G4, Mac mini G4, Power Mac G4): such arrays
 *   do not exist; falls back to enumerating the per-sensor IOHWSensor
 *   service nodes (AppleHWSensor driver): temperature / voltage /
 *   fanspeed, same 16.16 fixed point scaling.
 *
 * Scaling notes:
 *   - temperatures / power / current / voltage are stored as 16.16 fixed
 *     point (divide by 65536). Verified against the internal PID profile:
 *     HD input-target 3801088 => 58.0 C, power-max 2949120 => 45 W (G5 TDP).
 *   - fan controllers of type "fan-rpm" (SMU path): plain RPM, taken from
 *     "target-value" - the only number that actually moves on SMU
 *     machines; the registry "current-value" stays frozen at ~min
 *     regardless of load (verified on iMac G5, Tiger 10.4.11 and
 *     Leopard 10.5.8, see notes-*.md).
 *   - PMU "fanspeed" sensors: plain RPM; a reading of 0 is VALID - the
 *     PMU switches the fan off entirely while the machine is cool
 *     (observed on iBook G4 under Tiger 10.4.11).
 *
 * Usage: fanmon [-w SECONDS]
 *   -w N   watch mode: refresh every N seconds (default: single run)
 *   -h     help
 *
 * Watch mode (-w N): each frame is fully rendered into an in-memory
 * buffer BEFORE any terminal output (registry walking is slow - a
 * clear-screen done before it would leave a visible blank phase, i.e.
 * flicker), then redrawn in place with a single fast pass: cursor home
 * (no clear-screen), overwrite, "\033[K" per line, "\033[J" below the
 * frame.
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

/* ------------------------------------------------------------- output */

/* one-frame text buffer: the frame is rendered here BEFORE any terminal
   output, so the redraw itself is a single fast pass (no blank phase,
   no flicker) */

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
    SInt64          cur, mn, mx, val;

    if (props_of(owner, &d) != KERN_SUCCESS || !d) {
        addf("fanmon: cannot read registry properties\n");
        return;
    }

    /* ---- fans (IOHWControls, type fan-rpm) ---- */
    controls = get_array(d, CFSTR("IOHWControls"));
    n = controls ? CFArrayGetCount(controls) : 0;
    addf("FANS\n");
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
        /* Display RPM from target-value (the live number); fall back to
           the frozen current-value only if no target is published. */
        if (!get_num((CFDictionaryRef)item, CFSTR("target-value"), &val) &&
            !get_num((CFDictionaryRef)item, CFSTR("current-value"), &val))
            continue;
        addf("  %s %6.0f rpm", pad(loc, 13), (double)val);
        if (get_num((CFDictionaryRef)item, CFSTR("min-value"), &mn) &&
            get_num((CFDictionaryRef)item, CFSTR("max-value"), &mx))
            addf("  (%ld..%ld)", (long)mn, (long)mx);
        addf("\n");
    }

    /* ---- sensors (IOHWSensors) ---- */
    sensors = get_array(d, CFSTR("IOHWSensors"));
    n = sensors ? CFArrayGetCount(sensors) : 0;
    addf("TEMPERATURES\n");
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
            addf("  %s %7.1f C\n", pad(loc, 13), (double)cur / 65536.0);
    }
    addf("ELECTRICAL\n");
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
           { unit = "W"; addf("  %s %7.2f %s\n", pad(loc, 13), (double)cur / 65536.0, unit); }
        else if (strcmp(type, "current") == 0)
            { unit = "A"; addf("  %s %7.2f %s\n", pad(loc, 13), (double)cur / 65536.0, unit); }
        else if (strcmp(type, "voltage") == 0)
            { unit = "V"; addf("  %s %7.2f %s\n", pad(loc, 13), (double)cur / 65536.0, unit); }
    }

    CFRelease(d);
}

/* ----------------------------------------------------------- PMU path */

/* G4/PMU machines publish one IOHWSensor service node per sensor
   (AppleHWSensor driver) instead of the SMU-style dictionary arrays.
   Same 16.16 scaling; "fanspeed" RPM may legitimately be 0 (the PMU
   keeps the fan off while the machine is cool). */

typedef struct {
    char    loc[64];
    char    type[16];
    SInt64  cur;
    SInt64  id;
} pmsens_t;

#define PMU_MAX 32

static int pmu_collect(pmsens_t *out, int maxn)
{
    io_iterator_t it = 0;
    int           n = 0;

    if (IORegistryCreateIterator(kIOMasterPortDefault, kIOServicePlane,
                                 kIORegistryIterateRecursively, &it)
            != KERN_SUCCESS)
        return 0;

    for (;;) {
        io_object_t             e;
        CFMutableDictionaryRef  d = NULL;
        char                    cls[64], type[16], loc[64];
        SInt64                  cur, id;

        e = IOIteratorNext(it);
        if (!e)
            break;

        if (props_of(e, &d) == KERN_SUCCESS && d &&
            !get_array(d, CFSTR("IOHWSensors")) &&          /* not SMU */
            get_num(d, CFSTR("current-value"), &cur) &&
            get_str(d, CFSTR("type"), type, sizeof type) &&
            get_str(d, CFSTR("location"), loc, sizeof loc) &&
            IOObjectGetClass(e, cls) == KERN_SUCCESS &&
            strcmp(cls, "IOHWSensor") == 0 &&
            n < maxn) {
            if (get_num(d, CFSTR("sensor-id"), &id))
                out[n].id = id;
            else
                out[n].id = n;
            out[n].cur = cur;
            snprintf(out[n].loc, sizeof out[n].loc, "%s", loc);
            snprintf(out[n].type, sizeof out[n].type, "%s", type);
            n++;
        }
        if (d)
            CFRelease(d);
        IOObjectRelease(e);
    }
    IOObjectRelease(it);

    /* stable order: by sensor-id */
    {
        int i, j;
        for (i = 1; i < n; i++) {
            pmsens_t k = out[i];
            for (j = i - 1; j >= 0 && out[j].id > k.id; j--)
                out[j + 1] = out[j];
            out[j + 1] = k;
        }
    }
    return n;
}

static int pmu_snapshot(void)
{
    pmsens_t s[PMU_MAX];
    int      n, i;

    n = pmu_collect(s, PMU_MAX);
    if (n == 0)
        return 0;

    addf("FANS (PMU)\n");
    for (i = 0; i < n; i++)
        if (strcmp(s[i].type, "fanspeed") == 0)
            addf("  %s %6.0f rpm\n", pad(s[i].loc, 13), (double)s[i].cur);

    addf("TEMPERATURES\n");
    for (i = 0; i < n; i++)
        if (strcmp(s[i].type, "temp") == 0 ||
            strcmp(s[i].type, "temperature") == 0)
            addf("  %s %7.1f C\n", pad(s[i].loc, 13),
                 (double)s[i].cur / 65536.0);

    addf("ELECTRICAL\n");
    for (i = 0; i < n; i++)
        if (strcmp(s[i].type, "voltage") == 0)
            addf("  %s %7.2f V\n", pad(s[i].loc, 13),
                 (double)s[i].cur / 65536.0);
        else if (strcmp(s[i].type, "current") == 0)
            addf("  %s %7.2f A\n", pad(s[i].loc, 13),
                 (double)s[i].cur / 65536.0);
        else if (strcmp(s[i].type, "power") == 0)
            addf("  %s %7.2f W\n", pad(s[i].loc, 13),
                 (double)s[i].cur / 65536.0);
    return 1;
}

/* --------------------------------------------------------------- main */

static void usage(void)
{
    printf("fanmon - fans & temperature monitor for G5 (SMU) and G4 (PMU) Macs\n\n"
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

    owner = find_owner();                  /* SMU (G5) path */

    if (!watch) {
        if (owner) {
            print_snapshot(owner);
            IOObjectRelease(owner);
            fputs(obuf, stdout);
            return 0;
        }
        if (pmu_snapshot()) {              /* PMU (G4) path */
            fputs(obuf, stdout);
            return 0;
        }
        fprintf(stderr, "fanmon: no IOHWSensors/IOHWControls owner found"
                        " and no IOHWSensor nodes"
                        " (unsupported machine?)\n");
        return 1;
    }

    while (1) {
        obuf_reset();
        if (owner) {
            print_snapshot(owner);
        } else if (!pmu_snapshot()) {
            addf("fanmon: unsupported machine?\n");
        }
        /* output goes out only now: the frame was fully rendered off-
           screen, the redraw keeps the text in place between frames */
        redraw();
        fflush(stdout);
        usleep((useconds_t)(interval * 1000000.0));
    }
}
