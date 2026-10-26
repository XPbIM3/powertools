/*
 * smartinfo.c - read-only SMART summary for ATA disks (Mac OS X 10.2 - 10.5)
 *
 * What it reads (three calls per disk, nothing else):
 *   SMARTReturnStatus         overall "threshold exceeded" flag
 *   SMARTReadData             512-byte attribute table + status bytes
 *   SMARTReadDataThresholds   per-attribute thresholds
 *
 * READ-ONLY BY CONSTRUCTION: no SMARTEnableDisableOperations, no
 * SMARTEnableDisableAutosave, no SMARTExecuteOffLineImmediate, no
 * SMARTWriteLogAtAddress. No code path mutates drive state.
 *
 * Device discovery walks /dev/diskN upwards through the IOService plane
 * and accepts the first node that is either
 *   - "SMART Capable" = Yes                  (10.4+)
 *   - IOUserClientClass = "ATASMARTUserClient" (10.2/10.3, no key yet)
 *   - IOATABlockStorageDevice + ATA Features & kIOATAFeatureSMART
 * On 10.2/10.3 the "ATA Features" key may live either on the node itself
 * or inside "Device Characteristics" - both are tried.
 *
 * The plugin UUID is identical in the 10.2.8, 10.3.9, 10.4u and 10.5 SDKs,
 * so the same binary reaches PATA (ATASMARTUserClient), G5 K2 SATA
 * (AppleSATLSMARTUserClient) and AHCI (AHCISMARTUserClient).
 *
 * Version gate: GetATAIdentifyData is the last vtable entry and was added
 * in 10.3 - it is never called here. Model/serial/firmware come from the
 * IORegistry "Device Characteristics" dictionary, which exists on 10.2 as
 * well. That keeps the 10.2 ABI untouched.
 *
 * Usage: smartinfo [-a] [-d diskN] [-h]
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOBSD.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOReturn.h>
#include <IOKit/storage/IOMedia.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <IOKit/storage/IOCDBlockStorageDevice.h>
#include <IOKit/storage/IOStorageDeviceCharacteristics.h>
#include <IOKit/storage/ata/IOATAStorageDefines.h>
#include <IOKit/storage/ata/ATASMARTLib.h>

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <inttypes.h>
#include <sys/sysctl.h>

#define MAX_DISKS   32
#define NATTR       30

/* ATA_BLOCK_CLASS was dropped from the 10.5 SDK headers
   (the class itself is still there); the registry class name is stable. */
#define ATA_BLOCK_CLASS "IOATABlockStorageDevice"

enum { VD_NA = 0, VD_OK, VD_WARN, VD_FAIL };

typedef struct {
    char        name[16];
    char        model[44];
    char        serial[40];
    char        fw[24];
    char        bus[24];
    uint64_t    bytes;
    int         optical;
    int         vd;                 /* verdict */
    char        note[80];
    int         have_data;
    int         have_thr;
    int         exceeded;
    ATASMARTData           sd;
    ATASMARTDataThresholds th;
    /* snapshot of the counters we report in the summary table */
    uint64_t    n_realloc, n_pend, n_unc, n_crc, n_hours;
    int         has_realloc, has_pend, has_unc, has_crc, has_hours;
    int         hours_scaled;       /* raw was a 1 Hz counter, divided by 3600 */
    int         need_root;          /* kIOReturnNotPrivileged seen */
    int         temp;               /* -1 = unknown */
} disk_t;

typedef struct {
    int         id;
    int         flags;
    int         value;
    int         worst;
    int         thr;
    int         has_thr;
    uint64_t    raw;
} attr_t;

static disk_t disks[MAX_DISKS];
static int    ndisks;

/* ------------------------------------------------------------- props */

static CFTypeRef copy_prop(io_object_t obj, const char *key)
{
    CFStringRef k;
    CFTypeRef   v = NULL;

    k = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
    if (!k)
        return NULL;
    v = IORegistryEntryCreateCFProperty(obj, k, kCFAllocatorDefault, 0);
    CFRelease(k);
    return v;
}

static int str_prop(io_object_t obj, const char *key, char *out, size_t outsz)
{
    CFTypeRef v = copy_prop(obj, key);
    int ok = 0;

    if (v && CFGetTypeID(v) == CFStringGetTypeID())
        ok = CFStringGetCString((CFStringRef)v, out,
                                (CFIndex)outsz, kCFStringEncodingUTF8) ? 1 : 0;
    if (v)
        CFRelease(v);
    return ok;
}

static int num_prop(io_object_t obj, const char *key, uint64_t *out)
{
    CFTypeRef v = copy_prop(obj, key);
    int ok = 0;

    if (v && CFGetTypeID(v) == CFNumberGetTypeID())
        ok = CFNumberGetValue((CFNumberRef)v, kCFNumberSInt64Type, out) ? 1 : 0;
    if (v)
        CFRelease(v);
    return ok;
}

static int dict_str_prop(io_object_t obj, const char *dict, const char *key,
                         char *out, size_t outsz)
{
    CFTypeRef   d = copy_prop(obj, dict);
    int         ok = 0;

    if (d && CFGetTypeID(d) == CFDictionaryGetTypeID()) {
        CFStringRef k = CFStringCreateWithCString(kCFAllocatorDefault, key,
                                                  kCFStringEncodingUTF8);
        if (k) {
            CFTypeRef v = CFDictionaryGetValue((CFDictionaryRef)d, k);
            if (v && CFGetTypeID(v) == CFStringGetTypeID())
                ok = CFStringGetCString((CFStringRef)v, out,
                                        (CFIndex)outsz,
                                        kCFStringEncodingUTF8) ? 1 : 0;
            CFRelease(k);
        }
    }
    if (d)
        CFRelease(d);
    return ok;
}

static int dict_num_prop(io_object_t obj, const char *dict, const char *key,
                         uint64_t *out)
{
    CFTypeRef   d = copy_prop(obj, dict);
    int         ok = 0;

    if (d && CFGetTypeID(d) == CFDictionaryGetTypeID()) {
        CFStringRef k = CFStringCreateWithCString(kCFAllocatorDefault, key,
                                                  kCFStringEncodingUTF8);
        if (k) {
            CFTypeRef v = CFDictionaryGetValue((CFDictionaryRef)d, k);
            if (v && CFGetTypeID(v) == CFNumberGetTypeID())
                ok = CFNumberGetValue((CFNumberRef)v, kCFNumberSInt64Type,
                                      out) ? 1 : 0;
            CFRelease(k);
        }
    }
    if (d)
        CFRelease(d);
    return ok;
}

/* "SMART Capable" is a CFBoolean on 10.4/10.5, but accept string spellings
   as well - the registry property is not formally typed. */
static int yes_prop(io_object_t obj, const char *key)
{
    CFTypeRef v = copy_prop(obj, key);
    int r = 0;

    if (!v)
        return 0;
    if (CFGetTypeID(v) == CFBooleanGetTypeID()) {
        r = CFBooleanGetValue((CFBooleanRef)v) ? 1 : 0;
    } else if (CFGetTypeID(v) == CFStringGetTypeID()) {
        char b[16];
        if (CFStringGetCString((CFStringRef)v, b, (CFIndex)sizeof b,
                               kCFStringEncodingUTF8))
            r = (!strcasecmp(b, "yes") || !strcasecmp(b, "true") ||
                 !strcmp(b, "1")) ? 1 : 0;
    }
    CFRelease(v);
    return r;
}

static int ata_features(io_object_t obj, uint64_t *feat)
{
    if (num_prop(obj, "ATA Features", feat))
        return 1;
    return dict_num_prop(obj, kIOPropertyDeviceCharacteristicsKey,
                         "ATA Features", feat);
}

static int is_smart_node(io_object_t obj, char *why, size_t whysz)
{
    char     cls[64];
    uint64_t feat;

    if (yes_prop(obj, kIOPropertySMARTCapableKey)) {
        snprintf(why, whysz, "SMART Capable");
        return 1;
    }
    if (str_prop(obj, "IOUserClientClass", cls, sizeof cls) &&
        !strcmp(cls, "ATASMARTUserClient")) {
        snprintf(why, whysz, "ATASMARTUserClient");
        return 1;
    }
    if (IOObjectConformsTo(obj, ATA_BLOCK_CLASS) &&
        ata_features(obj, &feat) && (feat & kIOATAFeatureSMART)) {
        snprintf(why, whysz, "ATA Features 0x%x", (unsigned)feat);
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------- identity */

static void trim(char *s)
{
    size_t n = strlen(s);
    size_t lead = 0;

    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
                     s[n - 1] == '\r' || s[n - 1] == '\n'))
        s[--n] = '\0';
    while (s[lead] == ' ' || s[lead] == '\t')
        lead++;
    if (lead)
        memmove(s, s + lead, n - lead + 1);
}

static void take_identity(io_object_t obj, disk_t *d)
{
    if (!d->model[0] &&
        dict_str_prop(obj, kIOPropertyDeviceCharacteristicsKey,
                      kIOPropertyProductNameKey, d->model, sizeof d->model))
        trim(d->model);

    if (!d->serial[0] &&
        dict_str_prop(obj, kIOPropertyDeviceCharacteristicsKey,
                      kIOPropertyProductSerialNumberKey,
                      d->serial, sizeof d->serial))
        trim(d->serial);

    if (!d->fw[0] &&
        dict_str_prop(obj, kIOPropertyDeviceCharacteristicsKey,
                      kIOPropertyProductRevisionLevelKey,
                      d->fw, sizeof d->fw))
        trim(d->fw);

    if (!d->bus[0] &&
        dict_str_prop(obj, kIOPropertyProtocolCharacteristicsKey,
                      kIOPropertyPhysicalInterconnectTypeKey,
                      d->bus, sizeof d->bus))
        trim(d->bus);
}

/* ------------------------------------------------------------- SMART */

static int csum_ok(const void *buf, size_t len)
{
    const unsigned char *p = (const unsigned char *)buf;
    unsigned            sum = 0;
    size_t              i;

    for (i = 0; i < len; i++)
        sum += p[i];
    return (sum & 0xff) == 0;
}

static IOReturn read_status(IOATASMARTInterface **ifp, Boolean *exceeded)
{
    IOReturn kr = kIOReturnError;
    int      i;

    for (i = 0; i < 3; i++) {
        kr = (*ifp)->SMARTReturnStatus(ifp, exceeded);
        if (kr != kIOReturnTimeout && kr != kIOReturnNotResponding)
            break;
        usleep(200000);
    }
    return kr;
}

static IOReturn read_data(IOATASMARTInterface **ifp, ATASMARTData *sd)
{
    IOReturn kr = kIOReturnError;
    int      i;

    for (i = 0; i < 3; i++) {
        kr = (*ifp)->SMARTReadData(ifp, sd);
        if (kr != kIOReturnTimeout && kr != kIOReturnNotResponding)
            break;
        usleep(200000);
    }
    return kr;
}

static IOReturn read_thr(IOATASMARTInterface **ifp, ATASMARTDataThresholds *th)
{
    IOReturn kr = kIOReturnError;
    int      i;

    for (i = 0; i < 3; i++) {
        kr = (*ifp)->SMARTReadDataThresholds(ifp, th);
        if (kr != kIOReturnTimeout && kr != kIOReturnNotResponding)
            break;
        usleep(200000);
    }
    return kr;
}

static void parse_attrs(const ATASMARTData *sd, attr_t *out, int *nout)
{
    const unsigned char *p = (const unsigned char *)sd;
    int                  i, k;

    *nout = 0;
    for (i = 0; i < NATTR; i++) {
        const unsigned char *e = p + 2 + i * 12;
        attr_t              *a = &out[*nout];

        if (e[0] == 0)
            continue;               /* unused slot */
        a->id    = e[0];
        a->flags = e[1] | (e[2] << 8);
        a->value = e[3];
        a->worst = e[4];
        a->raw   = 0;
        for (k = 0; k < 6; k++)     /* raw is 48-bit little-endian */
            a->raw |= (uint64_t)e[5 + k] << (8 * k);
        a->thr     = 0;
        a->has_thr = 0;
        (*nout)++;
    }
}

static void parse_thr(const ATASMARTDataThresholds *th, attr_t *attrs, int n)
{
    const unsigned char *p = (const unsigned char *)th;
    int                  i, j;

    for (i = 0; i < NATTR; i++) {
        const unsigned char *e = p + 2 + i * 12;

        if (e[0] == 0)
            continue;
        for (j = 0; j < n; j++)
            if (attrs[j].id == e[0]) {
                attrs[j].thr     = e[1];
                attrs[j].has_thr = 1;
            }
    }
}

struct attrname { int id; const char *name; };

static const struct attrname attr_names[] = {
    {   1, "Raw_Read_Error_Rate"        },
    {   3, "Spin_Up_Time"               },
    {   4, "Start_Stop_Count"           },
    {   5, "Reallocated_Sector_Ct"      },
    {   7, "Seek_Error_Rate"            },
    {   9, "Power_On_Hours"             },
    {  10, "Spin_Retry_Count"           },
    {  12, "Power_Cycle_Count"          },
    {  13, "Read_Soft_Error_Rate"       },
    { 184, "End-to-End_Error"           },
    { 187, "Reported_Uncorrect"         },
    { 188, "Command_Timeout"            },
    { 189, "High_Fly_Writes"            },
    { 190, "Airflow_Temperature_Cel"    },
    { 191, "G-Sense_Error_Rate"         },
    { 192, "Power-Off_Retract_Count"    },
    { 193, "Load_Cycle_Count"           },
    { 194, "Temperature_Celsius"        },
    { 196, "Reallocation_Event_Ct"      },
    { 197, "Current_Pending_Sector"     },
    { 198, "Offline_Uncorrectable"      },
    { 199, "UDMA_CRC_Error_Count"       },
    { 200, "Multi_Zone_Error_Rate"      },
    { 201, "Soft_Read_Error_Rate"       },
    { 0,   NULL                         }
};

static const char *attr_name(int id)
{
    int i;

    for (i = 0; attr_names[i].name; i++)
        if (attr_names[i].id == id)
            return attr_names[i].name;
    return NULL;
}

static void judge(disk_t *d)
{
    attr_t a[NATTR];
    int    n, i;

    parse_attrs(&d->sd, a, &n);
    if (d->have_thr)
        parse_thr(&d->th, a, n);

    d->vd      = VD_OK;
    d->note[0] = '\0';

    if (d->exceeded) {
        d->vd = VD_FAIL;
        snprintf(d->note, sizeof d->note, "SMART status: threshold exceeded");
    }

    for (i = 0; i < n; i++) {
        attr_t  *x = &a[i];
        uint64_t v = x->raw & 0xffffffffULL;

        switch (x->id) {
        case 5:   d->n_realloc = v; d->has_realloc = 1; break;
        case 9:
            /* ATA says hours, but some drives (this Fujitsu MHV2080BH)
               publish a 1 Hz counter; 500000 h is beyond any real drive. */
            if (v > 500000ULL) {
                d->hours_scaled = 1;
                v /= 3600;
            }
            d->n_hours = v;
            d->has_hours = 1;
            break;
        case 197: d->n_pend = v; d->has_pend = 1; break;
        case 198: d->n_unc  = v; d->has_unc = 1;  break;
        case 199: d->n_crc  = v; d->has_crc = 1;  break;
        default: break;
        }
        if ((x->id == 194 || x->id == 190) && d->temp < 0)
            d->temp = (int)(x->raw & 0xff);

        if (x->id == 197 && v) {
            d->vd = VD_FAIL;
            snprintf(d->note, sizeof d->note,
                     "%" PRIu64 " pending sector%s",
                     v, v == 1 ? "" : "s");
        }
        if (x->has_thr && (x->flags & 0x0f) == 0x01 && x->value <= x->thr) {
            d->vd = VD_FAIL;
            snprintf(d->note, sizeof d->note,
                     "attr %d at threshold (%d<=%d)",
                     x->id, x->value, x->thr);
        }
        if ((x->id == 5 || x->id == 198 || x->id == 199) && v &&
            d->vd < VD_FAIL) {
            const char *nm = attr_name(x->id);

            d->vd = VD_WARN;
            snprintf(d->note, sizeof d->note, "%s = %" PRIu64,
                     nm ? nm : "raw", v);
        }
    }
}

static void err_note(IOReturn kr, char *out, size_t outsz)
{
    const char *s;

    switch (kr) {
    case kIOReturnNotPrivileged:   s = "no access (need root)";           break;
    case kIOReturnExclusiveAccess: s = "busy (opened by another client)"; break;
    case kIOReturnNoDevice:        s = "no device";                       break;
    case kIOReturnUnsupported:     s = "unsupported by this driver";      break;
    case kIOReturnTimeout:         s = "no response (spun down?)";        break;
    case kIOReturnNotResponding:   s = "no response (spun down?)";        break;
    case kIOReturnBadArgument:     s = "bad argument";                    break;
    case kIOReturnNoResources:     s = "cannot load SMART plugin "
                                       "(arch mismatch?)";                break;
    default:                       s = NULL;                              break;
    }
    if (s)
        snprintf(out, outsz, "%s", s);
    else
        snprintf(out, outsz, "SMART unavailable (0x%08x)", (unsigned)kr);
}

static void read_all(IOATASMARTInterface **ifp, disk_t *d)
{
    Boolean  exceeded = 0;
    IOReturn kr;

    kr = read_status(ifp, &exceeded);
    if (kr != kIOReturnSuccess) {
        err_note(kr, d->note, sizeof d->note);
        if (kr == kIOReturnNotPrivileged)
            d->need_root = 1;
        return;
    }
    kr = read_data(ifp, &d->sd);
    if (kr != kIOReturnSuccess) {
        err_note(kr, d->note, sizeof d->note);
        if (kr == kIOReturnNotPrivileged)
            d->need_root = 1;
        return;
    }
    if (!csum_ok(&d->sd, sizeof d->sd)) {
        snprintf(d->note, sizeof d->note, "SMART data checksum mismatch");
        return;
    }
    d->have_data = 1;
    d->exceeded  = exceeded ? 1 : 0;

    /* Thresholds are optional in ATA: a drive without them still reports
       value/worst, we just do not judge attributes without a threshold. */
    if (read_thr(ifp, &d->th) == kIOReturnSuccess &&
        csum_ok(&d->th, sizeof d->th))
        d->have_thr = 1;

    judge(d);
}

/* ---------------------------------------------------------- discovery */

static int disk_cmp(const void *pa, const void *pb)
{
    const char *a = (const char *)pa;
    const char *b = (const char *)pb;

    return atoi(a + 4) - atoi(b + 4);
}

static int scan_disks(char names[][16], int max)
{
    DIR           *dir;
    struct dirent *de;
    int            n = 0;

    dir = opendir("/dev");
    if (!dir)
        return 0;
    while ((de = readdir(dir)) != NULL && n < max) {
        const char *s = de->d_name;
        const char *p;

        if (strncmp(s, "disk", 4) != 0)
            continue;
        for (p = s + 4; *p; p++)
            if (*p < '0' || *p > '9')
                break;
        if (p == s + 4 || *p)
            continue;               /* partitions (disk0s2) and friends */
        snprintf(names[n], 16, "%s", s);
        n++;
    }
    closedir(dir);
    qsort(names, (size_t)n, 16, disk_cmp);
    return n;
}

static void probe(const char *name, disk_t *d)
{
    CFMutableDictionaryRef match;
    io_service_t           media, cur, parent, smart = 0;
    char                   why[32];

    memset(d, 0, sizeof *d);
    d->vd   = VD_NA;
    d->temp = -1;
    snprintf(d->name, sizeof d->name, "%s", name);

    match = IOBSDNameMatching(kIOMasterPortDefault, 0, name);
    media = match ? IOServiceGetMatchingService(kIOMasterPortDefault, match) : 0;
    if (!media) {
        snprintf(d->note, sizeof d->note, "no IOMedia");
        return;
    }

    num_prop(media, kIOMediaSizeKey, &d->bytes);

    cur = media;
    while (cur) {
        take_identity(cur, d);
        if (IOObjectConformsTo(cur, kIOCDBlockStorageDeviceClass))
            d->optical = 1;
        if (is_smart_node(cur, why, sizeof why)) {
            smart = cur;            /* keep retained */
            break;
        }
        parent = 0;
        if (IORegistryEntryGetParentEntry(cur, kIOServicePlane, &parent) !=
            KERN_SUCCESS)
            parent = 0;
        IOObjectRelease(cur);
        cur = parent;
    }

    if (d->optical) {
        snprintf(d->note, sizeof d->note, "optical drive, skipped");
        if (smart)
            IOObjectRelease(smart);
        return;
    }
    if (!smart) {
        if (d->bus[0])
            snprintf(d->note, sizeof d->note, "no SMART driver (%s)", d->bus);
        else
            snprintf(d->note, sizeof d->note, "no SMART driver");
        return;
    }

    {
        IOCFPlugInInterface **plugin = NULL;
        IOATASMARTInterface **ifp    = NULL;
        SInt32                score  = 0;
        IOReturn              kr;

        kr = IOCreatePlugInInterfaceForService(smart,
                                               kIOATASMARTUserClientTypeID,
                                               kIOCFPlugInInterfaceID,
                                               &plugin, &score);
        IOObjectRelease(smart);
        if (kr != kIOReturnSuccess || !plugin) {
            err_note(kr, d->note, sizeof d->note);
            if (kr == kIOReturnNotPrivileged)
                d->need_root = 1;
            return;
        }
        if ((*plugin)->QueryInterface(plugin,
                                      CFUUIDGetUUIDBytes(kIOATASMARTInterfaceID),
                                      (void **)&ifp) != S_OK || !ifp) {
            snprintf(d->note, sizeof d->note, "no SMART interface");
            IODestroyPlugInInterface(plugin);
            return;
        }
        read_all(ifp, d);
        (*ifp)->Release(ifp);
        IODestroyPlugInInterface(plugin);
    }
}

/* ----------------------------------------------------------- printing */

static void fmt_size(uint64_t b, char *out, size_t outsz)
{
    if (!b)
        snprintf(out, outsz, "-");
    else if (b >= (1ULL << 30))
        snprintf(out, outsz, "%.1fG", (double)b / 1073741824.0);
    else if (b >= (1ULL << 20))
        snprintf(out, outsz, "%.0fM", (double)b / 1048576.0);
    else
        snprintf(out, outsz, "%" PRIu64 "B", b);
}

static void fmt_num(int known, uint64_t v, char *out, size_t outsz)
{
    if (!known)
        snprintf(out, outsz, "-");
    else
        snprintf(out, outsz, "%" PRIu64, v);
}

static const char *verdict_str(int vd)
{
    switch (vd) {
    case VD_OK:   return "OK";
    case VD_WARN: return "WARN";
    case VD_FAIL: return "FAIL";
    default:      return "n/a";
    }
}

static void print_table(disk_t *ds, int n)
{
    int i;

    printf("%-6s %-26s %-10s %-6s %-7s %6s %5s %4s %4s %7s %4s  %s\n",
           "DEV", "MODEL", "FW", "SIZE", "VERDICT",
           "REALLO", "PEND", "UNC", "CRC", "HOURS", "TEMP", "NOTE");

    for (i = 0; i < n; i++) {
        disk_t *d = &ds[i];
        char    sz[16], re[16], pe[16], un[16], cr[16], hr[16], tm[8];

        fmt_size(d->bytes, sz, sizeof sz);
        if (d->have_data) {
            fmt_num(d->has_realloc, d->n_realloc, re, sizeof re);
            fmt_num(d->has_pend,    d->n_pend,    pe, sizeof pe);
            fmt_num(d->has_unc,     d->n_unc,     un, sizeof un);
            fmt_num(d->has_crc,     d->n_crc,     cr, sizeof cr);
            fmt_num(d->has_hours,   d->n_hours,   hr, sizeof hr);
            if (d->temp >= 0)
                snprintf(tm, sizeof tm, "%d", d->temp);
            else
                snprintf(tm, sizeof tm, "-");
        } else {
            snprintf(re, sizeof re, "-");
            snprintf(pe, sizeof pe, "-");
            snprintf(un, sizeof un, "-");
            snprintf(cr, sizeof cr, "-");
            snprintf(hr, sizeof hr, "-");
            snprintf(tm, sizeof tm, "-");
        }

        printf("%-6s %-26.26s %-10.10s %-6s %-7s %6s %5s %4s %4s %7s %4s  %s\n",
               d->name,
               d->model[0] ? d->model : "-",
               d->fw[0] ? d->fw : "-",
               sz,
               verdict_str(d->vd),
               re, pe, un, cr, hr, tm,
               d->note);
    }
}

static void dump_disk(disk_t *d)
{
    attr_t a[NATTR];
    int    n, i;
    char   sz[16];

    fmt_size(d->bytes, sz, sizeof sz);
    printf("\n%s: %s   S/N %s   FW %s   %s   %s\n",
           d->name,
           d->model[0] ? d->model : "-",
           d->serial[0] ? d->serial : "-",
           d->fw[0] ? d->fw : "-",
           d->bus[0] ? d->bus : "-",
           sz);

    if (!d->have_data) {
        printf("  SMART: %s\n", d->note);
        return;
    }

    if (d->note[0])
        printf("  SMART: %s (%s)\n", verdict_str(d->vd), d->note);
    else
        printf("  SMART: %s\n", verdict_str(d->vd));
    printf("  off-line data collection: 0x%02x   last self-test: 0x%02x   thresholds: %s\n",
           d->sd.offLineDataCollectionStatus,
           d->sd.selfTestExecutionStatus,
           d->have_thr ? "yes" : "no");
    printf("  %3s  %-28s %6s %4s %4s %4s  %s\n",
           "ID", "NAME", "FLAGS", "VAL", "WOR", "THR", "RAW");

    parse_attrs(&d->sd, a, &n);
    if (d->have_thr)
        parse_thr(&d->th, a, n);
    for (i = 0; i < n; i++) {
        const char *nm = attr_name(a[i].id);
        char        thr[8];

        if (a[i].has_thr)
            snprintf(thr, sizeof thr, "%d", a[i].thr);
        else
            snprintf(thr, sizeof thr, "?");
        printf("  %3d  %-28.28s 0x%04x %4d %4d %4s  %" PRIu64 "\n",
               a[i].id, nm ? nm : "Vendor_Specific",
               a[i].flags, a[i].value, a[i].worst, thr,
               a[i].raw & 0xffffffffULL);
    }
    printf("  flags: 0x0f/0x03 = pre-fail, 0x32/0x33 = old-age\n");
    printf("  RAW = low 32 bits of the 48-bit field (upper bytes are vendor\n"
           "  filler on some drives); names are conventional (ATA/SFF-8035i)\n");
    if (d->hours_scaled)
        printf("  note: power-on counter is a 1 Hz counter on this drive, "
               "shown as %" PRIu64 " h\n", d->n_hours);
}

/* --------------------------------------------------------------- main */

static void usage(const char *argv0)
{
    printf("usage: %s [-a] [-d diskN] [-h]\n"
           "  -a          full attribute dump for every disk\n"
           "  -d diskN    only this disk (e.g. disk0)\n"
           "  -h          this help\n"
           "\n"
           "Read-only: no SMART enable/disable, no self-tests, no log writes.\n"
           "SMART reads require root.\n", argv0);
}

int main(int argc, char **argv)
{
    char        names[MAX_DISKS][16];
    const char *only = NULL;
    int         all = 0;
    int         n, i, arg;
    int         n_ok = 0, n_warn = 0, n_fail = 0, n_na = 0;

    while ((arg = getopt(argc, argv, "ad:h")) != -1) {
        switch (arg) {
        case 'a':
            all = 1;
            break;
        case 'd':
            only = optarg;
            break;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 3;
        }
    }

    n = scan_disks(names, MAX_DISKS);
    if (!n) {
        fprintf(stderr, "smartinfo: no /dev/diskN devices found\n");
        return 2;
    }

    ndisks = 0;
    for (i = 0; i < n; i++) {
        if (only && strcmp(only, names[i]))
            continue;
        probe(names[i], &disks[ndisks]);
        ndisks++;
    }
    if (only && !ndisks) {
        fprintf(stderr, "smartinfo: %s not found\n", only);
        return 3;
    }

    for (i = 0; i < ndisks; i++)
        if (disks[i].need_root)
            break;
    if (i < ndisks && geteuid() != 0)
        printf("note: SMART reads were refused - try: sudo %s\n\n", argv[0]);

    print_table(disks, ndisks);

    for (i = 0; i < ndisks; i++) {
        switch (disks[i].vd) {
        case VD_OK:   n_ok++;   break;
        case VD_WARN: n_warn++; break;
        case VD_FAIL: n_fail++; break;
        default:      n_na++;   break;
        }
    }
    printf("\n%d disk%s: %d OK, %d WARN, %d FAIL, %d n/a\n",
           ndisks, ndisks == 1 ? "" : "s", n_ok, n_warn, n_fail, n_na);

    if (all)
        for (i = 0; i < ndisks; i++)
            dump_disk(&disks[i]);

    if (n_fail || n_warn)
        return 1;
    if (!n_ok)
        return 2;
    return 0;
}
