/*
 * smartcore.c - read-only SMART probe as a FUNCTION (Mac OS X 10.2 - 10.5)
 *
 * Shared by the console smartinfo and the DiskGraph UI. Moved out of
 * smartinfo.c without changing the logic; no stdout in here.
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
 * so the same code reaches PATA (ATASMARTUserClient), G5 K2 SATA
 * (AppleSATLSMARTUserClient) and AHCI (AHCISMARTUserClient).
 *
 * Version gate: GetATAIdentifyData is the last vtable entry and was added
 * in 10.3 - it is called only on Darwin 7+ to fill fields the registry did
 * not provide. That keeps the 10.2 ABI untouched.
 */
#include "smartcore.h"

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

/* ATA_BLOCK_CLASS was dropped from the 10.5 SDK headers
   (the class itself is still there); the registry class name is stable. */
#define ATA_BLOCK_CLASS "IOATABlockStorageDevice"

/* not in the 10.4 /usr/include headers (Tiger native build), the registry
   key is the same on every release */
#ifndef kIOPropertyProductSerialNumberKey
#define kIOPropertyProductSerialNumberKey "Serial Number"
#endif

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

static void take_identity(io_object_t obj, sm_disk *d)
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

static int os_major(void)
{
    int    mib[2];
    char   buf[64];
    size_t len = sizeof buf;

    mib[0] = CTL_KERN;
    mib[1] = KERN_OSRELEASE;
    if (sysctl(mib, 2, buf, &len, NULL, 0) != 0)
        return -1;
    return atoi(buf);
}

/* ATA IDENTIFY strings: each 16-bit word holds two chars, high byte first.
   Apple returns the words in native order, so read the word natively and
   split it - that is correct on both G3/G4/G5 and i386. */
static void ata_id_str(const unsigned char *buf, int first, int last,
                       char *out, size_t outsz)
{
    size_t n = 0;
    int    i;

    out[0] = '\0';
    for (i = first; i <= last && n + 1 < outsz; i++) {
        uint16_t w;
        char     c1, c2;

        memcpy(&w, buf + i * 2, 2);
        c1 = (char)(w >> 8);
        c2 = (char)(w & 0xff);
        if (c1 == 0)
            break;
        out[n++] = c1;
        if (c2 == 0 || n + 1 >= outsz)
            break;
        out[n++] = c2;
    }
    out[n] = '\0';
    trim(out);
}

/* GetATAIdentifyData is the last vtable entry and does not exist on 10.2,
   so it is called only on Darwin 7+ (Panther 10.3 and later) and only to
   fill fields the registry did not provide (e.g. PATA on Tiger publishes
   Product Name/FW but no Serial Number). */
static int retryable(IOReturn kr);

static void identify_fill(IOATASMARTInterface **ifp, sm_disk *d)
{
    unsigned char buf[512];
    UInt32        out = 0;
    char          tmp[48];
    IOReturn      kr;
    int           i;

    memset(buf, 0, sizeof buf);
    for (i = 0; i < 4; i++) {
        kr = (*ifp)->GetATAIdentifyData(ifp, buf, sizeof buf, &out);
        if (!retryable(kr))
            break;
        usleep(500000);
    }
    if (kr != kIOReturnSuccess)
        return;

    if (!d->model[0]) {
        ata_id_str(buf, 27, 46, tmp, sizeof tmp);
        if (tmp[0])
            snprintf(d->model, sizeof d->model, "%s", tmp);
    }
    if (!d->serial[0]) {
        ata_id_str(buf, 10, 19, tmp, sizeof tmp);
        if (tmp[0])
            snprintf(d->serial, sizeof d->serial, "%s", tmp);
    }
    if (!d->fw[0]) {
        ata_id_str(buf, 23, 26, tmp, sizeof tmp);
        if (tmp[0])
            snprintf(d->fw, sizeof d->fw, "%s", tmp);
    }
}

static int csum_ok(const void *buf, size_t len)
{
    const unsigned char *p = (const unsigned char *)buf;
    unsigned            sum = 0;
    size_t              i;

    for (i = 0; i < len; i++)
        sum += p[i];
    return (sum & 0xff) == 0;
}

/* A spun-down drive answers NotReady/Timeout for a second or two; retry
   long enough to let it come up (worst case 6 x 500 ms). */
static int retryable(IOReturn kr)
{
    return kr == kIOReturnTimeout || kr == kIOReturnNotResponding ||
           kr == kIOReturnNotReady;
}

static IOReturn read_status(IOATASMARTInterface **ifp, Boolean *exceeded)
{
    IOReturn kr = kIOReturnError;
    int      i;

    for (i = 0; i < 6; i++) {
        kr = (*ifp)->SMARTReturnStatus(ifp, exceeded);
        if (!retryable(kr))
            break;
        usleep(500000);
    }
    return kr;
}

static IOReturn read_data(IOATASMARTInterface **ifp, ATASMARTData *sd)
{
    IOReturn kr = kIOReturnError;
    int      i;

    for (i = 0; i < 6; i++) {
        kr = (*ifp)->SMARTReadData(ifp, sd);
        if (!retryable(kr))
            break;
        usleep(500000);
    }
    return kr;
}

static IOReturn read_thr(IOATASMARTInterface **ifp, ATASMARTDataThresholds *th)
{
    IOReturn kr = kIOReturnError;
    int      i;

    for (i = 0; i < 6; i++) {
        kr = (*ifp)->SMARTReadDataThresholds(ifp, th);
        if (!retryable(kr))
            break;
        usleep(500000);
    }
    return kr;
}

static void parse_attrs(const ATASMARTData *sd, sm_attr *out, int *nout)
{
    const unsigned char *p = (const unsigned char *)sd;
    int                  i, k;

    *nout = 0;
    for (i = 0; i < NATTR; i++) {
        const unsigned char *e = p + 2 + i * 12;
        sm_attr              *a = &out[*nout];

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
        a->level   = SM_LV_OK;
        (*nout)++;
    }
}

static void parse_thr(const ATASMARTDataThresholds *th, sm_attr *attrs, int n)
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

const char *sm_attr_name(int id)
{
    int i;

    for (i = 0; attr_names[i].name; i++)
        if (attr_names[i].id == id)
            return attr_names[i].name;
    return NULL;
}

static void judge(sm_disk *d, const ATASMARTData *sd,
                  const ATASMARTDataThresholds *th)
{
    sm_attr *a = d->attrs;
    int      n, i;

    parse_attrs(sd, a, &n);
    if (th != NULL)
        parse_thr(th, a, n);
    d->nattr          = n;
    d->offline_status = sd->offLineDataCollectionStatus;
    d->selftest_status = sd->selfTestExecutionStatus;

    d->vd      = VD_OK;
    d->note[0] = '\0';

    if (d->exceeded) {
        d->vd = VD_FAIL;
        snprintf(d->note, sizeof d->note, "SMART status: threshold exceeded");
    }

    for (i = 0; i < n; i++) {
        sm_attr  *x = &a[i];
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
            x->level = SM_LV_FAIL;
            d->vd = VD_FAIL;
            snprintf(d->note, sizeof d->note,
                     "%" PRIu64 " pending sector%s",
                     v, v == 1 ? "" : "s");
        }
        if (x->has_thr && (x->flags & 0x0f) == 0x01 && x->value <= x->thr) {
            x->level = SM_LV_FAIL;
            d->vd = VD_FAIL;
            snprintf(d->note, sizeof d->note,
                     "attr %d at threshold (%d<=%d)",
                     x->id, x->value, x->thr);
        }
        if ((x->id == 5 || x->id == 198 || x->id == 199) && v &&
            x->level < SM_LV_WARN)
            x->level = SM_LV_WARN;
        if ((x->id == 5 || x->id == 198 || x->id == 199) && v &&
            d->vd < VD_FAIL) {
            const char *nm = sm_attr_name(x->id);

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
    case kIOReturnNotReady:        s = "not ready (spinning up?)";         break;
    case kIOReturnNoResources:     s = "cannot load SMART plugin "
                                       "(arch mismatch?)";                break;
    default:                       s = NULL;                              break;
    }
    if (s)
        snprintf(out, outsz, "%s", s);
    else
        snprintf(out, outsz, "SMART unavailable (0x%08x)", (unsigned)kr);
}

static void read_all(IOATASMARTInterface **ifp, sm_disk *d)
{
    ATASMARTData           sd;
    ATASMARTDataThresholds th;
    Boolean  exceeded = 0;
    IOReturn kr;

    kr = read_status(ifp, &exceeded);
    if (kr != kIOReturnSuccess) {
        err_note(kr, d->note, sizeof d->note);
        if (kr == kIOReturnNotPrivileged)
            d->need_root = 1;
        return;
    }
    kr = read_data(ifp, &sd);
    if (kr != kIOReturnSuccess) {
        err_note(kr, d->note, sizeof d->note);
        if (kr == kIOReturnNotPrivileged)
            d->need_root = 1;
        return;
    }
    if (!csum_ok(&sd, sizeof sd)) {
        snprintf(d->note, sizeof d->note, "SMART data checksum mismatch");
        return;
    }
    d->have_data = 1;
    d->exceeded  = exceeded ? 1 : 0;

    /* Thresholds are optional in ATA: a drive without them still reports
       value/worst, we just do not judge attributes without a threshold. */
    if (read_thr(ifp, &th) == kIOReturnSuccess &&
        csum_ok(&th, sizeof th))
        d->have_thr = 1;

    judge(d, &sd, d->have_thr ? &th : NULL);
}

/* ---------------------------------------------------------- discovery */

static int disk_cmp(const void *pa, const void *pb)
{
    const char *a = (const char *)pa;
    const char *b = (const char *)pb;

    return atoi(a + 4) - atoi(b + 4);
}

int sm_scan(char names[][16], int max)
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

void sm_probe(const char *name, sm_disk *d)
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
        if (os_major() >= 7)        /* 10.3+: GetATAIdentifyData exists */
            identify_fill(ifp, d);
        read_all(ifp, d);
        (*ifp)->Release(ifp);
        IODestroyPlugInInterface(plugin);
    }
}

/* ---------------------------------------------------------- verdict */

const char *sm_verdict_str(int vd)
{
    switch (vd) {
    case VD_OK:   return "OK";
    case VD_WARN: return "WARN";
    case VD_FAIL: return "FAIL";
    default:      return "n/a";
    }
}
