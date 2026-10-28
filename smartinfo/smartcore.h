/*
 * smartcore.h - read-only SMART probe as a FUNCTION (Mac OS X 10.2 - 10.5)
 *
 * One probe for two front ends:
 *   - smartinfo.c : console table / -a dump;
 *   - DiskGraph   : SMART tab (table + verdict banner).
 * Plain C, no IOKit types in this header - the raw 512-byte SMART
 * structures stay inside smartcore.c, callers get parsed attributes.
 *
 * Read-only: SMARTReturnStatus, SMARTReadData, SMARTReadDataThresholds
 * (+ GetATAIdentifyData on 10.3+). Nothing that changes drive state.
 */
#ifndef SMARTCORE_H
#define SMARTCORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NATTR 30

enum { VD_NA = 0, VD_OK, VD_WARN, VD_FAIL };

/* per-attribute level, set by the same rules as the disk verdict */
enum { SM_LV_OK = 0, SM_LV_WARN = 1, SM_LV_FAIL = 2 };

typedef struct {
    int         id;
    int         flags;
    int         value;
    int         worst;
    int         thr;
    int         has_thr;
    uint64_t    raw;                /* full 48-bit field; print low 32 bits */
    int         level;              /* SM_LV_* */
} sm_attr;

typedef struct {
    char        name[16];           /* "disk0" */
    char        model[44];
    char        serial[40];
    char        fw[24];
    char        bus[24];
    uint64_t    bytes;
    int         optical;
    int         vd;                 /* verdict VD_* */
    char        note[80];
    int         have_data;
    int         have_thr;
    int         exceeded;
    /* snapshot of the counters reported in the summary table */
    uint64_t    n_realloc, n_pend, n_unc, n_crc, n_hours;
    int         has_realloc, has_pend, has_unc, has_crc, has_hours;
    int         hours_scaled;       /* raw was a 1 Hz counter, divided by 3600 */
    int         need_root;          /* kIOReturnNotPrivileged seen */
    int         temp;               /* -1 = unknown */
    /* parsed SMART data (valid when have_data) */
    sm_attr     attrs[NATTR];
    int         nattr;
    int         offline_status;     /* off-line data collection status byte */
    int         selftest_status;    /* self-test execution status byte */
} sm_disk;

/* whole disks /dev/diskN, sorted by number; names[i] = "disk0" */
int         sm_scan(char names[][16], int max);

/* identity + SMART of one disk ("disk0"); never fails, see d->vd/d->note */
void        sm_probe(const char *name, sm_disk *d);

/* conventional attribute name (ATA/SFF-8035i) or NULL */
const char *sm_attr_name(int id);

/* "OK" / "WARN" / "FAIL" / "n/a" */
const char *sm_verdict_str(int vd);

#ifdef __cplusplus
}
#endif

#endif /* SMARTCORE_H */
