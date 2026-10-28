/*
 * dgdev.c - общее для ядер замеров и UI (см. dgdev.h)
 */
#include "dgdev.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/disk.h>
#include <mach/mach_time.h>

/* ------------------------------------------------------------ clock */
static double g_ns_per_tick = 0.0;

void dg_clock_init(void)
{
    mach_timebase_info_data_t tb;

    if (g_ns_per_tick > 0.0)
        return;                       /* гонка безобидна: число то же */
    mach_timebase_info(&tb);
    g_ns_per_tick = (double)tb.numer / (double)tb.denom;
}

uint64_t dg_now_us(void)
{
    return (uint64_t)((double)mach_absolute_time() * g_ns_per_tick / 1000.0);
}

/* ------------------------------------------------------------ device */

static int dev_capacity(int fd, uint64_t *cap)
{
    uint32_t bs = 512;
    uint64_t n = 0;

    if (ioctl(fd, DKIOCGETBLOCKSIZE, &bs) < 0)
        bs = 512;
    if (ioctl(fd, DKIOCGETBLOCKCOUNT, &n) < 0)
        return -1;
    *cap = n * (uint64_t)bs;
    return 0;
}

int dg_open_raw(const char *dev, uint64_t *cap, int *oserr,
                char *errbuf, size_t errlen)
{
    int fd;

    if (oserr != NULL)
        *oserr = 0;

    fd = open(dev, O_RDONLY);
    if (fd < 0) {
        if (oserr != NULL)
            *oserr = errno;
        snprintf(errbuf, errlen, "cannot open %s: %s",
                 dev, strerror(errno));
        return -1;
    }

    if (dev_capacity(fd, cap) < 0) {
        if (oserr != NULL)
            *oserr = errno;
        snprintf(errbuf, errlen, "ioctl(DKIOCGETBLOCKCOUNT) on %s: %s",
                 dev, strerror(errno));
        goto fail;
    }
    if (*cap == 0) {
        snprintf(errbuf, errlen, "zero capacity reported by %s", dev);
        goto fail;
    }

    if (fcntl(fd, F_NOCACHE, 1) < 0) {
        if (oserr != NULL)
            *oserr = errno;
        snprintf(errbuf, errlen, "fcntl(F_NOCACHE) on %s: %s",
                 dev, strerror(errno));
        goto fail;
    }
    if (fcntl(fd, F_RDAHEAD, 0) < 0) {
        if (oserr != NULL)
            *oserr = errno;
        snprintf(errbuf, errlen, "fcntl(F_RDAHEAD) on %s: %s",
                 dev, strerror(errno));
        goto fail;
    }
    return fd;

fail:
    close(fd);
    return -1;
}

double dg_probe_capacity(const char *dev)
{
    int      fd;
    uint64_t cap = 0;

    fd = open(dev, O_RDONLY);
    if (fd < 0)
        return 0.0;
    if (dev_capacity(fd, &cap) < 0)
        cap = 0;
    close(fd);
    return (double)cap;
}

int dg_is_perm_err(int oserr)
{
    return oserr == EACCES || oserr == EPERM;
}

/* ------------------------------------------------------------ stats */

int dg_dcmp(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

double dg_pct(const double *v, size_t n, double p)
{
    size_t i;

    if (n == 0)
        return 0.0;
    i = (size_t)(p * (double)(n - 1) + 0.5);
    if (i >= n)
        i = n - 1;
    return v[i];
}
