/*
 * tmpprobe.c - bare CLI probe of AppleSMC sensor keys on the local
 * machine, feeding any tray gauge (same counters, no AppKit).
 *
 * Walks the SMC key space by index (SMC_CMD_READ_INDEX), prints
 * every key with its type and a decoded value for the common
 * sensor encodings (sp78 temperatures, fpe2 rpm/watts, uint8,
 * float).  Recon tool: permute enough to catch "TC0D"-family core
 * temps on whichever Mac we sit.
 *
 * SMCUserClient private ABI (struct layout + commands) is the
 * de-facto standard published by devnull's smc tool (2006) and
 * smcFanControl; re-implemented here from those reference notes.
 *
 * Build natively (leopard):
 *   gcc -std=gnu99 -O1 -Wall tmpprobe.c -o tmpprobe \
 *       -framework CoreFoundation -framework IOKit
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char major;
    char minor;
    char build;
    char reserved[1];
    unsigned short release;
} SMC_vers_t;

typedef struct {
    unsigned short version;
    unsigned short length;
    unsigned int   cpuPLimit;
    unsigned int   gpuPLimit;
    unsigned int   memPLimit;
} SMC_plimit_t;

typedef struct {
    unsigned int  dataSize;
    unsigned int  dataType;
    char          dataAttributes;
} SMC_keyinfo_t;

typedef unsigned char SMCBytes_t[32];

typedef struct {
    unsigned int   key;
    SMC_vers_t     vers;
    SMC_plimit_t   pLimitData;
    SMC_keyinfo_t  keyInfo;
    char           result;
    char           status;
    char           data8;
    unsigned int   data32;
    SMCBytes_t     bytes;
} SMCKeyData_t;

/* de-facto AppleSMC user client command set */
#define KERNCMD_SMC       2
#define SMC_CMD_READ_BYTES 5
#define SMC_CMD_READ_INDEX 8
#define SMC_CMD_READ_KEYINFO 9

static io_connect_t g_conn = 0;

/* four-char key string <-> uint32 */
static unsigned int key_code(const char *s)
{
    unsigned int v = 0;
    int i;
    for (i = 0; i < 4; i++)
        v = (v << 8) | (unsigned char)(s[i] & 0xff);
    return v;
}

static void key_str(unsigned int c, char out[5])
{
    out[0] = (char)((c >> 24) & 0xff);
    out[1] = (char)((c >> 16) & 0xff);
    out[2] = (char)((c >> 8) & 0xff);
    out[3] = (char)(c & 0xff);
    out[4] = 0;
}

static kern_return_t smc_call(int cmd, SMCKeyData_t *in, SMCKeyData_t *out)
{
    size_t in_size = sizeof(SMCKeyData_t);
    size_t out_size = sizeof(SMCKeyData_t);
    return IOConnectCallStructMethod(g_conn, cmd, in, in_size,
                                     out, &out_size);
}

static int smc_open(void)
{
    CFMutableDictionaryRef match;
    io_iterator_t          it = 0;
    io_object_t            dev;
    kern_return_t          kr;

    match = IOServiceMatching("AppleSMC");
    if (match == NULL)
        return 1;
    kr = IOServiceGetMatchingServices(kIOMasterPortDefault, match, &it);
    if (kr != KERN_SUCCESS)
        return 1;
    dev = IOIteratorNext(it);
    IOObjectRelease(it);
    if (dev == 0)
        return 1;
    kr = IOServiceOpen(dev, mach_task_self(), 0, &g_conn);
    IOObjectRelease(dev);
    return (kr == KERN_SUCCESS) ? 0 : 1;
}

static void type_str(unsigned int t, char out[5])
{
    out[0] = (char)((t >> 24) & 0xff);
    out[1] = (char)((t >> 16) & 0xff);
    out[2] = (char)((t >> 8) & 0xff);
    out[3] = (char)(t & 0xff);
    out[4] = 0;
}

/* read a key by 4-char code; fills val; returns kern result */
static kern_return_t read_key(const char *keyc, unsigned char *bytes,
                              unsigned int *size, char tstr[5])
{
    SMCKeyData_t in, out;
    kern_return_t kr;

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    in.key = key_code(keyc);
    in.data8 = SMC_CMD_READ_KEYINFO;
    kr = smc_call(KERNCMD_SMC, &in, &out);
    if (kr != KERN_SUCCESS)
        return kr;
    *size = out.keyInfo.dataSize;
    type_str(out.keyInfo.dataType, tstr);

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    in.key = key_code(keyc);
    in.keyInfo.dataSize = *size;
    in.data8 = SMC_CMD_READ_BYTES;
    kr = smc_call(KERNCMD_SMC, &in, &out);
    if (kr != KERN_SUCCESS)
        return kr;
    memcpy(bytes, out.bytes, sizeof(out.bytes));
    return KERN_SUCCESS;
}

/* iterate SMC keys by index; index_cmd syntax: in.data32 = index */
static kern_return_t key_at_index(int idx, char keyc[5])
{
    SMCKeyData_t in, out;
    kern_return_t kr;

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    in.data8 = SMC_CMD_READ_INDEX;
    in.data32 = idx;
    kr = smc_call(KERNCMD_SMC, &in, &out);
    if (kr != KERN_SUCCESS)
        return kr;
    key_str(out.key, keyc);
    return KERN_SUCCESS;
}

static double decode_value(const char *t, const unsigned char *b,
                           unsigned int size)
{
    /* sp78: signed 7.8 fixed -> degrees C; classic decode */
    if (size == 2 && strncmp(t, "sp78", 4) == 0)
        return ((b[0] * 256 + b[1]) >> 2) / 64.0;
    if (size == 4 && strncmp(t, "flt ", 4) == 0) {
        float f;
        memcpy(&f, b, 4);
        return (double)f;
    }
    if (size == 2 && strncmp(t, "fpe2", 4) == 0)
        return (b[0] * 256 + b[1]) / 4.0;
    if (size == 1)
        return (double)b[0];
    /* unknown encodings: print first bytes raw in hex below */
    return -1;
}

int main(int argc, char **argv)
{
    int i;
    int args = (argc > 1) ? atoi(argv[1]) : 260;   /* key count ceiling */

    if (smc_open() != 0) {
        printf("tmpprobe: no AppleSMC or open failed\n");
        return 1;
    }

    for (i = 0; i < args; i++) {
        char kc[5];
        unsigned char bytes[32];
        unsigned int size = 0;
        char tstr[5] = "----";
        kern_return_t kr = key_at_index(i, kc);
        if (kr != KERN_SUCCESS)
            break;                                   /* end of key space */
        kr = read_key(kc, bytes, &size, tstr);
        if (kr == KERN_SUCCESS && size > 0) {
            char (*p)[5] = NULL;
            double v = decode_value(tstr, bytes, size);
            if (v >= 0)
                printf("%3d %-4s [%s] %9.2f   hex:", i, kc, tstr, v);
            else
                printf("%3d %-4s [%s]  unknown   hex:", i, kc, tstr);
            { int j; for (j = 0; j < size && j < 6; j++)
                printf(" %02x", bytes[j]); }
            printf("\n");
            (void)p;
        }
    }
    return 0;
}
