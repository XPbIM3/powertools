/*
 * dgprobe.c - bare CLI probe of the disk I/O counters that feed
 * diskgauge.  Same IOKit query (IOBlockStorageDriver -> Statistics
 * -> "Bytes (Read)" / "Bytes (Write)"), no AppKit, prints the
 * cumulative counters and the per-second rates.
 *
 * Purpose: cross-check the tray numbers objectively (ssh-able),
 * e.g.  `dgprobe 6 &  dd ... ` while the gauge shows what it shows.
 *
 * Cross-build (canon park flags, ppc/10.3):
 *   gcc -arch ppc -mcpu=G3 -Wl,-force_cpusubtype_ALL \
 *       -mmacosx-version-min=10.3 -std=gnu99 -O1 -Wall \
 *       dgprobe.c -o dgprobe -framework CoreFoundation -framework IOKit
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <unistd.h>

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
        return 0;

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

#define MB (1024.0 * 1024.0)

int main(int argc, char **argv)
{
    int       n    = (argc > 1) ? atoi(argv[1]) : 6;
    long long r = 0, w = 0, pr = 0, pw = 0;
    int       i, have = 0;
    double    cr, cw;                            /* cumulative MB */

    if (!disk_bytes(&r, &w)) {
        printf("dgprobe: no IOBlockStorageDriver statistics\n");
        return 1;
    }

    for (i = 0; i < n; i++) {
        sleep(1);
        if (!disk_bytes(&r, &w)) { fflush(stdout); continue; }
        cr = (double)r / MB;
        cw = (double)w / MB;
        if (have) {
            double rr = (double)(r - pr) / MB;
            double ww = (double)(w - pw) / MB;
            printf("tick %d: R %7.1f MB/s  W %7.1f MB/s"
                   "   (cum R %10.1f  W %10.1f MB)\n",
                   i + 1, rr, ww, cr, cw);
        } else {
            printf("tick 1: base snapshot (cum R %10.1f  W %10.1f MB)\n",
                   cr, cw);
        }
        fflush(stdout);
        pr = r;
        pw = w;
        have = 1;
    }
    return 0;
}
