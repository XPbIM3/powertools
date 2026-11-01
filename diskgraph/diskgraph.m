/*
 * diskgraph.m - UI DiskGraph: вкладки Sequential read / Access map /
 *               Seek profile на РЕАЛЬНЫХ замерах через dglincore/dgrndcore
 *               + вкладка SMART на ядре ../smartinfo/smartcore (только
 *               чтение: статус, атрибуты, пороги)
 *
 * Фронт и бэк скрещены: вкладки зовут те же ядра, что и консольные
 * dblin/dbrnd, но данные приходят не в stdout, а в sink-калбеки:
 *   - Sequential read : сегмент -> точка (позиция, MB/s), кривая;
 *   - Access map      : оп -> точка (позиция на диске, лаг), позиции
 *                       равномерно по диску - карта времени доступа по
 *                       поверхности (зона, где сбоит позиционер или
 *                       поверхность, приподнята);
 *   - Seek profile    : хоп -> точка (дистанция хопа, лаг) на лог-оси X,
 *                       дистанции log-равномерно 8K..ёмкость (профиль
 *                       перемещения головки).
 * Ошибки чтения (EIO) - красные крестики на позиции (Sequential, Access).
 * Обе random-вкладки - один код (rndtab), разный режим ядра.
 *
 * Поток: UI-поток (AppKit) + один рабочий поток на запуск (pthread).
 * Рабочий поток только дописывает точки в массивы под мьютексом и
 * никогда не трогает ObjC. UI раз в 0.25 c снимает копию данных под
 * мьютексом и рисует уже без него - замер не ждёт отрисовку.
 * Одновременно идёт один прогон.
 *
 * Выбор диска - глобальный, в шапке окна над вкладками (на вкладках
 * своих селекторов нет, замер всегда /dev/rdiskN выбранного диска).
 * Данные по-дисковые: devdata с именем "diskN" хранит результаты всех
 * трёх замеров; выбрали диск - вкладки показывают его данные, у диска
 * без замеров вкладки пусты; вернулись - данные не забыты.
 *
 * Права: чтение /dev/rdiskN требует root. При запуске приложение само
 * перезапускает себя через AuthorizationExecuteWithPrivileges (пароль
 * спрашивает SecurityAgent). Без прав работает и честно об этом пишет.
 *
 * Только ЧТЕНИЕ. Запись не выполняется никогда.
 *
 * ObjC строго objc1 (Jaguar..Leopard runtime): без свойств и fast
 * enumeration; @"..."-литералы только ASCII; только API 10.2.
 */
#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#include <dirent.h>
#include <sys/types.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <math.h>
#include <mach-o/dyld.h>
#include <Security/Authorization.h>
#include <Security/AuthorizationTags.h>

#include "dglincore.h"
#include "dgrndcore.h"
#include "dgdev.h"
#include "smartcore.h"

/* ---- параметры приборов (как в консольных тулзах) ---- */
#define LIN_SEG (32ULL << 20)      /* 32 MiB чанк  */
#define LIN_BLK (1ULL  << 20)      /* 1 MiB блок   */
#define RND_BLK 4096ULL            /* 4 KiB        */
#define RND_WARMUP 16
#define TICK_SEC 0.25

#define WIN_W 1000.0f
#define WIN_H  750.0f

#define GIB 1073741824.0
#define SEEK_XMIN 4096.0           /* левый край лог-оси хопов */
#define NBIN 24                    /* корзин по дистанции       */

/* g_run: какой прогон идёт */
enum { RUN_IDLE = 0, RUN_LIN = 1, RUN_ACC = 2, RUN_SEEK = 3 };

/* ------------------------------------------------------------ данные */

typedef struct {
    double x;     /* linear: позиция; random: дистанция, байты */
    double val;   /* MB/s (linear) / µs (random)               */
    int    tag;   /* random: +1 вперёд, -1 назад                */
} dpt;

/* растущий массив точек: ёмкость удваивается, без realloc на точку */
typedef struct {
    dpt *a;
    int  n;
    int  alloc;
} dbuf;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static int g_run = RUN_IDLE;            /* UI-поток */
static time_t g_run_t0 = 0;             /* UI-поток */

/* ---- данные по диску: стейт-машина «выбранный диск» ----
 *
 * Результаты замеров живут в devdata диска ("diskN"), а не в вкладках.
 * Выбор в шапке переключает, какие данные показывают вкладки; при
 * возврате к диску показывается то, что осталось от прошлых прогонов.
 *
 * Данные ядер замеров - под g_lock (кроме помеченного); вкладки,
 * шапка и записи дисков - только UI-поток.
 */

typedef struct {                        /* Sequential read              */
    dbuf        pts, errs;              /* EIO: x = позиция блока       */
    volatile int cancel;
    int         done, failed, oserr, elapsed, idx_done, duty;
    char        err[256], path[64];     /* path - нода снимка на Старт  */
    dglinresult res;
    uint64_t    total, bytes, time_us;
} dlin;

typedef struct {                        /* Access map / Seek profile    */
    dbuf        pts, errs;
    volatile int cancel;
    int         done, failed, oserr, elapsed;
    char        err[256];
    dgrndresult res;
    double      med, p99;
    double      short_med, long_med;    /* seek: <1 MB и > 1/2 диска    */
    int         stat_n;                 /* по скольким точкам посчитано */
    int         bn[NBIN];
    double      bmed[NBIN], blo[NBIN];
} drnd;

typedef struct {
    char        name[16];               /* "disk0" - ключ               */
    int         used;
    double      cap;                    /* ёмкость, байты; 0 - неизвестна */
    dlin        lin;
    drnd        acc, seek;
} devdata;

#define DD_N 16                  /* потолок дисков (равен SM_MAX smartcore) */

/* смарт-опросник - список дисков (определение - в блоке SMART ниже) */
static sm_disk g_sm_ui[DD_N];           /* UI-поток */
static int     g_sm_ui_n = 0;
static int     g_sm_ui_gen = 0;         /* номер поколения в UI */

static devdata g_dd[DD_N];              /* память по дискам             */
static devdata *g_cur = NULL;           /* UI-поток: выбранный диск     */
static char     g_dd_sel[16] = "disk0"; /* имя выбранного диска         */

static int    g_duty = 5;               /* duty Sequential read, %      */

/* найти или завести запись по имени ("disk0"). Только UI-поток:
 * записи заводятся в тике по поколению смарт-опросника и dev-хуком,
 * рабочие потоки пишут только в уже выбранную g_cur. */
static devdata *dd_of(const char *name)
{
    int i;

    for (i = 0; i < DD_N; i++)
        if (g_dd[i].used && strcmp(g_dd[i].name, name) == 0)
            return &g_dd[i];
    for (i = 0; i < DD_N; i++) {
        if (!g_dd[i].used) {
            memset(&g_dd[i], 0, sizeof g_dd[i]);
            g_dd[i].used = 1;
            snprintf(g_dd[i].name, sizeof g_dd[i].name, "%s", name);
            return &g_dd[i];
        }
    }
    return NULL;
}

/* ---- Access map / Seek profile: одна структура на вкладку ---- */
typedef struct {
    int          mode;          /* DGRND_ACCESS / DGRND_SEEK          */
    int          run_id;        /* RUN_ACC / RUN_SEEK                 */
    const char  *name;          /* "Access map"                       */

    devdata     *dd;            /* данные выбранного диска            */

    /* UI-поток */
    char         path[64];      /* снимок ноды на Старт               */
    int          secs, secs_run;

    NSSlider      *slider;
    NSTextField   *val;
    NSTextField   *status;
    NSButton      *start, *stop;
    NSView        *view;
} rndtab;

static rndtab g_acc;
static rndtab g_seek;

/* данные вкладки в devdata текущего диска */
static drnd *drnd_of(rndtab *t)
{
    return (t->mode == DGRND_SEEK) ? &t->dd->seek : &t->dd->acc;
}

/* 1 - процесс с правами root (иначе чтение rdisk упрётся в EACCES) */
static int g_is_root = 0;

/* ---- UI Linear ---- */
static NSTextField   *g_lin_status = NULL;
static NSTextField   *g_duty_val  = NULL;
static NSButton      *g_lin_start = NULL;
static NSButton      *g_lin_stop  = NULL;
static NSSlider      *g_lin_slider = NULL;
static NSView        *g_lin_view = NULL;
static NSTabView     *g_tabs = NULL;

/* шапка: глобальный выбор диска над всеми вкладками */
static NSPopUpButton *g_dd_dev = NULL;

/* dev-хук -cycle */
static int g_cycle = 0;
static int g_cycle_ticks = 0;

/* dev-хук -stop N: через N секунд после старта нажать "Стоп" */
static time_t g_stop_at = 0;

/* dev-хук -again: после завершения прогона сразу стартовать снова */
static int g_again = 0;

/* dev-хук -close N: через N секунд закрыть окно (как красной кнопкой) */
static time_t    g_close_at = 0;
static NSWindow *g_win = NULL;

/* dev-хук -snap N: через N секунд сохранить PDF всего окна
 * (/tmp/diskgraph-snap.pdf) - визуальная проверка UI без глаз рядом */
static time_t g_snap_at = 0;

static void rndtab_init(rndtab *t, int mode, int run_id, const char *name,
                        int secs)
{
    memset(t, 0, sizeof *t);
    t->mode   = mode;
    t->run_id = run_id;
    t->name   = name;
    t->secs   = secs;
    t->dd     = dd_of(g_dd_sel);
}

static rndtab *rndtab_of_run(int run)
{
    if (run == RUN_ACC)  return &g_acc;
    if (run == RUN_SEEK) return &g_seek;
    return NULL;
}

/* вызывать под g_lock */
static int pts_add(dbuf *b, double x, double val, int tag)
{
    if (b->n == b->alloc) {
        int  na = (b->alloc > 0) ? b->alloc * 2 : 1024;
        dpt *p  = (dpt *)realloc(b->a, (size_t)na * sizeof(dpt));
        if (p == NULL)
            return 0;
        b->a = p;
        b->alloc = na;
    }
    b->a[b->n].x   = x;
    b->a[b->n].val = val;
    b->a[b->n].tag = tag;
    b->n++;
    return 1;
}

/* вызывать под g_lock: копия src в dst (для рисования без лока) */
static void pts_copy(const dbuf *src, dbuf *dst)
{
    if (dst->alloc < src->n) {
        dpt *p = (dpt *)realloc(dst->a, (size_t)src->alloc * sizeof(dpt));
        if (p == NULL) {
            dst->n = 0;
            return;
        }
        dst->a = p;
        dst->alloc = src->alloc;
    }
    if (src->n > 0)
        memcpy(dst->a, src->a, (size_t)src->n * sizeof(dpt));
    dst->n = src->n;
}

static double mbs_of(uint64_t bytes, uint64_t us)
{
    return us > 0 ? (double)bytes / ((double)us / 1e6) / 1048576.0 : 0.0;
}

/* ------------------------------------------------------- оси X */

/* доля 0..1 по оси X: random seek - лог-ось от SEEK_XMIN, иначе линейная */
static double x_frac(int xlog, double x, double cap)
{
    double f;

    if (cap <= 0.0)
        return 0.0;
    if (xlog) {
        if (cap <= SEEK_XMIN)
            return 0.0;
        if (x < SEEK_XMIN)
            x = SEEK_XMIN;
        f = log(x / SEEK_XMIN) / log(cap / SEEK_XMIN);
    } else {
        f = x / cap;
    }
    if (f < 0.0) f = 0.0;
    if (f > 1.0) f = 1.0;
    return f;
}

static int bin_of(int xlog, double x, double cap)
{
    int b = (int)(x_frac(xlog, x, cap) * (double)NBIN);
    return (b >= NBIN) ? NBIN - 1 : b;
}

/* ось X обоих замеров - ёмкость диска, на котором данные (dd->cap
 * ставится и опросником (IOMedia), и begin()-калбеком ядра) */

/* ------------------------------------------------------- статистика */

typedef struct {
    int    b;
    double v;
} binval;

static int bv_cmp(const void *a, const void *b)
{
    const binval *x = (const binval *)a, *y = (const binval *)b;
    if (x->b != y->b)
        return x->b - y->b;
    return (x->v > y->v) - (x->v < y->v);
}

static double median_sorted(const double *v, int n)
{
    return n > 0 ? dg_pct(v, (size_t)n, 0.50) : 0.0;
}

/*
 * UI-поток: med/p99, медиана и p10 по корзинам дистанции, для seek -
 * медианы коротких (<1 MB) и длинных (> 1/2 диска) хопов. Копия под
 * локом, сортировки вне лока; пересчёт только если точек прибавилось.
 */
static void rnd_stats_update(rndtab *t)
{
    static double *v = NULL, *sv = NULL, *lv = NULL;
    static binval *bv = NULL;
    static int     valloc = 0;
    drnd          *r;
    int            n, i, ns = 0, nl = 0, xlog = (t->mode == DGRND_SEEK);
    double         cap;

    if (t->dd == NULL)
        return;
    r = drnd_of(t);

    pthread_mutex_lock(&g_lock);
    n = r->pts.n;
    if (n == r->stat_n) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    if (n > valloc) {
        int    a = r->pts.alloc;
        double *p1 = (double *)realloc(v,  (size_t)a * sizeof(double));
        double *p2 = (double *)realloc(sv, (size_t)a * sizeof(double));
        double *p3 = (double *)realloc(lv, (size_t)a * sizeof(double));
        binval *p4 = (binval *)realloc(bv, (size_t)a * sizeof(binval));
        if (p1) v = p1;
        if (p2) sv = p2;
        if (p3) lv = p3;
        if (p4) bv = p4;
        if (!p1 || !p2 || !p3 || !p4) {
            pthread_mutex_unlock(&g_lock);
            return;
        }
        valloc = a;
    }
    cap = t->dd->cap;
    for (i = 0; i < n; i++) {
        const dpt *p = &r->pts.a[i];
        v[i] = p->val;
        bv[i].b = bin_of(xlog, p->x, cap);
        bv[i].v = p->val;
        if (p->x < 1048576.0)
            sv[ns++] = p->val;
        else if (p->x > cap * 0.5)
            lv[nl++] = p->val;
    }
    pthread_mutex_unlock(&g_lock);

    qsort(v, (size_t)n, sizeof(double), dg_dcmp);
    r->med = dg_pct(v, (size_t)n, 0.50);
    r->p99 = dg_pct(v, (size_t)n, 0.99);

    qsort(sv, (size_t)ns, sizeof(double), dg_dcmp);
    qsort(lv, (size_t)nl, sizeof(double), dg_dcmp);
    r->short_med = median_sorted(sv, ns);
    r->long_med  = median_sorted(lv, nl);

    qsort(bv, (size_t)n, sizeof(binval), bv_cmp);
    for (i = 0; i < NBIN; i++)
        r->bn[i] = 0;
    for (i = 0; i < n; ) {
        int b = bv[i].b, j = i, k;
        while (j < n && bv[j].b == b)
            j++;
        k = j - i;
        r->bn[b]   = k;
        r->bmed[b] = bv[i + (int)(0.50 * (double)(k - 1) + 0.5)].v;
        r->blo[b]  = bv[i + (int)(0.10 * (double)(k - 1) + 0.5)].v;
        i = j;
    }
    r->stat_n = n;
}

/* ------------------------------------------------ список устройств */

/*
 * Единый энумератор дисков - фоновый смарт-опросник (смартcore
 * sm_scan/sm_probe): он перечисляет ВСЕ целые /dev/diskN независимо от
 * шины (внутренний ATA, USB, FireWire), достаёт из реестра IOMedia
 * ёмкость (без открытия ноды - спящий диск не будит), модель и шину
 * там, где мост их выдаёт, SMART - где он есть. Порядковый номер diskN
 * един для всех шин, rdiskN - та же нода в raw-проекции.
 *
 * UI-поток: записи по дискам (devdata) заводятся/обогащаются здесь,
 * по завершённому поколению опроса (см. sm_poll -> dd_repopulate).
 */

static const sm_disk *sm_find(const char *name);
static NSString *sm_title(const sm_disk *d);
static NSString *u8(const char *s);

/* "disk0 - ST360021A, 55.9 GB" -> "disk0" */
static void title_to_disk(const char *u, char *out, size_t outsz)
{
    size_t i;

    for (i = 0; u != NULL && u[i] != 0 && u[i] != ' ' && i + 1 < outsz; i++)
        out[i] = u[i];
    out[i] = 0;
}

/* пункт шапки по диску; у диска без паспорта (USB-мост) - как у опроса
 * SMART: только имя и ёмкость от IOMedia */
static NSString *dd_title(const devdata *dd)
{
    const sm_disk *d = sm_find(dd->name);

    if (d == NULL)
        return u8(dd->name);
    return sm_title(d);
}

/* UI-поток: осмотреть поколение опросника - завести недостающие записи,
 * дописать ёмкость (модель беру оттуда же; файловая система не нужна) */
static void dd_sync_ui(void)
{
    int i;

    for (i = 0; i < g_sm_ui_n; i++) {
        devdata *dd = dd_of(g_sm_ui[i].name);
        if (dd != NULL && dd->cap == 0.0 && g_sm_ui[i].bytes > 0)
            dd->cap = (double)g_sm_ui[i].bytes;
    }
}

/* UI-поток: перезаполнить шапку списком дисков поколения. Выбор
 * сохраняется; если выбранного нет в списке (диск исчез) - первый.
 * Записи со смарт-данными пусты до первого поколения - пункт-заглушка. */
static void dd_repopulate(void)
{
    int i, n = 0, sel = -1;

    if (g_dd_dev == NULL)
        return;
    [g_dd_dev removeAllItems];
    for (i = 0; i < g_sm_ui_n; i++) {
        devdata *dd = dd_of(g_sm_ui[i].name);

        if (dd == NULL)
            continue;               /* DD_N закончился */
        if (dd->cap == 0.0 && g_sm_ui[i].bytes > 0)
            dd->cap = (double)g_sm_ui[i].bytes;
        [g_dd_dev addItemWithTitle:dd_title(dd)];
        if (strcmp(dd->name, g_dd_sel) == 0)
            sel = n;
        n++;
    }
    if (n == 0) {
        [g_dd_dev addItemWithTitle:u8("(опрос дисков...)")];
        [g_dd_dev selectItemAtIndex:0];
        return;
    }
    if (sel < 0) {
        snprintf(g_dd_sel, sizeof g_dd_sel, "%s", g_sm_ui[0].name);
        sel = 0;
    }
    [g_dd_dev selectItemAtIndex:sel];
}

/* ------------------------------------------------------- шрифты/текст */

/*
 * C-строка -> NSString без nil: если snprintf/strncat обрезали русский
 * текст посреди UTF-8-последовательности, хвост отбрасывается (nil в
 * setStringValue: на старых AppKit - исключение).
 */
static NSString *u8(const char *s)
{
    NSString *r = [NSString stringWithUTF8String:s];
    char      b[1024];
    size_t    n;

    if (r != nil)
        return r;
    snprintf(b, sizeof b, "%s", s);
    n = strlen(b);
    while (n > 0 && ((unsigned char)b[n - 1] & 0x80))
        b[--n] = 0;                 /* до последнего ASCII-байта */
    r = [NSString stringWithUTF8String:b];
    return r != nil ? r : @"";
}

static NSColor *rgb(float r, float g, float b, float a)
{
    return [NSColor colorWithCalibratedRed:r green:g blue:b alpha:a];
}

static NSColor *col_line(void)    { return rgb(0.13f, 0.40f, 0.82f, 1.0f); }
static NSColor *col_fill(void)    { return rgb(0.13f, 0.40f, 0.82f, 0.13f); }
static NSColor *col_dot(void)     { return rgb(0.10f, 0.25f, 0.55f, 0.50f); }
static NSColor *col_dot_fwd(void) { return rgb(0.05f, 0.60f, 0.60f, 0.55f); }
static NSColor *col_spike(void)   { return rgb(0.85f, 0.15f, 0.12f, 0.85f); }
static NSColor *col_avg(void)     { return rgb(0.10f, 0.55f, 0.30f, 1.0f); }
static NSColor *col_p99(void)     { return rgb(0.85f, 0.35f, 0.10f, 1.0f); }

static NSDictionary *attrs(float size, int bold, NSColor *c)
{
    NSFont *f = bold ? [NSFont boldSystemFontOfSize:size]
                     : [NSFont systemFontOfSize:size];
    return [NSDictionary dictionaryWithObjectsAndKeys:
                f, NSFontAttributeName,
                c, NSForegroundColorAttributeName,
                NULL];
}

/* подписи тиков: кеш, рисуются десятками за кадр */
static NSDictionary *tick_attrs(void)
{
    static NSDictionary *d = NULL;
    if (d == NULL)
        d = [attrs(10.0f, 0, [NSColor colorWithCalibratedWhite:0.30f
                                                         alpha:1.0f]) retain];
    return d;
}

static NSSize text_size(const char *s, NSDictionary *a)
{
    return [[NSString stringWithUTF8String:s] sizeWithAttributes:a];
}

static void draw_text(const char *s, NSPoint p, NSDictionary *a)
{
    [[NSString stringWithUTF8String:s] drawAtPoint:p withAttributes:a];
}

/* ------------------------------------------------------- шкалы */

/* круглые мантиссы для приборной шкалы */
static const double k_cand[10] = {1.0, 1.2, 1.5, 2.0, 2.5,
                                  3.0, 4.0, 5.0, 6.0, 8.0};

static int is_nice(double x)
{
    double m;
    int    i;

    if (x <= 0.0)
        return 0;
    m = x;
    while (m >= 10.0) m /= 10.0;
    while (m < 1.0)   m *= 10.0;
    for (i = 0; i < 10; i++)
        if (fabs(m - k_cand[i]) < 1e-9)
            return 1;
    return 0;
}

static double nice_up(double x)
{
    double exp = 1.0;
    double f;
    int    i;

    if (x <= 0.0)
        return 1.0;
    while (x / exp >= 10.0) exp *= 10.0;
    while (x / exp < 1.0)   exp /= 10.0;
    f = x / exp;
    for (i = 0; i < 10; i++)
        if (f <= k_cand[i] + 1e-12)
            return k_cand[i] * exp;
    return 10.0 * exp;
}

/* Y-максимум под данные: круглые деления, запас ~8%, без пустого верха */
static double axis_vmax(double raw, int *ndiv)
{
    double vmax = nice_up(raw * 1.08);
    int    d;

    for (d = 4; d <= 6; d++) {
        if (is_nice(vmax / (double)d)) {
            *ndiv = d;
            return vmax;
        }
    }
    *ndiv = 5;
    return vmax;
}

/* шаг меток X (GB) из ряда 1-2-5 под ширину: не теснее ~70 px */
static double x_step_gb(double capgb, float width)
{
    static const double c[4] = {1.0, 2.0, 5.0, 10.0};
    double raw, exp = 1.0;
    int    maxt, i;

    maxt = (int)(width / 70.0f);
    if (maxt < 2)
        maxt = 2;
    raw = capgb / (double)maxt;
    if (raw <= 0.0)
        return 1.0;
    while (raw / exp >= 10.0) exp *= 10.0;
    while (raw / exp < 1.0)   exp /= 10.0;
    for (i = 0; i < 4; i++)
        if (raw / exp <= c[i] + 1e-12)
            return c[i] * exp;
    return 10.0 * exp;
}

/* 4096 -> "4K", 268435456 -> "256M", 4294967296 -> "4G" */
static void fmt_bytes(double b, char *out, size_t outsz)
{
    if (b >= GIB)
        snprintf(out, outsz, "%.0fG", b / GIB);
    else if (b >= 1048576.0)
        snprintf(out, outsz, "%.0fM", b / 1048576.0);
    else
        snprintf(out, outsz, "%.0fK", b / 1024.0);
}

/* ------------------------------------------------------- рисование */

typedef struct {
    NSRect pv;      /* область построения */
    double cap;     /* правый край оси X, байты; <= 0 - неизвестен */
    int    xlog;    /* 1 - лог-ось X от SEEK_XMIN */
    double vmax;    /* ось Y; <= 0 - данных нет, шкалы нет */
    int    ndiv;
} plotgeo;

static float geo_x(const plotgeo *g, double x)
{
    return NSMinX(g->pv) + (float)x_frac(g->xlog, x, g->cap) *
                           g->pv.size.width;
}

static NSPoint geo_pt(const plotgeo *g, double x, double val)
{
    double fy = (g->vmax > 0.0) ? val / g->vmax : 0.0;
    if (fy > 1.0) fy = 1.0;
    return NSMakePoint(geo_x(g, x),
                       NSMinY(g->pv) + (float)fy * g->pv.size.height);
}

static NSRect plot_rect(NSRect bounds)
{
    float padl = 52.0f, padr = 16.0f, padt = 24.0f, padb = 40.0f;
    return NSMakeRect(padl, padb,
                      NSWidth(bounds) - padl - padr,
                      NSHeight(bounds) - padt - padb);
}

/* метка X: вертикаль сетки + тик + подпись */
static void x_tick(const plotgeo *g, NSBezierPath *grid, NSBezierPath *ticks,
                   float x, const char *lbl, int with_grid)
{
    NSSize ts = text_size(lbl, tick_attrs());

    if (with_grid && x > NSMinX(g->pv) + 0.5f && x < NSMaxX(g->pv) - 0.5f) {
        [grid moveToPoint:NSMakePoint(x, NSMinY(g->pv))];
        [grid lineToPoint:NSMakePoint(x, NSMaxY(g->pv))];
    }
    [ticks moveToPoint:NSMakePoint(x, NSMinY(g->pv))];
    [ticks lineToPoint:NSMakePoint(x, NSMinY(g->pv) - 4.0f)];
    draw_text(lbl, NSMakePoint(x - ts.width * 0.5f, NSMinY(g->pv) - 17.0f),
              tick_attrs());
}

/* подложка, сетка, оси и подписи; ylab - имя оси Y, ydiv - делитель
 * для подписей (1 - как есть, 1000 - µs в ms), xlab - имя оси X */
static void draw_frame(const plotgeo *g, const char *ylab, double ydiv,
                       const char *xlab)
{
    NSRect pv = g->pv;
    float  x0 = NSMinX(pv), y0 = NSMinY(pv);
    float  x1 = NSMaxX(pv), y1 = NSMaxY(pv);
    float  dash[2] = {2.0f, 3.0f};
    NSBezierPath *grid = [NSBezierPath bezierPath];
    NSBezierPath *ticks = [NSBezierPath bezierPath];
    NSBezierPath *box;
    int    i;

    /* белая область построения в тонкой рамке */
    [[NSColor whiteColor] set];
    NSRectFill(pv);

    if (g->vmax > 0.0) {
        for (i = 1; i < g->ndiv; i++) {
            float y = y0 + ((float)i / (float)g->ndiv) * pv.size.height;
            [grid moveToPoint:NSMakePoint(x0, y)];
            [grid lineToPoint:NSMakePoint(x1, y)];
        }
    }

    if (g->cap > 0.0 && g->xlog) {
        /* лог-ось: степени 4 от 4K, при тесноте - степени 16 */
        double mul = 4.0, b;
        if (geo_x(g, SEEK_XMIN * 4.0) - x0 < 34.0f)
            mul = 16.0;
        for (b = SEEK_XMIN; b <= g->cap * 1.0001; b *= mul) {
            char lbl[16];
            fmt_bytes(b, lbl, sizeof lbl);
            x_tick(g, grid, ticks, geo_x(g, b), lbl, 1);
        }
    } else if (g->cap > 0.0) {
        double capgb = g->cap / GIB;
        double step  = x_step_gb(capgb, pv.size.width);
        int    k;

        for (k = 0; step * (double)k <= capgb + 1e-9; k++) {
            double gb = step * (double)k;
            char   lbl[24];
            if (step >= 1.0)
                snprintf(lbl, sizeof lbl, "%.0f", gb);
            else
                snprintf(lbl, sizeof lbl, "%.1f", gb);
            x_tick(g, grid, ticks, x0 + (float)(gb / capgb) * pv.size.width,
                   lbl, 1);
        }
    }

    [[NSColor colorWithCalibratedWhite:0.86f alpha:1.0f] set];
    [grid setLineWidth:1.0f];
    [grid setLineDash:dash count:2 phase:0.0f];
    [grid stroke];

    if (g->vmax > 0.0) {
        for (i = 0; i <= g->ndiv; i++) {
            float  y   = y0 + ((float)i / (float)g->ndiv) * pv.size.height;
            double val = g->vmax * ((double)i / (double)g->ndiv) / ydiv;
            char   lbl[32];
            NSSize ts;

            [ticks moveToPoint:NSMakePoint(x0, y)];
            [ticks lineToPoint:NSMakePoint(x0 - 4.0f, y)];
            if (g->vmax / ydiv < 10.0)
                snprintf(lbl, sizeof lbl, "%.1f", val);
            else
                snprintf(lbl, sizeof lbl, "%.0f", val);
            ts = text_size(lbl, tick_attrs());
            draw_text(lbl, NSMakePoint(x0 - 7.0f - ts.width, y - 7.0f),
                      tick_attrs());
        }
    }

    [[NSColor colorWithCalibratedWhite:0.55f alpha:1.0f] set];
    [ticks setLineWidth:1.0f];
    [ticks stroke];

    box = [NSBezierPath bezierPathWithRect:NSInsetRect(pv, -0.5f, -0.5f)];
    [[NSColor colorWithCalibratedWhite:0.72f alpha:1.0f] set];
    [box setLineWidth:1.0f];
    [box stroke];

    if (g->cap > 0.0) {
        NSDictionary *a = attrs(10.0f, 0,
            [NSColor colorWithCalibratedWhite:0.40f alpha:1.0f]);
        NSSize ts = text_size(xlab, a);
        draw_text(xlab, NSMakePoint(x0 + (pv.size.width - ts.width) * 0.5f,
                                    y0 - 33.0f), a);
    }
    if (g->vmax > 0.0)
        draw_text(ylab, NSMakePoint(x0 - 4.0f, y1 + 5.0f),
                  attrs(10.0f, 1,
                        [NSColor colorWithCalibratedWhite:0.25f alpha:1.0f]));
}

/* серая подсказка по центру пустой области */
static void draw_hint(NSRect pv, const char *s)
{
    NSDictionary *a = attrs(13.0f, 0,
        [NSColor colorWithCalibratedWhite:0.55f alpha:1.0f]);
    NSSize ts = text_size(s, a);

    draw_text(s, NSMakePoint(NSMidX(pv) - ts.width * 0.5f,
                             NSMidY(pv) - ts.height * 0.5f), a);
}

/* горизонтальная пунктирная отметка с подписью у правого края */
static void draw_hmark(const plotgeo *g, double val, NSColor *c,
                       const char *lbl)
{
    NSPoint p = geo_pt(g, 0.0, val);
    float   dash[2] = {5.0f, 3.0f};
    NSBezierPath *bp = [NSBezierPath bezierPath];
    NSDictionary *a = attrs(10.0f, 1, c);
    NSSize ts = text_size(lbl, a);
    NSRect bg;

    if (val > g->vmax)
        return;
    [bp moveToPoint:NSMakePoint(NSMinX(g->pv), p.y)];
    [bp lineToPoint:NSMakePoint(NSMaxX(g->pv), p.y)];
    [bp setLineWidth:1.0f];
    [bp setLineDash:dash count:2 phase:0.0f];
    [c set];
    [bp stroke];

    bg = NSMakeRect(NSMaxX(g->pv) - ts.width - 8.0f, p.y + 2.0f,
                    ts.width + 4.0f, ts.height);
    if (NSMaxY(bg) > NSMaxY(g->pv))
        bg.origin.y = p.y - ts.height - 2.0f;
    [[NSColor colorWithCalibratedWhite:1.0f alpha:0.85f] set];
    NSRectFillUsingOperation(bg, NSCompositeSourceOver);
    draw_text(lbl, NSMakePoint(bg.origin.x + 2.0f, bg.origin.y), a);
}

/* легенда в правом верхнем углу над графиком: цветной квадрат + текст */
static float draw_legend(const plotgeo *g, float right, NSColor *c,
                         const char *lbl, int dashed_line)
{
    NSDictionary *a = attrs(10.0f, 0,
        [NSColor colorWithCalibratedWhite:0.30f alpha:1.0f]);
    NSSize ts = text_size(lbl, a);
    float  y = NSMaxY(g->pv) + 5.0f;
    float  x = right - ts.width;

    draw_text(lbl, NSMakePoint(x, y), a);
    [c set];
    if (dashed_line < 0) {
        NSRectFillUsingOperation(NSMakeRect(x - 11.0f, y + 4.0f, 6.0f, 6.0f),
                                 NSCompositeSourceOver);
    } else {
        float dash[2] = {3.0f, 2.0f};
        NSBezierPath *bp = [NSBezierPath bezierPath];
        [bp moveToPoint:NSMakePoint(x - 18.0f, y + 7.0f)];
        [bp lineToPoint:NSMakePoint(x - 4.0f, y + 7.0f)];
        [bp setLineWidth:2.0f];
        if (dashed_line)
            [bp setLineDash:dash count:2 phase:0.0f];
        [bp stroke];
    }
    return x - 32.0f;
}

static const char *idle_hint(void)
{
    if (!g_is_root)
        return "нет прав на raw-диск - перезапустите приложение "
               "и введите пароль";
    return "выберите диск и нажмите Старт";
}

/* EIO: красные крестики у нижнего края на позиции ошибки */
static int draw_eio(const plotgeo *g, const dbuf *errs)
{
    NSBezierPath *bp = [NSBezierPath bezierPath];
    float y = NSMinY(g->pv) + 6.0f;
    int   i;

    for (i = 0; i < errs->n; i++) {
        float x = geo_x(g, errs->a[i].x);
        [bp moveToPoint:NSMakePoint(x - 4.0f, y - 4.0f)];
        [bp lineToPoint:NSMakePoint(x + 4.0f, y + 4.0f)];
        [bp moveToPoint:NSMakePoint(x - 4.0f, y + 4.0f)];
        [bp lineToPoint:NSMakePoint(x + 4.0f, y - 4.0f)];
    }
    [bp setLineWidth:2.0f];
    [col_spike() set];
    [bp stroke];
    return errs->n;
}

@interface LinView : NSView
@end

@implementation LinView

- (void)drawRect:(NSRect)dirty
{
    static dbuf snap = { NULL, 0, 0 };
    static dbuf esnap = { NULL, 0, 0 };
    devdata *dd = g_cur;
    dlin    *r  = (dd != NULL) ? &dd->lin : NULL;
    plotgeo g;
    double  raw = 0.0;
    int     i, running = (g_run == RUN_LIN);
    uint64_t bytes, us;

    (void)dirty;

    if (dd == NULL) {
        draw_hint(plot_rect([self bounds]), idle_hint());
        return;
    }

    pthread_mutex_lock(&g_lock);
    pts_copy(&r->pts, &snap);
    pts_copy(&r->errs, &esnap);
    g.cap  = dd->cap;
    bytes  = r->bytes;
    us     = r->time_us;
    pthread_mutex_unlock(&g_lock);
    g.xlog = 0;

    for (i = 0; i < snap.n; i++)
        if (snap.a[i].val > raw)
            raw = snap.a[i].val;

    g.pv   = plot_rect([self bounds]);
    g.ndiv = 4;
    g.vmax = (raw > 0.0) ? axis_vmax(raw, &g.ndiv) : 0.0;

    draw_frame(&g, "MB/s", 1.0, "позиция на диске, GB");
    if (esnap.n > 0) {
        char lbl[32];
        draw_eio(&g, &esnap);
        snprintf(lbl, sizeof lbl, "EIO: %d", esnap.n);
        draw_legend(&g, NSMaxX(g.pv), col_spike(), lbl, -1);
    }

    if (snap.n == 0) {
        draw_hint(g.pv, running ? "открываю диск..." : idle_hint());
        return;
    }

    {
        NSBezierPath *line = [NSBezierPath bezierPath];
        NSBezierPath *area = [NSBezierPath bezierPath];
        NSPoint first = geo_pt(&g, snap.a[0].x, snap.a[0].val);
        NSPoint last  = first;

        [area moveToPoint:NSMakePoint(first.x, NSMinY(g.pv))];
        for (i = 0; i < snap.n; i++) {
            NSPoint p = geo_pt(&g, snap.a[i].x, snap.a[i].val);
            if (i == 0)
                [line moveToPoint:p];
            else
                [line lineToPoint:p];
            [area lineToPoint:p];
            last = p;
        }
        [area lineToPoint:NSMakePoint(last.x, NSMinY(g.pv))];
        [area closePath];

        [col_fill() set];
        [area fill];

        [col_line() set];
        [line setLineWidth:1.5f];
        [line setLineJoinStyle:NSRoundLineJoinStyle];
        [line stroke];

        /* где сейчас головка */
        if (running) {
            float dash[2] = {3.0f, 3.0f};
            NSBezierPath *cur = [NSBezierPath bezierPath];
            [cur moveToPoint:NSMakePoint(last.x, NSMinY(g.pv))];
            [cur lineToPoint:NSMakePoint(last.x, NSMaxY(g.pv))];
            [cur setLineDash:dash count:2 phase:0.0f];
            [[NSColor colorWithCalibratedWhite:0.55f alpha:1.0f] set];
            [cur stroke];
        }
    }

    if (us > 0) {
        char lbl[48];
        double avg = mbs_of(bytes, us);
        snprintf(lbl, sizeof lbl, "avg %.1f MB/s", avg);
        draw_hmark(&g, avg, col_avg(), lbl);
    }
}

@end

/* Access map: лаг vs позиция; Seek profile: лаг vs дистанция хопа */
@interface RndView : NSView
{
    rndtab *tab;
}
- (void)setTab:(rndtab *)t;
@end

@implementation RndView

- (void)setTab:(rndtab *)t
{
    tab = t;
}

- (void)drawRect:(NSRect)dirty
{
    static dbuf snap = { NULL, 0, 0 };
    static dbuf esnap = { NULL, 0, 0 };
    rndtab *t = tab;
    plotgeo g;
    double  raw = 0.0, thr = 1e30;
    int     i, over = 0, seek, running;

    (void)dirty;
    if (t == NULL || t->dd == NULL)
        return;
    seek    = (t->mode == DGRND_SEEK);
    running = (g_run == t->run_id);

    {
        drnd *r = drnd_of(t);
        pthread_mutex_lock(&g_lock);
        pts_copy(&r->pts, &snap);
        pts_copy(&r->errs, &esnap);
        g.cap = t->dd->cap;
        pthread_mutex_unlock(&g_lock);
    }
    g.xlog = seek;

    for (i = 0; i < snap.n; i++)
        if (snap.a[i].val > raw)
            raw = snap.a[i].val;

    /* единичный выброс (рекалибровка, спин-ап) не сплющивает облако:
     * шкала по 2*p99, всё выше - красной меткой у верхнего края */
    if (snap.n >= 100) {
        drnd *r = drnd_of(t);
        if (r->stat_n > 0 && r->p99 > 0.0) {
            if (!seek)
                thr = r->p99;
            if (raw > 3.0 * r->p99)
                raw = 2.0 * r->p99;
        }
    }

    g.pv   = plot_rect([self bounds]);
    g.ndiv = 5;
    g.vmax = (raw > 0.0) ? axis_vmax(raw, &g.ndiv) : 0.0;

    draw_frame(&g, "ms", 1000.0,
               seek ? "дистанция хопа (лог. шкала)"
                    : "позиция на диске, GB");

    if (snap.n == 0) {
        draw_hint(g.pv, running ? "открываю диск, прогрев..." : idle_hint());
        return;
    }

    {
        NSBezierPath *dots   = [NSBezierPath bezierPath];
        NSBezierPath *fwd    = [NSBezierPath bezierPath];
        NSBezierPath *spikes = [NSBezierPath bezierPath];

        for (i = 0; i < snap.n; i++) {
            NSPoint p = geo_pt(&g, snap.a[i].x, snap.a[i].val);
            NSRect  r = NSMakeRect(p.x - 1.5f, p.y - 1.5f, 3.0f, 3.0f);
            if (snap.a[i].val > g.vmax) {
                /* за шкалой: треугольник остриём вверх у края */
                float y = NSMaxY(g.pv) - 1.0f;
                [spikes moveToPoint:NSMakePoint(p.x - 3.0f, y - 5.0f)];
                [spikes lineToPoint:NSMakePoint(p.x + 3.0f, y - 5.0f)];
                [spikes lineToPoint:NSMakePoint(p.x, y)];
                [spikes closePath];
                over++;
            } else if (snap.a[i].val > thr) {
                [spikes appendBezierPathWithRect:r];
            } else if (seek && snap.a[i].tag > 0) {
                [fwd appendBezierPathWithRect:r];
            } else {
                [dots appendBezierPathWithRect:r];
            }
        }
        [col_dot() set];
        [dots fill];
        [col_dot_fwd() set];
        [fwd fill];
        [col_spike() set];
        [spikes fill];
    }

    /* кривые по корзинам дистанции: медиана и p10 (≈ чистый seek) */
    if (drnd_of(t)->stat_n >= 50) {
        NSBezierPath *med = [NSBezierPath bezierPath];
        NSBezierPath *lo  = [NSBezierPath bezierPath];
        float  dash[2] = {4.0f, 2.0f};
        int    started = 0;

        for (i = 0; i < NBIN; i++) {
            double fx;
            float  x;
            NSPoint pm, pl;

            if (drnd_of(t)->bn[i] < 5)
                continue;
            fx = ((double)i + 0.5) / (double)NBIN;
            x  = NSMinX(g.pv) + (float)fx * g.pv.size.width;
            pm = geo_pt(&g, 0.0, drnd_of(t)->bmed[i]);
            pl = geo_pt(&g, 0.0, drnd_of(t)->blo[i]);
            pm.x = pl.x = x;
            if (!started) {
                [med moveToPoint:pm];
                [lo moveToPoint:pl];
                started = 1;
            } else {
                [med lineToPoint:pm];
                [lo lineToPoint:pl];
            }
        }
        [med setLineWidth:2.0f];
        [med setLineJoinStyle:NSRoundLineJoinStyle];
        [lo setLineWidth:2.0f];
        [lo setLineJoinStyle:NSRoundLineJoinStyle];
        [lo setLineDash:dash count:2 phase:0.0f];
        [col_avg() set];
        [med stroke];
        [col_p99() set];
        [lo stroke];
    }

    if (!seek && snap.n >= 20) {
        char lbl[48];
        snprintf(lbl, sizeof lbl, "p99 %.1f ms", drnd_of(t)->p99 / 1000.0);
        draw_hmark(&g, drnd_of(t)->p99, col_spike(), lbl);
    }

    {
        float right = NSMaxX(g.pv);
        if (esnap.n > 0) {
            char lbl[32];
            if (!seek)
                draw_eio(&g, &esnap);   /* у seek нет дистанции ошибки */
            snprintf(lbl, sizeof lbl, "EIO: %d", esnap.n);
            right = draw_legend(&g, right, col_spike(), lbl, -1);
        }
        if (over > 0) {
            char lbl[48];
            snprintf(lbl, sizeof lbl, "выше шкалы: %d", over);
            right = draw_legend(&g, right, col_spike(), lbl, -1);
        }
        right = draw_legend(&g, right, col_p99(),
                            seek ? "p10 (≈ seek)" : "p10", 1);
        right = draw_legend(&g, right, col_avg(), "медиана", 0);
        if (seek) {
            right = draw_legend(&g, right, col_dot(), "назад", -1);
            right = draw_legend(&g, right, col_dot_fwd(), "вперёд", -1);
        }
    }
}

@end

/* ------------------------------------------------------- статус-строки */

enum { ST_NORMAL, ST_DONE, ST_ERROR };

static void set_status(NSTextField *f, const char *s, int kind)
{
    if (f == NULL)
        return;
    [f setStringValue:u8(s)];
    [f setFont:(kind == ST_DONE) ? [NSFont boldSystemFontOfSize:11.0f]
                                 : [NSFont systemFontOfSize:11.0f]];
    [f setTextColor:(kind == ST_ERROR) ? rgb(0.75f, 0.10f, 0.08f, 1.0f)
                                       : [NSColor controlTextColor]];
}

static void err_text(char *s, size_t sz, int oserr, const char *err)
{
    if (dg_is_perm_err(oserr))
        snprintf(s, sz, "Ошибка: нет прав на raw-диск - перезапустите "
                        "приложение и введите пароль администратора");
    else
        snprintf(s, sz, "Ошибка: %s", err);
}

static const char *k_noroot =
    "Нет прав на raw-диск - перезапустите приложение и введите пароль "
    "администратора";

/* ------------------------------------------------------------ SMART */

/*
 * SMART всех целых дисков читает фоновый поток (ядро smartcore, ретраи
 * спящего диска до ~3 с - UI не замирает). Результат - массив под
 * g_lock + номер поколения; тик видит новое поколение и копирует его
 * в g_sm_ui (UI-поток), откуда читают вкладка SMART и статусы.
 */
#define SM_MAX 16

static sm_disk g_sm_res[SM_MAX];        /* под g_lock */
static int     g_sm_res_n = 0;
static int     g_sm_gen = 0;            /* +1 на каждый завершённый опрос */
static int     g_sm_busy = 0;           /* UI-поток: опрос идёт */

/* g_sm_ui (UI-поток, копия поколения) объявлен выше - около devdata,
 * т.к. список дисков (шапка и devdata) собирается из него */

static NSButton      *g_sm_refresh = NULL;
static NSTextField   *g_sm_verdict = NULL;
static NSTextField   *g_sm_note = NULL;
static NSTextField   *g_sm_ident = NULL;
static NSTextField   *g_sm_explain = NULL;
static NSTableView   *g_sm_table = NULL;
static NSTabViewItem *g_sm_tab = NULL;

static const sm_disk *sm_find(const char *name)
{
    int i;
    for (i = 0; i < g_sm_ui_n; i++)
        if (strcmp(g_sm_ui[i].name, name) == 0)
            return &g_sm_ui[i];
    return NULL;
}

/* "/dev/rdisk1" и "/dev/disk1" -> "disk1" */
static void dev_to_disk(const char *path, char *out, size_t outsz)
{
    const char *p = strrchr(path, '/');
    p = (p != NULL) ? p + 1 : path;
    if (p[0] == 'r')
        p++;
    snprintf(out, outsz, "%s", p);
}

static void status_lin(void)
{
    devdata *dd = g_cur;
    dlin    *r;
    char s[320];
    int  n, idx, failed, canc, oserr, elapsed;
    uint64_t total, bytes, us;
    dglinresult res;
    char err[256];

    if (dd == NULL)
        return;
    r = &dd->lin;
    pthread_mutex_lock(&g_lock);
    n      = r->pts.n;
    idx    = r->idx_done;
    total  = r->total;
    bytes  = r->bytes;
    us     = r->time_us;
    failed = r->failed;
    oserr  = r->oserr;
    canc   = r->cancel;
    res    = r->res;
    elapsed = r->elapsed;
    strncpy(err, r->err, sizeof err);
    err[sizeof err - 1] = 0;
    pthread_mutex_unlock(&g_lock);

    if (g_run == RUN_LIN) {
        int t = (int)(time(NULL) - g_run_t0);
        if (canc)
            snprintf(s, sizeof s, "Останавливаюсь после текущего блока...");
        else if (total > 0)
            snprintf(s, sizeof s,
                     "Сегмент %d / %llu   ·   %.1f MB/s   ·   "
                     "duty %d%%   ·   %d:%02d",
                     idx, (unsigned long long)total, mbs_of(bytes, us),
                     r->duty, t / 60, t % 60);
        else
            snprintf(s, sizeof s, "Открываю %s ...", r->path);
        set_status(g_lin_status, s, ST_NORMAL);
    } else if (failed) {
        err_text(s, sizeof s, oserr, err);
        set_status(g_lin_status, s, ST_ERROR);
    } else if (n > 0) {
        snprintf(s, sizeof s,
                 "Готово: %llu сегментов   ·   avg %.1f MB/s   ·   "
                 "eio %llu   ·   %d:%02d%s",
                 (unsigned long long)res.chunks,
                 mbs_of(res.bytes, res.time_us),
                 (unsigned long long)res.eio,
                 elapsed / 60, elapsed % 60,
                 res.stopped ? "   [остановлено]" : "");
        set_status(g_lin_status, s, ST_DONE);
    } else if (!g_is_root) {
        set_status(g_lin_status, k_noroot, ST_ERROR);
    } else {
        if (dd->cap > 0.0)
            snprintf(s, sizeof s,
                     "Будет прочитано ≈ %.1f GB из %.1f GB "
                     "(%d%% поверхности, сегменты по 32 MB)",
                     dd->cap / GIB * (double)g_duty / 100.0,
                     dd->cap / GIB, g_duty);
        else
            snprintf(s, sizeof s, "Выберите диск и нажмите Старт");
        set_status(g_lin_status, s, ST_NORMAL);
    }
}

/* seek: "короткие ... · длинные ..." для статуса */
static void seek_summary(const drnd *r, char *out, size_t outsz)
{
    snprintf(out, outsz,
             "хопы < 1 MB: med %.1f ms   ·   > ½ диска: med %.1f ms",
             r->short_med / 1000.0, r->long_med / 1000.0);
}

static void status_rnd(rndtab *t)
{
    char s[400], extra[160];
    int  n, failed, canc, oserr, elapsed;
    dgrndresult res;
    char err[256];
    int  seek = (t->mode == DGRND_SEEK);
    drnd *r = drnd_of(t);

    if (r != NULL) {
    pthread_mutex_lock(&g_lock);
    n       = r->pts.n;
    failed  = r->failed;
    oserr   = r->oserr;
    canc    = r->cancel;
    res     = r->res;
    elapsed = r->elapsed;
    strncpy(err, r->err, sizeof err);
    err[sizeof err - 1] = 0;
    pthread_mutex_unlock(&g_lock);
    } else {
        n = failed = oserr = canc = elapsed = 0;
        memset(&res, 0, sizeof res);
        err[0] = 0;
    }

    extra[0] = 0;
    if (seek && r->stat_n > 0)
        seek_summary(r, extra, sizeof extra);

    if (g_run == t->run_id) {
        int tt = (int)(time(NULL) - g_run_t0);
        if (tt < 0) tt = 0;
        if (tt > t->secs_run) tt = t->secs_run;
        if (canc)
            snprintf(s, sizeof s, "Останавливаюсь...");
        else if (seek)
            snprintf(s, sizeof s, "%s %d   ·   %d / %d с   ·   %s",
                     "хопов", n, tt, t->secs_run, extra);
        else
            snprintf(s, sizeof s,
                     "ops %d   ·   %d / %d с   ·   med %.1f ms   ·   "
                     "p99 %.1f ms",
                     n, tt, t->secs_run, r->med / 1000.0, r->p99 / 1000.0);
        set_status(t->status, s, ST_NORMAL);
    } else if (failed) {
        err_text(s, sizeof s, oserr, err);
        set_status(t->status, s, ST_ERROR);
    } else if (n > 0) {
        if (seek)
            snprintf(s, sizeof s,
                     "Готово: %llu хопов   ·   %s   ·   eio %llu   ·   "
                     "%d:%02d%s",
                     (unsigned long long)res.ops, extra,
                     (unsigned long long)res.eio,
                     elapsed / 60, elapsed % 60,
                     res.stopped ? "   [остановлено]" : "");
        else
            snprintf(s, sizeof s,
                     "Готово: %llu ops   ·   med %.1f ms   ·   p99 %.1f ms   ·"
                     "   eio %llu   ·   %d:%02d%s",
                     (unsigned long long)res.ops,
                     res.med_us / 1000.0, res.p99_us / 1000.0,
                     (unsigned long long)res.eio,
                     elapsed / 60, elapsed % 60,
                     res.stopped ? "   [остановлено]" : "");
        set_status(t->status, s, ST_DONE);
    } else if (!g_is_root) {
        set_status(t->status, k_noroot, ST_ERROR);
    } else {
        if (seek)
            snprintf(s, sizeof s,
                     "%d с хопов от 8 KB до всей ёмкости - профиль "
                     "\"время перемещения головки vs дистанция\"", t->secs);
        else
            snprintf(s, sizeof s,
                     "%d с чтений по 4 KB в случайные точки - карта времени "
                     "доступа по поверхности", t->secs);
        set_status(t->status, s, ST_NORMAL);
    }
}

static void status_all(void)
{
    status_lin();
    status_rnd(&g_acc);
    status_rnd(&g_seek);
}

/* ------------------------------------------------------- рабочие потоки */

static void lin_begin(void *ctx, const dglininfo *info)
{
    devdata *dd = (devdata *)ctx;

    pthread_mutex_lock(&g_lock);
    dd->lin.total = info->chunks;
    dd->cap = (double)info->capacity;
    pthread_mutex_unlock(&g_lock);
}

static void lin_err(void *ctx, const dglinerr *e)
{
    devdata *dd = (devdata *)ctx;

    pthread_mutex_lock(&g_lock);
    pts_add(&dd->lin.errs, (double)e->off, 0.0, 0);
    pthread_mutex_unlock(&g_lock);
}

static void lin_seg(void *ctx, const dglinseg *seg)
{
    devdata *dd = (devdata *)ctx;

    pthread_mutex_lock(&g_lock);
    /* точка - в середине сегмента: кривая от начала до конца диска */
    pts_add(&dd->lin.pts, (double)seg->off + (double)seg->len * 0.5,
            mbs_of(seg->bytes, seg->time_us), 0);
    dd->lin.bytes    += seg->bytes;
    dd->lin.time_us  += seg->time_us;
    dd->lin.idx_done = (int)(seg->index + 1);
    pthread_mutex_unlock(&g_lock);
}

static void *lin_thread(void *arg)
{
    devdata    *dd = (devdata *)arg;
    dglincfg    cfg;
    dglinresult res;
    char        err[256];
    int         rc;
    time_t      t0 = time(NULL);

    memset(&cfg, 0, sizeof cfg);
    cfg.dev    = dd->lin.path;
    cfg.seg    = LIN_SEG;
    cfg.blk    = LIN_BLK;
    cfg.duty   = dd->lin.duty;
    cfg.extent = 0;

    rc = dglincore(&cfg, lin_begin, lin_seg, lin_err, dd,
                   &dd->lin.cancel, &res, err, sizeof err);

    pthread_mutex_lock(&g_lock);
    dd->lin.failed = (rc != 0);
    dd->lin.oserr  = res.oserr;
    if (rc != 0)
        snprintf(dd->lin.err, sizeof dd->lin.err, "%s", err);
    dd->lin.res = res;
    dd->lin.elapsed = (int)(time(NULL) - t0);
    dd->lin.done = 1;
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

static void rnd_begin(void *ctx, const dgrndinfo *info)
{
    rndtab *t = (rndtab *)ctx;
    pthread_mutex_lock(&g_lock);
    t->dd->cap = (double)info->capacity;
    pthread_mutex_unlock(&g_lock);
}

/* access: X = позиция на диске; seek: X = дистанция хопа */
static void rnd_op(void *ctx, const dgrndop *op)
{
    rndtab *t = (rndtab *)ctx;
    drnd   *r = drnd_of(t);
    double  x = (t->mode == DGRND_SEEK) ? (double)op->dist : (double)op->off;

    pthread_mutex_lock(&g_lock);
    pts_add(&r->pts, x, (double)op->us, op->dir);
    pthread_mutex_unlock(&g_lock);
}

static void rnd_err(void *ctx, const dgrnderr *e)
{
    rndtab *t = (rndtab *)ctx;
    drnd   *r = drnd_of(t);

    pthread_mutex_lock(&g_lock);
    pts_add(&r->errs, (double)e->off, (double)e->us, 0);
    pthread_mutex_unlock(&g_lock);
}

static void *rnd_thread(void *arg)
{
    rndtab     *t = (rndtab *)arg;
    drnd       *r = drnd_of(t);
    dgrndcfg    cfg;
    dgrndresult res;
    char        err[256];
    int         rc;
    time_t      t0 = time(NULL);

    memset(&cfg, 0, sizeof cfg);
    cfg.dev    = t->path;
    cfg.blk    = RND_BLK;
    cfg.secs   = t->secs_run;
    cfg.warmup = RND_WARMUP;
    cfg.seed   = 0;                 /* из времени/pid */
    cfg.mode   = t->mode;

    rc = dgrndcore(&cfg, rnd_begin, rnd_op, rnd_err, t,
                   &r->cancel, &res, err, sizeof err);

    pthread_mutex_lock(&g_lock);
    r->failed = (rc != 0);
    r->oserr  = res.oserr;
    if (rc != 0)
        snprintf(r->err, sizeof r->err, "%s", err);
    r->res = res;
    r->elapsed = (int)(time(NULL) - t0);
    r->done = 1;
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

/* UI-поток: доступность контролов по состоянию прогона */
static void update_rnd_controls(rndtab *t)
{
    drnd *r = drnd_of(t);
    int  idle = (g_run == RUN_IDLE);

    if (r == NULL)
        return;
    [t->start setEnabled:idle];
    [t->stop setEnabled:(g_run == t->run_id && !r->cancel)];
    [t->slider setEnabled:idle];
}

static void update_controls(void)
{
    int idle = (g_run == RUN_IDLE);

    [g_lin_start setEnabled:idle];
    [g_lin_stop setEnabled:(g_run == RUN_LIN && g_cur != NULL &&
                            !g_cur->lin.cancel)];
    [g_lin_slider setEnabled:idle];
    [g_dd_dev setEnabled:idle];
    update_rnd_controls(&g_acc);
    update_rnd_controls(&g_seek);
}

static int start_thread(void *(*fn)(void *), void *arg)
{
    pthread_t th;

    if (pthread_create(&th, NULL, fn, arg) != 0)
        return 0;
    pthread_detach(th);
    return 1;
}

static void lin_start(void)
{
    devdata *dd;

    if (g_run != RUN_IDLE)
        return;
    dd = g_cur;
    if (dd == NULL)
        return;

    pthread_mutex_lock(&g_lock);
    dd->lin.pts.n = 0;
    dd->lin.errs.n = 0;
    dd->lin.total = 0;
    dd->lin.bytes = 0;
    dd->lin.time_us = 0;
    dd->lin.idx_done = 0;
    dd->lin.failed = 0;
    dd->lin.oserr = 0;
    dd->lin.err[0] = 0;
    dd->lin.done = 0;
    memset(&dd->lin.res, 0, sizeof dd->lin.res);
    pthread_mutex_unlock(&g_lock);

    dd->lin.cancel = 0;
    g_run = RUN_LIN;
    g_run_t0 = time(NULL);

    snprintf(dd->lin.path, sizeof dd->lin.path, "/dev/r%s", dd->name);
    dd->lin.duty = g_duty;

    if (!start_thread(lin_thread, dd)) {
        pthread_mutex_lock(&g_lock);
        snprintf(dd->lin.err, sizeof dd->lin.err, "не удалось создать поток");
        dd->lin.failed = 1;
        dd->lin.done = 1;
        pthread_mutex_unlock(&g_lock);
    }
    update_controls();
    status_lin();
}

static void rnd_start(rndtab *t)
{
    devdata *dd = t->dd;
    drnd    *r;

    if (g_run != RUN_IDLE)
        return;
    if (dd == NULL)
        return;
    r = drnd_of(t);

    pthread_mutex_lock(&g_lock);
    r->pts.n = 0;
    r->errs.n = 0;
    r->failed = 0;
    r->oserr = 0;
    r->err[0] = 0;
    r->done = 0;
    memset(&r->res, 0, sizeof r->res);
    pthread_mutex_unlock(&g_lock);

    r->med = r->p99 = r->short_med = r->long_med = 0.0;
    r->stat_n = -1;
    memset(r->bn, 0, sizeof r->bn);
    r->cancel = 0;
    g_run = t->run_id;
    g_run_t0 = time(NULL);

    snprintf(t->path, sizeof t->path, "/dev/r%s", dd->name);
    t->secs_run = t->secs;

    if (!start_thread(rnd_thread, t)) {
        pthread_mutex_lock(&g_lock);
        snprintf(r->err, sizeof r->err, "не удалось создать поток");
        r->failed = 1;
        r->done = 1;
        pthread_mutex_unlock(&g_lock);
    }
    update_controls();
    status_rnd(t);
}

static void sm_show(void);              /* ниже, в блоке SMART */
static void sm_tab_light(void);         /* там же */

/* UI-поток: выбрать диск (после devChanged / тихого переезда выбора)
 * - вкладки и статусы переключаются на его devdata; у диска с
 * результатами графики показываются как были, у нового - пустые */
static void dd_select(void)
{
    devdata *dd = dd_of(g_dd_sel);

    if (dd == NULL)
        return;
    g_cur = dd;
    g_acc.dd  = dd;
    g_seek.dd = dd;
    status_all();
    [g_lin_view setNeedsDisplay:YES];
    [g_acc.view setNeedsDisplay:YES];
    [g_seek.view setNeedsDisplay:YES];
    sm_tab_light();
    sm_show();
}

/* по контролу - его random-вкладка (NULL - Linear) */
static rndtab *rndtab_of(id ctl)
{
    if (ctl == g_acc.start || ctl == g_acc.stop || ctl == g_acc.slider)
        return &g_acc;
    if (ctl == g_seek.start || ctl == g_seek.stop || ctl == g_seek.slider)
        return &g_seek;
    return NULL;
}

/* ------------------------------------------------------- SMART: опрос */

static void *sm_thread(void *arg)
{
    static sm_disk tmp[SM_MAX];        /* один поток за раз (g_sm_busy) */
    char names[SM_MAX][16];
    int  n, i;

    (void)arg;
    n = sm_scan(names, SM_MAX);
    for (i = 0; i < n; i++)
        sm_probe(names[i], &tmp[i]);

    pthread_mutex_lock(&g_lock);
    memcpy(g_sm_res, tmp, (size_t)n * sizeof(sm_disk));
    g_sm_res_n = n;
    g_sm_gen++;
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

static void sm_refresh(void)
{
    if (g_sm_busy)
        return;
    g_sm_busy = 1;
    if (!start_thread(sm_thread, NULL)) {
        g_sm_busy = 0;
        return;
    }
    [g_sm_refresh setEnabled:NO];
    [g_sm_verdict setStringValue:[NSString stringWithUTF8String:"..."]];
    [g_sm_note setStringValue:[NSString stringWithUTF8String:
                                  "читаю SMART..."]];
}

/* короткое пояснение атрибута для колонки "Что это" */
static const char *sm_explain(int id)
{
    switch (id) {
    case 1:   return "ошибки чтения; у Seagate RAW велик - норма";
    case 3:   return "время раскрутки шпинделя";
    case 4:   return "число пусков/остановок шпинделя";
    case 5:   return "переназначенные сектора: > 0 - поверхность сыпется";
    case 7:   return "ошибки позиционирования; у Seagate RAW велик - норма";
    case 9:   return "наработка, часы";
    case 10:  return "повторы раскрутки - механика или питание";
    case 12:  return "число включений";
    case 184: return "ошибки тракта данных внутри диска";
    case 187: return "неисправимые ошибки, о которых диск сообщил";
    case 188: return "таймауты команд";
    case 189: return "запись при высоком полёте головки";
    case 190:
    case 194: return "температура, °C";
    case 191: return "удары (датчик ускорения)";
    case 192: return "аварийные парковки при пропадании питания";
    case 193: return "парковки головок (ноутбуки: ресурс ~300-600 тыс.)";
    case 195: return "аппаратная ECC-коррекция (Seagate)";
    case 196: return "события переназначения";
    case 197: return "нестабильные сектора: данные под угрозой";
    case 198: return "нечитаемые сектора по фоновому скану";
    case 199: return "CRC-ошибки шлейфа/контроллера, не поверхность";
    case 200: return "ошибки записи";
    default:  return "";
    }
}

/* человеческое пояснение к вердикту: по каждой находке строка */
static void sm_findings(const sm_disk *d, char *out, size_t outsz)
{
    int i;

    out[0] = 0;
    if (!d->have_data) {
        if (d->need_root)
            snprintf(out, outsz, "SMART читается только с правами "
                                 "администратора.");
        else
            snprintf(out, outsz, "Данных SMART нет: %s. USB/FireWire-"
                     "мосты и RAID-тома SMART не пропускают.", d->note);
        return;
    }
    if (d->exceeded)
        snprintf(out + strlen(out), outsz - strlen(out),
                 "Диск сам сообщает о превышении порога (SMART status: "
                 "FAIL) - готовьтесь к замене.  ");
    for (i = 0; i < d->nattr; i++) {
        const sm_attr *a = &d->attrs[i];
        unsigned long  v = (unsigned long)(a->raw & 0xffffffffULL);
        char           b[512];

        if (a->level == SM_LV_OK)
            continue;
        b[0] = 0;
        if (a->id == 197)
            snprintf(b, sizeof b, "Pending = %lu: диск не смог прочитать "
                     "%lu сектор(ов), данные в них под угрозой - сделайте "
                     "копию.  ", v, v);
        else if (a->id == 5)
            snprintf(b, sizeof b, "Reallocated = %lu: диск уже подменил "
                     "%lu сектор(ов) резервными - поверхность деградирует, "
                     "следите за ростом.  ", v, v);
        else if (a->id == 198)
            snprintf(b, sizeof b, "Offline Uncorrectable = %lu: фоновый "
                     "скан нашёл нечитаемые сектора.  ", v);
        else if (a->id == 199)
            snprintf(b, sizeof b, "UDMA CRC = %lu: ошибки передачи по "
                     "шлейфу/контроллеру, а не поверхности. Проверьте "
                     "80-жильный кабель и разъёмы.  ", v);
        else if (a->level == SM_LV_FAIL)
            snprintf(b, sizeof b, "Атрибут %d (%s) дошёл до порога "
                     "(%d <= %d) - диск прогнозирует отказ.  ",
                     a->id, sm_attr_name(a->id) ? sm_attr_name(a->id) : "?",
                     a->value, a->thr);
        strncat(out, b, outsz - strlen(out) - 1);
    }
    if (out[0] == 0)
        snprintf(out, outsz, "Критичные счётчики (5, 197, 198, 199) "
                 "нулевые, пороги не достигнуты.");
}

/* заголовок пункта popup: "disk0 - ST360021A, 55.9 GB" */
static NSString *sm_title(const sm_disk *d)
{
    char t[96];
    if (d->model[0])
        snprintf(t, sizeof t, "%s - %s, %.1f GB", d->name, d->model,
                 (double)d->bytes / GIB);
    else
        snprintf(t, sizeof t, "%s", d->name);
    return [NSString stringWithUTF8String:t];
}

/* UI-поток: ярлык SMART-вкладки: (!) только если ВЫБРАННЫЙ диск
 * WARN/FAIL (не общий машинный флаг по всем дискам) */
static void sm_tab_light(void)
{
    const sm_disk *d = sm_find(g_dd_sel);
    int bad = (d != NULL && (d->vd == VD_WARN || d->vd == VD_FAIL));

    if (g_sm_tab == NULL)
        return;
    [g_sm_tab setLabel:[NSString stringWithUTF8String:
                           bad ? "SMART (!)" : "SMART"]];
    [g_tabs setNeedsDisplay:YES];
}

/* UI-поток: показать выбранный диск (плашка, паспорт, таблица) */
static void sm_show(void)
{
    const sm_disk *d = sm_find(g_dd_sel);
    char  s[1024];
    NSColor *c;

    if (g_sm_verdict == nil)
        return;
    [g_sm_table reloadData];
    if (d == NULL) {
        [g_sm_verdict setStringValue:[NSString stringWithUTF8String:"-"]];
        [g_sm_note setStringValue:@""];
        [g_sm_ident setStringValue:@""];
        [g_sm_explain setStringValue:@""];
        return;
    }

    switch (d->vd) {
    case VD_OK:   c = col_avg(); break;
    case VD_WARN: c = col_p99(); break;
    case VD_FAIL: c = col_spike(); break;
    default:      c = [NSColor grayColor]; break;
    }
    [g_sm_verdict setTextColor:c];
    [g_sm_verdict setStringValue:
        [NSString stringWithUTF8String:sm_verdict_str(d->vd)]];
    [g_sm_note setStringValue:[NSString stringWithUTF8String:
        d->note[0] ? d->note : (d->vd == VD_OK ? "всё в порядке" : "")]];

    {
        char h[64] = "", t[24] = "";
        if (d->has_hours)
            snprintf(h, sizeof h, "   ·   %llu ч (%.1f лет)",
                     (unsigned long long)d->n_hours,
                     (double)d->n_hours / 8766.0);
        if (d->temp >= 0)
            snprintf(t, sizeof t, "   ·   %d °C", d->temp);
        snprintf(s, sizeof s, "%s   ·   S/N %s   ·   FW %s   ·   %s   ·   "
                 "%.1f GB%s%s",
                 d->model[0] ? d->model : "-",
                 d->serial[0] ? d->serial : "-",
                 d->fw[0] ? d->fw : "-",
                 d->bus[0] ? d->bus : "-",
                 (double)d->bytes / GIB, h, t);
        [g_sm_ident setStringValue:u8(s)];
    }
    sm_findings(d, s, sizeof s);
    [g_sm_explain setStringValue:u8(s)];
}

/* UI-поток: тик - забрать новое поколение опроса */
static void sm_poll(void)
{
    int gen;

    pthread_mutex_lock(&g_lock);
    gen = g_sm_gen;
    if (gen != g_sm_ui_gen) {
        memcpy(g_sm_ui, g_sm_res, (size_t)g_sm_res_n * sizeof(sm_disk));
        g_sm_ui_n = g_sm_res_n;
    }
    pthread_mutex_unlock(&g_lock);
    if (gen == g_sm_ui_gen)
        return;
    g_sm_ui_gen = gen;
    g_sm_busy = 0;
    [g_sm_refresh setEnabled:YES];

    /* шапка: список дисков нового поколения; выбор сохраняется.
     * Во время прогона шапка заморожена - не трогаем ни список, ни
     * выбор (иначе dd_select мог бы перепрыгнуть на другой диск) */
    if (g_run == RUN_IDLE) {
        dd_sync_ui();
        dd_repopulate();
        dd_select();
    }

    /* "лампочка" на ярлыке вкладки - по ВЫБРАННОМУ диску */
    sm_tab_light();
}

/* ------------------------------------------------------------ контроллер */

@interface Controller : NSObject
- (void)linStart:(id)s;
- (void)linStop:(id)s;
- (void)rndStart:(id)s;
- (void)rndStop:(id)s;
- (void)dutyChanged:(id)s;
- (void)secsChanged:(id)s;
- (void)devChanged:(id)s;
- (void)appActive:(NSNotification *)n;
- (void)windowDidResize:(NSNotification *)n;
- (void)smRefresh:(id)s;
- (void)tick:(NSTimer *)timer;
- (void)quit:(id)s;
@end

@implementation Controller

- (id)init
{
    self = [super init];
    if (self != nil) {
        [[NSNotificationCenter defaultCenter] addObserver:self
            selector:@selector(appActive:)
            name:NSApplicationDidBecomeActiveNotification
            object:nil];
        [[NSNotificationCenter defaultCenter] addObserver:self
            selector:@selector(windowDidResize:)
            name:NSWindowDidResizeNotification
            object:nil];
    }
    return self;
}

- (void)dealloc
{
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [super dealloc];
}

- (void)linStart:(id)s
{
    (void)s;
    lin_start();
}

- (void)linStop:(id)s
{
    (void)s;
    if (g_run == RUN_LIN && g_cur != NULL) {
        g_cur->lin.cancel = 1;
        update_controls();
        status_lin();
    }
}

- (void)rndStart:(id)s
{
    rndtab *t = rndtab_of(s);
    if (t != NULL)
        rnd_start(t);
}

- (void)rndStop:(id)s
{
    rndtab *t = rndtab_of(s);
    drnd   *r;

    if (t == NULL)
        t = rndtab_of_run(g_run);   /* dev-хук -stop шлёт nil */
    if (t != NULL && g_run == t->run_id) {
        r = drnd_of(t);
        if (r != NULL) {
            r->cancel = 1;
            update_controls();
            status_rnd(t);
        }
    }
}

- (void)dutyChanged:(id)s
{
    char lbl[64];
    g_duty = [s intValue];
    snprintf(lbl, sizeof lbl, "%d %%", g_duty);
    [g_duty_val setStringValue:[NSString stringWithUTF8String:lbl]];
    if (g_run == RUN_IDLE)
        status_lin();
}

- (void)secsChanged:(id)s
{
    rndtab *t = rndtab_of(s);
    char    lbl[64];

    if (t == NULL)
        return;
    t->secs = [s intValue];
    snprintf(lbl, sizeof lbl, "%d с", t->secs);
    [t->val setStringValue:[NSString stringWithUTF8String:lbl]];
    if (g_run == RUN_IDLE)
        status_rnd(t);
}

- (void)devChanged:(id)s
{
    NSString *title = [s titleOfSelectedItem];
    char      nm[16];

    if (title == nil)
        return;
    /* пункт "disk0 - ST360021A, 55.9 GB" -> имя диска "disk0" */
    title_to_disk([title UTF8String], nm, sizeof nm);
    if (strncmp(nm, "disk", 4) != 0)
        return;                     /* пункт-заглушка "(опрос...)" */
    snprintf(g_dd_sel, sizeof g_dd_sel, "%s", nm);
    dd_select();
}

- (void)appActive:(NSNotification *)n
{
    (void)n;
    /* горячее подключение/уход диска: пересканировать опросником */
    sm_refresh();
}

- (void)smRefresh:(id)s
{
    (void)s;
    sm_refresh();
}

/* таблица SMART: data source / delegate (API 10.0) */
- (int)numberOfRowsInTableView:(NSTableView *)tv
{
    const sm_disk *d = sm_find(g_dd_sel);
    (void)tv;
    return (d != NULL && d->have_data) ? d->nattr : 0;
}

- (id)tableView:(NSTableView *)tv objectValueForTableColumn:(NSTableColumn *)col
            row:(int)row
{
    const sm_disk *d = sm_find(g_dd_sel);
    const sm_attr *a;
    NSString      *cid = [col identifier];
    char           b[256];

    (void)tv;
    if (d == NULL || row < 0 || row >= d->nattr)
        return @"";
    a = &d->attrs[row];
    b[0] = 0;
    if ([cid isEqualToString:@"id"])
        snprintf(b, sizeof b, "%d", a->id);
    else if ([cid isEqualToString:@"name"])
        snprintf(b, sizeof b, "%s", sm_attr_name(a->id) ?
                 sm_attr_name(a->id) : "Vendor_Specific");
    else if ([cid isEqualToString:@"val"])
        snprintf(b, sizeof b, "%d", a->value);
    else if ([cid isEqualToString:@"wor"])
        snprintf(b, sizeof b, "%d", a->worst);
    else if ([cid isEqualToString:@"thr"])
        snprintf(b, sizeof b, a->has_thr ? "%d" : "?", a->thr);
    else if ([cid isEqualToString:@"raw"])
        snprintf(b, sizeof b, "%lu", (unsigned long)(a->raw & 0xffffffffULL));
    else if ([cid isEqualToString:@"type"])
        snprintf(b, sizeof b, "%s", (a->flags & 1) ? "pre-fail" : "old-age");
    else if ([cid isEqualToString:@"what"])
        snprintf(b, sizeof b, "%s", sm_explain(a->id));
    return u8(b);
}

- (void)tableView:(NSTableView *)tv willDisplayCell:(id)cell
   forTableColumn:(NSTableColumn *)col row:(int)row
{
    const sm_disk *d = sm_find(g_dd_sel);
    int            lv = SM_LV_OK;

    (void)tv;
    (void)col;
    if (d != NULL && row >= 0 && row < d->nattr)
        lv = d->attrs[row].level;
    if (![cell respondsToSelector:@selector(setDrawsBackground:)])
        return;
    if (lv == SM_LV_OK) {
        [cell setDrawsBackground:NO];
        [cell setTextColor:[NSColor controlTextColor]];
    } else {
        [cell setDrawsBackground:YES];
        [cell setBackgroundColor:(lv == SM_LV_FAIL)
            ? rgb(1.0f, 0.82f, 0.80f, 1.0f) : rgb(1.0f, 0.90f, 0.72f, 1.0f)];
        [cell setTextColor:(lv == SM_LV_FAIL)
            ? rgb(0.60f, 0.0f, 0.0f, 1.0f) : rgb(0.45f, 0.22f, 0.0f, 1.0f)];
    }
}

/* view вкладок сами не тянутся за NSTabView - выставляем contentRect */
- (void)windowDidResize:(NSNotification *)n
{
    NSRect cr;
    int    i;

    (void)n;
    if (g_tabs == NULL)
        return;
    cr = [g_tabs contentRect];
    for (i = 0; i < (int)[g_tabs numberOfTabViewItems]; i++) {
        NSView *v = [[g_tabs tabViewItemAtIndex:i] view];
        if (v != nil)
            [v setFrame:cr];
    }
    [g_sm_table sizeLastColumnToFit];
}

- (void)tick:(NSTimer *)timer
{
    int     done = 0;
    rndtab *rt;

    (void)timer;

    rt = rndtab_of_run(g_run);
    pthread_mutex_lock(&g_lock);
    if (g_run == RUN_LIN)
        done = (g_cur != NULL) ? g_cur->lin.done : 1;
    else if (rt != NULL && rt->dd != NULL)
        done = drnd_of(rt)->done;
    pthread_mutex_unlock(&g_lock);

    rnd_stats_update(&g_acc);
    rnd_stats_update(&g_seek);
    sm_poll();

    if (g_run != RUN_IDLE && done) {
        int was = g_run;
        g_run = RUN_IDLE;
        update_controls();
        if (g_again) {
            g_again = 0;
            printf("again: restart %d\n", was);
            fflush(stdout);
            if (was == RUN_LIN)
                lin_start();
            else
                rnd_start(rt);
        }
    }

    if (g_stop_at > 0 && time(NULL) >= g_stop_at) {
        g_stop_at = 0;
        if (g_run == RUN_LIN)
            [self linStop:nil];
        else if (g_run != RUN_IDLE)
            [self rndStop:nil];
    }

    if (g_snap_at > 0 && time(NULL) >= g_snap_at) {
        g_snap_at = 0;
        {
            NSView *cv = [g_win contentView];
            NSData *pdf = [cv dataWithPDFInsideRect:[cv bounds]];

            if (pdf != nil && [pdf writeToFile:@"/tmp/diskgraph-snap.pdf"
                                    atomically:YES]) {
                printf("snap: /tmp/diskgraph-snap.pdf written\n");
                fflush(stdout);
            } else {
                printf("snap: FAILED\n");
                fflush(stdout);
            }
        }
    }

    if (g_close_at > 0 && time(NULL) >= g_close_at) {
        g_close_at = 0;
        printf("close: performClose\n");
        fflush(stdout);
        [g_win performClose:nil];
        return;
    }

    status_all();
    [g_lin_view setNeedsDisplay:YES];
    [g_acc.view setNeedsDisplay:YES];
    [g_seek.view setNeedsDisplay:YES];

    if (g_cycle) {
        g_cycle_ticks++;
        if (g_cycle_ticks % (int)(3.0 / TICK_SEC) == 0) {
            int cur = (int)[g_tabs indexOfTabViewItem:
                                [g_tabs selectedTabViewItem]];
            int nxt = (cur + 1) % 3;
            [g_tabs selectTabViewItemAtIndex:nxt];
            pthread_mutex_lock(&g_lock);
            printf("cycle: tab %d -> %d   lin_n %d   acc_n %d   seek_n %d\n",
                   cur, nxt, g_cur->lin.pts.n, g_cur->acc.pts.n,
                   g_cur->seek.pts.n);
            fflush(stdout);
            pthread_mutex_unlock(&g_lock);
        }
    }
}

- (void)quit:(id)s
{
    (void)s;
    [NSApp terminate:nil];
}

/* NSApp delegate: закрыли окно - выходим (root-процесс не висит) */
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)a
{
    (void)a;
    return YES;
}

/* дать ядру выйти из pread и закрыть fd; не дольше секунды */
- (void)applicationWillTerminate:(NSNotification *)n
{
    rndtab *rt = rndtab_of_run(g_run);
    int     i, done = 1;

    (void)n;
    if (g_cur != NULL) {
        g_cur->lin.cancel = 1;
        g_cur->acc.cancel = 1;
        g_cur->seek.cancel = 1;
    }
    for (i = 0; i < 20; i++) {
        pthread_mutex_lock(&g_lock);
        if (g_run == RUN_LIN)
            done = (g_cur != NULL) ? g_cur->lin.done : 1;
        else if (rt != NULL && rt->dd != NULL)
            done = drnd_of(rt)->done;
        pthread_mutex_unlock(&g_lock);
        if (done)
            break;
        usleep(50000);
    }
}

@end

/* ------------------------------------------------------------ сборка UI */

static void tip(NSView *v, const char *s)
{
    /* NSView setToolTip: - с 10.0; ярлыки вкладок тултипов не умеют
     * до 10.6, поэтому подсказки висят на контролах и графике */
    [v setToolTip:[NSString stringWithUTF8String:s]];
}

static NSTextField *make_label(NSView *v, const char *txt,
                               float x, float y, float w, int right)
{
    NSTextField *lab = [[NSTextField alloc]
        initWithFrame:NSMakeRect(x, y, w, 17.0f)];
    [lab setStringValue:[NSString stringWithUTF8String:txt]];
    [lab setFont:[NSFont systemFontOfSize:12.0f]];
    [lab setBezeled:NO];
    [lab setEditable:NO];
    [lab setSelectable:NO];
    [lab setDrawsBackground:NO];
    if (right)
        [lab setAlignment:NSRightTextAlignment];
    [lab setAutoresizingMask:NSViewMinYMargin];
    [v addSubview:lab];
    return [lab autorelease];
}

static NSButton *make_button(NSView *v, Controller *ctl, const char *title,
                             SEL act, float x, float y)
{
    NSButton *b = [[NSButton alloc]
        initWithFrame:NSMakeRect(x, y, 96.0f, 32.0f)];
    [b setBezelStyle:NSRoundedBezelStyle];
    [b setTitle:[NSString stringWithUTF8String:title]];
    [b setTarget:ctl];
    [b setAction:act];
    [b setAutoresizingMask:NSViewMinXMargin | NSViewMinYMargin];
    [v addSubview:b];
    return [b autorelease];
}

static const char *k_tip_dd =
    "Выбор диска для всех вкладок сразу. Замер всегда идёт по raw-ноде\n"
    "/dev/rdiskN выбранного диска: чтение прямо с диска, без кеша ядра.\n"
    " Один прогон одновременно; во время прогона выбор заблокирован.";

/* что даёт вкладка и как выглядит норма - серая строка под контролами */
static const char *k_desc_seq =
    "Скорость чтения подряд по всей поверхности: как быстро диск читает "
    "большие файлы. Норма - плавный спад от начала к концу (внешние "
    "дорожки быстрее); провалы - медленные или переназначенные участки.";
static const char *k_desc_acc =
    "Время доступа в случайные точки поверхности: как диск справляется с "
    "мелкими разбросанными файлами. Норма - ровная полоса по всему диску "
    "(у краёв чуть выше - это геометрия); приподнятый участок или красные "
    "крестики - зона, где сбоят позиционер или поверхность.";
static const char *k_desc_seek =
    "Время прыжка головки в зависимости от его длины: механика "
    "позиционера. Норма - плавный рост от долей ms на коротких прыжках до "
    "полного хода; ступеньки и разрывы - проблемы механики.";
static const char *k_desc_smart =
    "Самодиагностика: счётчики ошибок и износа, которые диск ведёт сам. "
    "Только чтение - тесты не запускаются, состояние диска не меняется.";

#define DESC_H 30.0f                /* две строки по 11 pt */

/* серая строка описания; top - посадка верхнего края: на вкладках
 * замеров 44 (под рядом контролей), на SMART - плотнее */
static void make_desc(NSView *v, NSSize sz, const char *txt, float top)
{
    NSTextField *t = [[NSTextField alloc] initWithFrame:
        NSMakeRect(14.0f, sz.height - top - DESC_H, sz.width - 28.0f,
                   DESC_H)];
    [t setStringValue:u8(txt)];
    [t setFont:[NSFont systemFontOfSize:11.0f]];
    [t setTextColor:[NSColor colorWithCalibratedWhite:0.40f alpha:1.0f]];
    [t setBezeled:NO];
    [t setEditable:NO];
    [t setSelectable:NO];
    [t setDrawsBackground:NO];
    [t setAutoresizingMask:NSViewWidthSizable | NSViewMinYMargin];
    [v addSubview:t];
    [t release];
}

/*
 * Вкладка: ряд контролов (диск, слайдер, Старт/Стоп), описание, статус,
 * график на остальное место. t == NULL - Sequential read, иначе random.
 */
static NSView *make_tab(Controller *ctl, NSSize sz, rndtab *t)
{
    NSView        *v = [[NSView alloc] initWithFrame:
                            NSMakeRect(0.0f, 0.0f, sz.width, sz.height)];
    float          h = sz.height, w = sz.width;
    float          y = h - 36.0f;
    int            lin = (t == NULL);
    NSSlider      *sl;
    NSTextField   *val, *st;
    NSView        *gv;
    NSButton      *bstart, *bstop;
    char           lbl[32];
    /* у вкладок своего селектора диска нет: замер идёт по /dev/rdiskN
     * диска, выбранного в шапке */

    make_label(v, lin ? "Duty:" : "Время:", 14.0f, y + 4.0f, 52.0f, 1);
    sl = [[NSSlider alloc] initWithFrame:
        NSMakeRect(70.0f, y + 1.0f, 170.0f, 22.0f)];
    [sl setMinValue:lin ? 1.0 : 5.0];
    [sl setMaxValue:lin ? 100.0 : 120.0];
    [sl setIntValue:lin ? g_duty : t->secs];
    [sl setTarget:ctl];
    [sl setAction:lin ? @selector(dutyChanged:) : @selector(secsChanged:)];
    [sl setAutoresizingMask:NSViewMinYMargin];
    tip(sl, lin ? "Какую долю поверхности прочитать: сегменты по 32 MB "
                  "равномерно по всему диску. 5% - быстрый профиль зон, "
                  "100% - сплошной проход."
                : "Длительность прогона в секундах.");
    [v addSubview:sl];
    [sl release];

    if (lin)
        snprintf(lbl, sizeof lbl, "%d %%", g_duty);
    else
        snprintf(lbl, sizeof lbl, "%d с", t->secs);
    val = make_label(v, lbl, 246.0f, y + 4.0f, 60.0f, 0);
    [val setFont:[NSFont boldSystemFontOfSize:12.0f]];

    st = [[NSTextField alloc]
        initWithFrame:NSMakeRect(14.0f, h - 66.0f - DESC_H, w - 28.0f,
                                 17.0f)];
    [st setBezeled:NO];
    [st setEditable:NO];
    [st setSelectable:NO];
    [st setDrawsBackground:NO];
    [st setAutoresizingMask:NSViewWidthSizable | NSViewMinYMargin];
    [v addSubview:st];
    [st release];

    gv = [[(lin ? [LinView class] : [RndView class]) alloc] initWithFrame:
        NSMakeRect(0.0f, 0.0f, w, h - 72.0f - DESC_H)];
    [gv setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [v addSubview:gv];
    [gv release];

    bstart = make_button(v, ctl, "Старт",
                         lin ? @selector(linStart:) : @selector(rndStart:),
                         w - 206.0f, y - 4.0f);
    bstop  = make_button(v, ctl, "Стоп",
                         lin ? @selector(linStop:) : @selector(rndStop:),
                         w - 108.0f, y - 4.0f);
    tip(bstop, "Остановить прогон; уже снятые точки остаются на графике.");

    make_desc(v, sz, lin ? k_desc_seq
                         : (t->mode == DGRND_SEEK ? k_desc_seek : k_desc_acc),
              44.0f);

    if (lin) {
        g_lin_slider = sl; g_duty_val = val;
        g_lin_status = st; g_lin_view = gv;
        g_lin_start = bstart; g_lin_stop = bstop;
        tip(bstart, "Sequential read: сегменты по 32 MB блоками 1 MB "
                    "равномерно от начала до конца диска. Кривая MB/s "
                    "показывает зоны: снаружи быстрее, к центру медленнее.");
        tip(gv, "Скорость последовательного чтения (MB/s) по позиции на "
                "диске.\nПунктир - средняя скорость, красные крестики - "
                "ошибки чтения (EIO).");
    } else {
        t->slider = sl; t->val = val;
        t->status = st; t->view = gv;
        t->start = bstart; t->stop = bstop;
        [(RndView *)gv setTab:t];
        if (t->mode == DGRND_SEEK) {
            tip(bstart, "Seek profile: цепочка хопов - следующее чтение 4 KB "
                        "на +-d от предыдущего, d log-равномерно от 8 KB до "
                        "всей ёмкости. Меряет время перемещения головки в "
                        "зависимости от дистанции.");
            tip(gv, "Время хопа (ms) vs дистанция (лог. шкала).\n"
                    "Зелёная линия - медиана по дистанции (seek + "
                    "в среднем полоборота), оранжевый пунктир - p10, "
                    "близко к чистому seek.\n"
                    "Короткие хопы вперёд могут попасть в кеш диска "
                    "(доли ms).");
        } else {
            tip(bstart, "Access map: чтения 4 KB в равномерно случайные "
                        "точки всего диска. Время доступа = seek + ожидание "
                        "вращения + передача; медиана - классическая цифра "
                        "random access time.");
            tip(gv, "Время доступа (ms) по позиции на диске.\n"
                    "Линии - медиана и p10 по полосам позиции: горб на "
                    "них - зона, куда головка приходит хуже.\n"
                    "Красные точки - выше p99, крестики внизу - ошибки "
                    "чтения (EIO).");
        }
    }
    return [v autorelease];
}

static NSTextField *make_text(NSView *v, NSRect r, float size, int bold)
{
    NSTextField *t = [[NSTextField alloc] initWithFrame:r];
    [t setStringValue:@""];
    [t setFont:bold ? [NSFont boldSystemFontOfSize:size]
                    : [NSFont systemFontOfSize:size]];
    [t setBezeled:NO];
    [t setEditable:NO];
    [t setSelectable:NO];
    [t setDrawsBackground:NO];
    [t setAutoresizingMask:NSViewWidthSizable | NSViewMinYMargin];
    [v addSubview:t];
    return [t autorelease];
}

static void sm_column(NSTableView *tv, NSString *ident, const char *title,
                      float w, int right)
{
    NSTableColumn *c = [[NSTableColumn alloc] initWithIdentifier:ident];
    [[c headerCell] setStringValue:[NSString stringWithUTF8String:title]];
    [c setWidth:w];
    [c setMinWidth:24.0f];
    [c setEditable:NO];
    [[c dataCell] setFont:[NSFont systemFontOfSize:11.0f]];
    if (right)
        [[c dataCell] setAlignment:NSRightTextAlignment];
    [tv addTableColumn:c];
    [c release];
}

/*
 * Вкладка SMART: плашка вердикта, паспорт, пояснение находок, таблица
 * атрибутов. Диск берётся из шапки (g_dd_sel); «Обновить» - в шапке.
 * Только чтение SMART.
 */
static NSView *make_smart_tab(Controller *ctl, NSSize sz)
{
    NSView       *v = [[NSView alloc] initWithFrame:
                           NSMakeRect(0.0f, 0.0f, sz.width, sz.height)];
    float         h = sz.height, w = sz.width;
    NSScrollView *sv;
    NSTableView  *tv;

    /* стопка: описание 26..56, зазор 12, плашка/паспорт 68..106,
     * зазор 12, объяснение 118..154, таблица с 162 */
    make_desc(v, sz, k_desc_smart, 26.0f);

    g_sm_verdict = make_text(v, NSMakeRect(14.0f, h - 100.0f, 90.0f, 32.0f),
                             26.0f, 1);
    tip(g_sm_verdict, "OK - критичные счётчики нулевые; WARN - есть "
                      "переназначенные/нечитаемые сектора или CRC-ошибки "
                      "шлейфа; FAIL - pending-сектора или атрибут на пороге.");
    g_sm_note = make_text(v, NSMakeRect(110.0f, h - 88.0f, w - 124.0f, 18.0f),
                          13.0f, 1);
    g_sm_ident = make_text(v, NSMakeRect(110.0f, h - 106.0f, w - 124.0f, 17.0f),
                           11.0f, 0);
    g_sm_explain = make_text(v, NSMakeRect(14.0f, h - 154.0f, w - 28.0f,
                                           36.0f), 11.0f, 0);
    [g_sm_explain setTextColor:
        [NSColor colorWithCalibratedWhite:0.25f alpha:1.0f]];

    sv = [[NSScrollView alloc] initWithFrame:
        NSMakeRect(8.0f, 8.0f, w - 16.0f, h - 170.0f)];
    [sv setHasVerticalScroller:YES];
    if ([sv respondsToSelector:@selector(setAutohidesScrollers:)])
        [sv setAutohidesScrollers:YES];         /* 10.3+ */
    [sv setBorderType:NSBezelBorder];
    [sv setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];

    tv = [[NSTableView alloc] initWithFrame:[[sv contentView] bounds]];
    sm_column(tv, @"id",   "ID",       34.0f, 1);
    sm_column(tv, @"name", "Атрибут", 170.0f, 0);
    sm_column(tv, @"val",  "Value",    46.0f, 1);
    sm_column(tv, @"wor",  "Worst",    46.0f, 1);
    sm_column(tv, @"thr",  "Thr",      38.0f, 1);
    sm_column(tv, @"raw",  "Raw",      84.0f, 1);
    sm_column(tv, @"type", "Тип",      64.0f, 0);
    sm_column(tv, @"what", "Что это", 300.0f, 0);
    [tv setDataSource:ctl];
    [tv setDelegate:ctl];
    [tv setAllowsColumnReordering:NO];
    [tv setAutoresizingMask:NSViewWidthSizable];
    [tv setRowHeight:16.0f];
    tip(tv, "Value/Worst - нормированные вендором значения (больше - "
            "лучше), Thr - порог; Raw - сырой счётчик (младшие 32 бита). "
            "Подсвечены строки, из-за которых вердикт WARN/FAIL.");
    [sv setDocumentView:tv];
    [tv sizeLastColumnToFit];
    g_sm_table = tv;
    [tv release];
    [v addSubview:sv];
    [sv release];

    return [v autorelease];
}

/* ------------------------------------------------------- подъём прав */

/*
 * Перезапуск себя с правами root через Authorization Services:
 * пароль спрашивает системный SecurityAgent, в коде пароля нет.
 * Для автотестов можно задать DISKGRAPH_AUTH_USER/DISKGRAPH_AUTH_PASS,
 * тогда диалог не появляется.
 * DISKGRAPH_NOELEVATE=1 - не поднимать права (путь ошибки EACCES).
 *
 * Возврат 1 - root-экземпляр запущен, текущему пора выходить;
 *         0 - не вышло, продолжаем без прав (чтение упрётся в EACCES).
 */
static int relaunch_as_root(int argc, char **argv)
{
    AuthorizationRef      auth = NULL;
    AuthorizationItem     items[1];
    AuthorizationRights   rights;
    AuthorizationEnvironment env;
    AuthorizationItem     envitems[2];
    OSStatus              st;
    char                  path[1024];
    uint32_t              sz = sizeof path;
    char                **args;
    int                   i, n = 0, rc = 0;
    const char           *u = getenv("DISKGRAPH_AUTH_USER");
    const char           *p = getenv("DISKGRAPH_AUTH_PASS");

    if (_NSGetExecutablePath(path, &sz) != 0)
        return 0;

    /* -psn_X_Y Finder выдал этому процессу; root-копии он чужой */
    args = (char **)calloc((size_t)argc + 1, sizeof(char *));
    if (args == NULL)
        return 0;
    for (i = 1; i < argc; i++)
        if (strncmp(argv[i], "-psn_", 5) != 0)
            args[n++] = argv[i];
    args[n] = NULL;

    memset(items, 0, sizeof items);
    items[0].name        = kAuthorizationRightExecute;
    items[0].valueLength = strlen(path);
    items[0].value       = path;
    rights.count = 1;
    rights.items = items;

    if (u != NULL && p != NULL) {
        envitems[0].name        = kAuthorizationEnvironmentUsername;
        envitems[0].valueLength = strlen(u);
        envitems[0].value       = (void *)u;
        envitems[0].flags       = 0;
        envitems[1].name        = kAuthorizationEnvironmentPassword;
        envitems[1].valueLength = strlen(p);
        envitems[1].value       = (void *)p;
        envitems[1].flags       = 0;
        env.count = 2;
        env.items = envitems;
        st = AuthorizationCreate(&rights, &env,
                kAuthorizationFlagInteractionAllowed |
                kAuthorizationFlagExtendRights, &auth);
    } else {
        st = AuthorizationCreate(&rights, kAuthorizationEmptyEnvironment,
                kAuthorizationFlagInteractionAllowed |
                kAuthorizationFlagExtendRights, &auth);
    }
    if (st != errAuthorizationSuccess) {
        fprintf(stderr, "diskgraph: authorization failed (%ld)\n", (long)st);
        free(args);
        if (auth != NULL)
            AuthorizationFree(auth, kAuthorizationFlagDefaults);
        return 0;
    }

    st = AuthorizationExecuteWithPrivileges(auth, path,
            kAuthorizationFlagDefaults, args, NULL);
    if (st == errAuthorizationSuccess)
        rc = 1;
    else
        fprintf(stderr, "diskgraph: cannot relaunch as root (%ld)\n",
                (long)st);

    AuthorizationFree(auth, kAuthorizationFlagDefaults);
    free(args);
    return rc;
}

static int has_arg(int argc, char **argv, const char *a)
{
    int i;
    for (i = 1; i < argc; i++)
        if (strcmp(argv[i], a) == 0)
            return 1;
    return 0;
}

static void add_tab(Controller *ctl, NSSize sz, rndtab *t, const char *label)
{
    NSTabViewItem *item = [[NSTabViewItem alloc] initWithIdentifier:nil];
    [item setLabel:[NSString stringWithUTF8String:label]];
    [item setView:make_tab(ctl, sz, t)];
    [g_tabs addTabViewItem:item];
    [item release];
}

int main(int argc, char *argv[])
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    NSWindow      *win;
    NSRect         rc = NSMakeRect(0.0f, 0.0f, WIN_W, WIN_H);
    NSView        *root;
    Controller    *ctl;
    NSMenu        *menu, *submenu;
    NSMenuItem    *mi;
    NSTimer       *timer;
    NSSize         csz;
    int            sized = 0, i;

    /* -devs: список дисков из реестра, без окна и без прав */
    if (has_arg(argc, argv, "-devs")) {
        char names[SM_MAX][16];
        int  n = sm_scan(names, SM_MAX);

        for (i = 0; i < n; i++) {
            sm_disk d;
            sm_probe(names[i], &d);
            printf("%s   %s   %.1f GB   %s", d.name,
                   d.model[0] ? d.model : "-",
                   (double)d.bytes / GIB, sm_verdict_str(d.vd));
            if (d.bus[0])
                printf("  %s", d.bus);
            printf("\n");
        }
        [pool release];
        return 0;
    }

    /* чтение /dev/rdiskN требует root: поднимаем права при запуске */
    if (geteuid() != 0 && getenv("DISKGRAPH_NOELEVATE") == NULL &&
        relaunch_as_root(argc, argv)) {
        [pool release];
        return 0;
    }
    g_is_root = (geteuid() == 0) ? 1 : 0;

    rndtab_init(&g_acc,  DGRND_ACCESS, RUN_ACC,  "Access map",   30);
    rndtab_init(&g_seek, DGRND_SEEK,   RUN_SEEK, "Seek profile", 30);

    /* dev-хук: -w N -h N задают стартовый размер (проверка раскладки) */
    for (i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "-w") == 0) {
            rc.size.width = (float)atof(argv[i + 1]);
            sized = 1;
        } else if (strcmp(argv[i], "-h") == 0) {
            rc.size.height = (float)atof(argv[i + 1]);
            sized = 1;
        }
    }

    [NSApplication sharedApplication];
    ctl = [[Controller alloc] init];
    [NSApp setDelegate:ctl];

    menu = [[NSMenu alloc] initWithTitle:
                [NSString stringWithUTF8String:"main"]];
    {
        NSMenuItem *appmi = [menu addItemWithTitle:
            [NSString stringWithUTF8String:"DiskGraph"]
            action:NULL keyEquivalent:[NSString stringWithUTF8String:""]];
        submenu = [[NSMenu alloc] initWithTitle:
            [NSString stringWithUTF8String:"DiskGraph"]];
        [submenu addItemWithTitle:
            [NSString stringWithUTF8String:"Закрыть окно"]
            action:@selector(performClose:)
            keyEquivalent:[NSString stringWithUTF8String:"w"]];
        mi = [submenu addItemWithTitle:
            [NSString stringWithUTF8String:"Выход"]
            action:@selector(quit:)
            keyEquivalent:[NSString stringWithUTF8String:"q"]];
        [mi setTarget:ctl];
        [appmi setSubmenu:submenu];
        /* без nib Cocoa не знает, что это меню приложения, и рисует
         * второй "DiskGraph"; setAppleMenu: есть, но не в заголовках */
        if ([NSApp respondsToSelector:@selector(setAppleMenu:)])
            [NSApp performSelector:@selector(setAppleMenu:)
                        withObject:submenu];
        [submenu release];
    }
    [NSApp setMainMenu:menu];
    [menu release];

    win = [[NSWindow alloc]
             initWithContentRect:rc
             styleMask:NSTitledWindowMask |
                       NSClosableWindowMask |
                       NSMiniaturizableWindowMask |
                       NSResizableWindowMask
             backing:NSBackingStoreBuffered
             defer:NO];
    [win setTitle:[NSString stringWithUTF8String:"DiskGraph"]];
    g_win = win;
    [win setMinSize:NSMakeSize(840.0f, 550.0f)];
    root = [[[NSView alloc] initWithFrame:rc] autorelease];
    [root setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [win setContentView:root];

    /* шапка над вкладками: глобальный выбор диска + Обновить.
     * Диск один на все вкладки; отдельный прогон - тоже один */
    {
        NSRect rb = [root bounds];

        make_label(root, "Диск:", 10.0f, rb.size.height - 27.0f, 40.0f, 1);
        g_dd_dev = [[NSPopUpButton alloc]
            initWithFrame:NSMakeRect(54.0f, rb.size.height - 31.0f,
                                     320.0f, 26.0f) pullsDown:NO];
        [g_dd_dev setTarget:ctl];
        [g_dd_dev setAction:@selector(devChanged:)];
        [g_dd_dev setAutoresizingMask:NSViewMinYMargin];
        tip(g_dd_dev, k_tip_dd);
        [root addSubview:g_dd_dev];
        [g_dd_dev release];

        g_sm_refresh = make_button(root, ctl, "Обновить",
                                   @selector(smRefresh:),
                                   rb.size.width - 108.0f,
                                   rb.size.height - 34.0f);
        tip(g_sm_refresh, "Пересканировать диски и перечитать SMART "
                          "всех дисков. Только чтение: статус, атрибуты, "
                          "пороги - тесты не запускаются.");
    }

    /* вкладки с отступом от краёв окна и под шапкой */
    g_tabs = [[NSTabView alloc] initWithFrame:
                 NSMakeRect(8.0f, 6.0f,
                            [root bounds].size.width - 16.0f,
                            [root bounds].size.height - 44.0f)];
    [g_tabs setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [root addSubview:g_tabs];

    csz = [g_tabs contentRect].size;
    add_tab(ctl, csz, NULL,    "Sequential read");
    add_tab(ctl, csz, &g_acc,  "Access map");
    add_tab(ctl, csz, &g_seek, "Seek profile");
    g_sm_tab = [[NSTabViewItem alloc] initWithIdentifier:nil];
    [g_sm_tab setLabel:[NSString stringWithUTF8String:"SMART"]];
    [g_sm_tab setView:make_smart_tab(ctl, csz)];
    [g_tabs addTabViewItem:g_sm_tab];
    [ctl windowDidResize:nil];

    /* выбор диска: пока смарт-опрос не отработал, в шапке заглушка;
     * после первого поколения список появится, выбор сохранится */
    dd_repopulate();
    dd_select();
    update_controls();
    status_all();
    sm_refresh();                   /* SMART всех дисков - фоном */

    /* dev-хуки: -dev diskN (или rdiskN) - выбрать диск для всех вкладок;
     * -lin/-acc/-seek стартуют реальный прогон; -cycle щёлкает вкладки
     * и печатает счётчики выбранного диска */
    for (i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "-dev") == 0) {
            char nm[16];
            /* rdisk1 -> disk1: обе проекции - один диск */
            dev_to_disk(argv[i + 1], nm, sizeof nm);
            snprintf(g_dd_sel, sizeof g_dd_sel, "%s", nm);
            dd_repopulate();
            dd_select();
        } else if (strcmp(argv[i], "-duty") == 0) {
            char lbl[16];
            g_duty = atoi(argv[i + 1]);
            if (g_duty < 1) g_duty = 1;
            if (g_duty > 100) g_duty = 100;
            [g_lin_slider setIntValue:g_duty];
            snprintf(lbl, sizeof lbl, "%d %%", g_duty);
            [g_duty_val setStringValue:u8(lbl)];
        }
    }
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-lin") == 0) {
            [g_tabs selectTabViewItemAtIndex:0];
            lin_start();
        } else if (strcmp(argv[i], "-acc") == 0) {
            [g_tabs selectTabViewItemAtIndex:1];
            rnd_start(&g_acc);
        } else if (strcmp(argv[i], "-seek") == 0) {
            [g_tabs selectTabViewItemAtIndex:2];
            rnd_start(&g_seek);
        } else if (strcmp(argv[i], "-smart") == 0) {
            [g_tabs selectTabViewItemAtIndex:3];
        } else if (strcmp(argv[i], "-tab") == 0 && i + 1 < argc) {
            [g_tabs selectTabViewItemAtIndex:atoi(argv[i + 1])];
            i++;
        } else if (strcmp(argv[i], "-stop") == 0 && i + 1 < argc) {
            g_stop_at = time(NULL) + atoi(argv[i + 1]);
            i++;
        } else if (strcmp(argv[i], "-close") == 0 && i + 1 < argc) {
            g_close_at = time(NULL) + atoi(argv[i + 1]);
            i++;
        } else if (strcmp(argv[i], "-snap") == 0 && i + 1 < argc) {
            g_snap_at = time(NULL) + atoi(argv[i + 1]);
            i++;
        } else if (strcmp(argv[i], "-again") == 0) {
            g_again = 1;
        } else if (strcmp(argv[i], "-cycle") == 0) {
            g_cycle = 1;
            lin_start();
        }
    }

    /* позиция/размер окна запоминаются, если не заданы -w/-h */
    if (sized || ![win setFrameUsingName:@"DiskGraphMain"])
        [win center];
    if (!sized)
        [win setFrameAutosaveName:@"DiskGraphMain"];
    [win makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];

    timer = [NSTimer timerWithTimeInterval:TICK_SEC
                                    target:ctl
                                  selector:@selector(tick:)
                                  userInfo:nil
                                   repeats:YES];
    [[NSRunLoop currentRunLoop] addTimer:timer
                    forMode:NSDefaultRunLoopMode];
    /* таймер тикает и пока тянут ресайз окна/слайдер */
    [[NSRunLoop currentRunLoop] addTimer:timer
                    forMode:NSEventTrackingRunLoopMode];

    [NSApp run];

    [g_tabs release];
    [ctl release];
    [win release];
    [pool release];
    return 0;
}
