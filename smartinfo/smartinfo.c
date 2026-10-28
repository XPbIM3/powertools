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
 * The probe itself lives in smartcore.c (shared with DiskGraph.app);
 * this file is the console front end: table, -a dump, exit codes.
 *
 * Usage: smartinfo [-a] [-d diskN] [-h]
 */
#include "smartcore.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>

#define MAX_DISKS   32

static sm_disk disks[MAX_DISKS];
static int     ndisks;

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

static void print_table(sm_disk *ds, int n)
{
    int i;

    printf("%-6s %-26s %-10s %-6s %-7s %6s %5s %4s %4s %7s %4s  %s\n",
           "DEV", "MODEL", "FW", "SIZE", "VERDICT",
           "REALLO", "PEND", "UNC", "CRC", "HOURS", "TEMP", "NOTE");

    for (i = 0; i < n; i++) {
        sm_disk *d = &ds[i];
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
               sm_verdict_str(d->vd),
               re, pe, un, cr, hr, tm,
               d->note);
    }
}

static void dump_disk(sm_disk *d)
{
    const sm_attr *a = d->attrs;
    int    n = d->nattr, i;
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
        printf("  SMART: %s (%s)\n", sm_verdict_str(d->vd), d->note);
    else
        printf("  SMART: %s\n", sm_verdict_str(d->vd));
    printf("  off-line data collection: 0x%02x   last self-test: 0x%02x   thresholds: %s\n",
           d->offline_status,
           d->selftest_status,
           d->have_thr ? "yes" : "no");
    printf("  %3s  %-28s %6s %4s %4s %4s  %s\n",
           "ID", "NAME", "FLAGS", "VAL", "WOR", "THR", "RAW");

    for (i = 0; i < n; i++) {
        const char *nm = sm_attr_name(a[i].id);
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

    n = sm_scan(names, MAX_DISKS);
    if (!n) {
        fprintf(stderr, "smartinfo: no /dev/diskN devices found\n");
        return 2;
    }

    ndisks = 0;
    for (i = 0; i < n; i++) {
        if (only && strcmp(only, names[i]))
            continue;
        sm_probe(names[i], &disks[ndisks]);
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
