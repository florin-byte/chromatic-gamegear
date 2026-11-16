/* sd.c: lane sdcard1: the microSD card service: its work task (started when asked, gone when idle:
 * its stack is heap the TLS calls need the rest of the time), the OSD's view, the receive summary
 * and the `sd` console command.
 *
 *   sd st                      state, size, free space, games
 *   sd mount | umount          bring the card up / let it go
 *   sd off | on                bench: act as if no card were in (RAM only, until on or a restart)
 *   sd lib [force]             the library refreshed (rebuilt if the card changed, or always)
 *   sd list [FROM [N]]         records of the index
 *   sd ls [DIR]                a folder
 *   sd format [exfat] yes      FORMAT CARD (FAT32 up to 32 GB, exFAT above; exfat = exFAT always)
 *   sd rm PATH                 a file deleted
 *   sd mv FROM TO              fr3j: a file or a folder moved (TO's folders made; never onto an existing name)
 *   sd backup | tocart PATH    the cart in the slot onto the card / a game on the card onto the cart
 *   sd wtest PATH KIB          a test file written (a counting pattern) and read back: MB/s both ways
 *   sd rtest PATH              a file read through: MB/s
 *   sd sum PATH                crc32 of a file
 *   sd import FLASHADDR LEN NAME   a game staged in this ESP32's flash (the USB route) copied to /GAMES/NAME
 *   sd imp begin NAME | part PATH FLASHADDR LEN OFF | end PATH CRC32 | abort PATH   the same in parts (a game bigger than the
 *                              staging area, staged and copied part by part; PATH.new on the card holds the parts
 *                              across restarts; the file appears whole at `end`, checked by crc32)
 *   sd pins | mmc W KHZ        the wiring probe (sd_probe.c)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_console.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "driver/sdmmc_host.h"
#include "hal/sdmmc_ll.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sd.h"
#include "sd_probe.h"

static struct {
    volatile int op, pct;
    char *path;                         /* (heap, while an op needs it) */
    char result[48];
    TaskHandle_t task;
    /* lane ui110dev: a GAMES job (kSdOp_Job) */
    int job, jflags;
    uint32_t juid;
    char jtitle[24];
    const char *jstep;                  /* a literal */
} _T;
static portMUX_TYPE _TMux = portMUX_INITIALIZER_UNLOCKED;
static const sd_cart_hooks_t *_Hooks;

void Sd_SetCartHooks(const sd_cart_hooks_t *h) { _Hooks = h; }
/* (asked of the FPGA at most once a second: the OSD asks on every draw) */
int Sd_CartIn(void)
{
    static int64_t at;
    static int last;
    const int64_t now = esp_timer_get_time();
    if (at && now - at < 1000000) return last;
    at = now ? now : 1;
    last = _Hooks && _Hooks->cart_in && _Hooks->cart_in();
    return last;
}
int Sd_Busy(void) { return _T.op; }
void Sd_ClearResult(void) { _T.result[0] = 0; }
static void pct(int p) { _T.pct = p; }
static void res(const char *r) { snprintf(_T.result, sizeof _T.result, "%s", r); }

uint64_t Sd_UnixTime(void)
{
    const time_t t = time(NULL);
    return t > 1735689600 ? (uint64_t)t : 0;         /* (after 2025-01-01: a clock that was set) */
}

static void do_op(int op)
{
    switch (op) {
    case kSdOp_Mount: {
        const int m = Sd_Mount();
        if (m == 0) (void)SdLib_Refresh(0);
        break;
    }
    case kSdOp_Refresh:
        if (Sd_Mount() == 0) (void)SdLib_Refresh(0);
        break;
    case kSdOp_Look: {                               /* 1.1.6: the card still there (CMD13); a card mounted now: its games */
        const uint32_t g = Sd_Generation();
        if (Sd_Mount() == 0 && Sd_Generation() != g) (void)SdLib_Refresh(0);
        break;
    }
    case kSdOp_Format: case kSdOp_FormatExfat: {
        _T.pct = 0;
        const int r = Sd_Format(op == kSdOp_FormatExfat, pct);
        if (r == 0) { _T.pct = 95; (void)SdLib_Refresh(1); res("CARD  FORMATTED|"); }
        else res(Sd_State() == kSdSt_ReadOnly ? "NOT  FORMATTED|THE  CARD  IS  LOCKED" : Sd_State() == kSdSt_None ? "NOT  FORMATTED|NO  CARD" : "NOT  FORMATTED|TRY  AGAIN");
        break;
    }
    case kSdOp_Delete:
        if (Sd_Mount() == 0 && Sd_Delete(_T.path) == 0) { (void)SdLib_Refresh(0); Sd_RefreshSpace(); res("GAME  DELETED|ITS  SAVE  STAYS"); }
        else res("NOT  DELETED|TRY  AGAIN");
        break;
    case kSdOp_Backup:
        if (!_Hooks || !_Hooks->backup) { res("NOT  BACKED  UP|"); break; }
        if (Sd_Mount() != 0 || !Sd_Writable()) { res("NOT  BACKED  UP|CARD  NOT  READY"); break; }
        _T.pct = 0;
        { char r[48] = ""; const int e = _Hooks->backup(pct, r, sizeof r); if (e == 0) { (void)SdLib_Refresh(0); Sd_RefreshSpace(); } res(r); }
        break;
    case kSdOp_ToCart:
        if (!_Hooks || !_Hooks->to_cart) { res("NOT  WRITTEN|"); break; }
        _T.pct = 0;
        { char r[48] = ""; (void)_Hooks->to_cart(_T.path, pct, r, sizeof r); res(r); }
        break;
    case kSdOp_Job:
        if (!_Hooks || !_Hooks->job) { res("NOT  DONE|"); break; }
        _T.pct = 0;
        { char r[48] = ""; const int e = _Hooks->job(_T.job, _T.path, _T.juid, _T.jflags, _T.jtitle, pct, r, sizeof r); if (e == 0) Sd_RefreshSpace(); res(r); }
        _T.jstep = NULL;
        break;
    default: break;
    }
}

static void sd_task(void *arg)
{
    (void)arg;
    for (;;) {
        const int op = _T.op;
        if (op) {
            _T.pct = -1;
            do_op(op);
            _T.pct = -1;
            portENTER_CRITICAL(&_TMux);
            char *p = _T.path;
            _T.path = NULL;
            _T.op = kSdOp_None;
            portEXIT_CRITICAL(&_TMux);
            free(p);
        }
        int bye = 0;
        portENTER_CRITICAL(&_TMux);
        if (_T.op == kSdOp_None) { _T.task = NULL; bye = 1; }
        portEXIT_CRITICAL(&_TMux);
        if (bye) vTaskDelete(NULL);
    }
}

void Sd_LentClear(void);   /* sd_card.c */
int Sd_TakeBack(void)
{
    if (!Sd_Lent()) { Sd_LentClear(); return 0; }
    if (Sd_Ask(kSdOp_Mount, NULL) != 0) return -1;
    Sd_LentClear();
    printf("sd: the card back after a load / an install (mount asked)\n");
    return 0;
}

int Sd_Ask(int op, const char *path)
{
    int start = 0;
    char *dup = NULL;
    if (path && !(dup = strdup(path))) return -2;
    portENTER_CRITICAL(&_TMux);
    if (_T.op != kSdOp_None) { portEXIT_CRITICAL(&_TMux); free(dup); return -1; }
    char *old = _T.path;
    _T.path = dup;
    _T.op = op;
    start = _T.task == NULL;
    if (start) _T.task = (TaskHandle_t)1;
    portEXIT_CRITICAL(&_TMux);
    free(old);
    if (op == kSdOp_Format || op == kSdOp_FormatExfat || op == kSdOp_Backup || op == kSdOp_ToCart || op == kSdOp_Delete || op == kSdOp_Job) _T.result[0] = 0;
    if (start) {
        TaskHandle_t h = NULL;
        /* 4 KiB: FatFs keeps its long-name buffers on the heap; the walk's state is heap too */
        if (xTaskCreate(sd_task, "sd", 4096, NULL, 3, &h) != pdPASS) {
            portENTER_CRITICAL(&_TMux); _T.task = NULL; _T.op = kSdOp_None; portEXIT_CRITICAL(&_TMux);
            return -2;
        }
        _T.task = h;
    }
    return 0;
}

/* lane ui110dev: a GAMES job (its words and title stay for the page until the next one) */
int Sd_AskJob(int kind, const char *path, uint32_t uid, int flags, const char *title)
{
    if (_T.op != kSdOp_None) return -1;
    _T.job = kind; _T.juid = uid; _T.jflags = flags; _T.jstep = NULL;
    snprintf(_T.jtitle, sizeof _T.jtitle, "%s", title ? title : "");
    const int r = Sd_Ask(kSdOp_Job, path);
    if (r == 0) printf("sd: job %d asked (%s%s)\n", kind, _T.jtitle, (flags & kSdJf_BackupFirst) ? ", the cartridge backed up first" : "");
    return r;
}
int Sd_JobKind(void) { return _T.job; }
const char *Sd_JobTitle(void) { return _T.jtitle; }
const char *Sd_JobStep(void) { const char *s = _T.jstep; return s ? s : ""; }
void Sd_JobSetStep(const char *words) { _T.jstep = words; }
int Sd_SetJobTitle(const char *title) { if (_T.op != kSdOp_None) return -1; _T.job = kSdJob_None; _T.jstep = NULL; snprintf(_T.jtitle, sizeof _T.jtitle, "%s", title ? title : ""); return 0; }

void Sd_View(sd_view_t *v)
{
    memset(v, 0, sizeof *v);
    v->st = Sd_State();
    v->games = Sd_Mounted() ? SdLib_Count() : -1;
    Sd_Space(&v->total_mb, &v->free_mb);
    v->exfat = Sd_IsExfat();
    v->op = _T.op;
    v->pct = _T.op == kSdOp_Mount || _T.op == kSdOp_Refresh ? SdLib_Building() : _T.pct;
    snprintf(v->result, sizeof v->result, "%s", _T.result);
}

void Sd_RecvSummary(char *json, size_t n, const char **caps)
{
    *caps = "";
    json[0] = 0;
    {   /* lane ui110dev: a card lent for the heap is still in the unit: its summary as it was */
        int wr = 0, g = 0;
        uint32_t t = 0, f = 0;
        if (Sd_LentInfo(&wr, &g, &t, &f)) {
            if (wr) *caps = ",sd1";
            snprintf(json, n, ",\"sd\":{\"games\":%d,\"free_mb\":%lu,\"mb\":%lu%s}", g < 0 ? 0 : g, (unsigned long)f, (unsigned long)t, wr ? "" : ",\"ro\":1");
            return;
        }
    }
    if (!Sd_Present()) return;
    uint32_t t = 0, f = 0;
    Sd_Space(&t, &f);
    int g = SdLib_Count();
    if (g < 0) g = 0;
    if (Sd_Writable()) *caps = ",sd1";
    snprintf(json, n, ",\"sd\":{\"games\":%d,\"free_mb\":%lu,\"mb\":%lu%s}", g, (unsigned long)f, (unsigned long)t, Sd_Writable() ? "" : ",\"ro\":1");
}

/* ---------------------------------------------------------------- console */
static uint32_t num(const char *s) { return (uint32_t)strtoul(s, NULL, 0); }
static sd_unpack_fn _Unpack;                       /* lane gamecomp1 */
void Sd_SetUnpack(sd_unpack_fn f) { _Unpack = f; }
static int zp_put(void *u, uint32_t off, const uint8_t *b, uint32_t n)
{
    FIL *f = u;
    UINT w = 0;
    (void)off;                                         /* (in order from 0: the file grows with it) */
    return f_write(f, b, n, &w) != FR_OK || w != n;
}
static void show(void)
{
    sd_view_t v;
    Sd_View(&v);
    /* stg: where a USB copy stages its parts (the app slot that is not running, as `cart usb info` says) */
    const esp_partition_t *run = esp_ota_get_running_partition();
    const unsigned stg = (run && run->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1) ? 0x010000u : 0x210000u;
    char shown[16];
    if (SdLib_CorePending()) snprintf(shown, sizeof shown, "-");
    else snprintf(shown, sizeof shown, "%d", v.games);
    printf("sd: st e=0 state %s, card %lu MB, volume %s %lu MB, %lu MB free, games %d (shown %s), op %d, heap %lu (largest %lu) stg %06x%s\n", Sd_StateWord(), (unsigned long)Sd_CardMb(),
           v.exfat ? "exFAT" : "FAT32", (unsigned long)v.total_mb, (unsigned long)v.free_mb, SdLib_Total(), shown, v.op, (unsigned long)esp_get_free_heap_size(),
           (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), stg, _Unpack ? " z1" : "");
}
static int wait_op(void)
{
    for (int i = 0; i < 6000 && Sd_Busy(); i++) vTaskDelay(pdMS_TO_TICKS(100));
    return Sd_Busy() ? 1 : 0;
}

static int cmd_sd(int argc, char **argv)
{
    const char *c = argc >= 2 ? argv[1] : "st";
    if (!strcmp(c, "st")) { show(); return 0; }
    if (!strcmp(c, "pins")) { SdProbe_Pins(); return 0; }
    if (!strcmp(c, "mmc")) { Sd_Unmount(); int r = SdProbe_Mmc(argc >= 3 ? atoi(argv[2]) : 1, argc >= 4 ? atoi(argv[3]) : 400); printf("sd: mmc e=%d\n", r); return 0; }
    if (!strcmp(c, "mount")) {
        const int64_t t0 = esp_timer_get_time();
        int r = Sd_Ask(kSdOp_Mount, NULL);
        if (!r) r = wait_op();
        printf("sd: mount e=%d (%s) in %lld ms\n", Sd_Mounted() ? 0 : 1, Sd_StateWord(), (long long)(esp_timer_get_time() - t0) / 1000);
        show();
        return 0;
    }
    if (!strcmp(c, "off") || !strcmp(c, "on")) { Sd_BenchOff(c[1] == 'f'); printf("sd: %s e=0 (bench: %s)\n", c, c[1] == 'f' ? "the unit acts as if it had no card" : "the card is used again"); return 0; }
    if (!strcmp(c, "umount")) { Sd_Unmount(); printf("sd: umount e=0\n"); return 0; }
    if (!strcmp(c, "lib")) {
        const int64_t t0 = esp_timer_get_time();
        const int r = Sd_Mount() == 0 ? SdLib_Refresh(argc >= 3 && !strcmp(argv[2], "force")) : -9;
        printf("sd: lib e=%d (%s) %d games in %lld ms\n", r < 0 ? 1 : 0, r == 1 ? "rebuilt" : r == 0 ? "unchanged" : "failed", SdLib_Count(), (long long)(esp_timer_get_time() - t0) / 1000);
        return 0;
    }
    if (!strcmp(c, "list")) {
        const uint32_t from = argc >= 3 ? num(argv[2]) : 0, n = argc >= 4 ? num(argv[3]) : 20;
        const int cnt = SdLib_Total();
        const int64_t t0 = esp_timer_get_time();
        sdc_rec_t r;
        for (uint32_t i = from; (int)i < cnt && i < from + n; i++)
            if (SdLib_GetAll(i, &r) == 0) printf("sd: %4lu %-16s %s %c%c%c%c t%02x %luK %s\n", (unsigned long)i, r.title, (r.cgb & 0x80) ? "CGB" : "DMG",
                                              (r.flags & SDC_F_PLAYABLE) ? 'P' : '-', (r.flags & SDC_F_BATTERY) ? 'B' : '-', (r.flags & SDC_F_RTC) ? 'C' : '-', (r.flags & SDC_F_GG) ? 'G' : '-', r.type,
                                              (unsigned long)(r.size / 1024u), r.path);
        printf("sd: list e=%d %d games (%lld us)\n", cnt < 0 ? 1 : 0, cnt, (long long)(esp_timer_get_time() - t0));
        return 0;
    }
    if (!strcmp(c, "ls")) {
        Sd_Lock();
        FF_DIR *d = malloc(sizeof *d);
        FILINFO *fi = malloc(sizeof *fi);
        int e = 1, n = 0;
        if (d && fi && f_opendir(d, argc >= 3 ? argv[2] : "/") == FR_OK) {
            while (f_readdir(d, fi) == FR_OK && fi->fname[0]) {
                printf("sd: %s %10lu %04u-%02u-%02u %s\n", (fi->fattrib & AM_DIR) ? "DIR" : "   ", (unsigned long)fi->fsize, 1980 + (fi->fdate >> 9), (fi->fdate >> 5) & 15, fi->fdate & 31, fi->fname);
                n++;
            }
            (void)f_closedir(d);
            e = 0;
        }
        free(d); free(fi);
        Sd_Unlock();
        printf("sd: ls e=%d %d entries\n", e, n);
        return 0;
    }
    if (!strcmp(c, "format")) {
        const int exfat = argc >= 3 && !strcmp(argv[2], "exfat");
        if (strcmp(argv[argc - 1], "yes")) { printf("sd: format: every file on the card goes: sd format [exfat] yes\n"); return 0; }
        const int64_t t0 = esp_timer_get_time();
        int r = Sd_Ask(exfat ? kSdOp_FormatExfat : kSdOp_Format, NULL);
        if (!r) r = wait_op();
        printf("sd: format e=%d \"%s\" in %lld ms\n", Sd_Writable() ? 0 : 1, _T.result, (long long)(esp_timer_get_time() - t0) / 1000);
        show();
        return 0;
    }
    if (!strcmp(c, "backup") || (!strcmp(c, "tocart") && argc >= 3)) {
        /* SYSTEM > GAMES's BACK UP CART / WRITE TO CART from the console */
        const int64_t t0 = esp_timer_get_time();
        int r = Sd_Ask(c[0] == 'b' ? kSdOp_Backup : kSdOp_ToCart, c[0] == 'b' ? NULL : argv[2]);
        if (!r) r = wait_op();
        printf("sd: %s e=%d \"%s\" in %lld ms\n", c, r, _T.result, (long long)(esp_timer_get_time() - t0) / 1000);
        return 0;
    }
    if (!strcmp(c, "job") && argc >= 3) {
        /* lane ui110dev: SYSTEM > GAMES's jobs from the console: sd job backup | card2lib PATH | cart2lib | lib2card UID |
           card2cart PATH [backup] | lib2cart UID [backup] */
        static const char *const kN[] = { "", "backup", "card2cart", "lib2cart", "card2lib", "cart2lib", "lib2card", "save2cloud" };
        int k = 0;
        for (int i = 1; i < (int)(sizeof kN / sizeof kN[0]); i++) if (!strcmp(argv[2], kN[i])) k = i;
        if (!k) { printf("sd: job: backup | card2lib PATH | cart2lib | lib2card UID | save2cloud UID | card2cart PATH [backup] | lib2cart UID [backup]\n"); return 1; }
        const char *a = argc >= 4 ? argv[3] : NULL;
        const int bf = argc >= 5 && !strcmp(argv[4], "backup") ? kSdJf_BackupFirst : 0;
        const int byuid = k == kSdJob_LibToCard || k == kSdJob_LibToCart || k == kSdJob_SaveToCloud;
        const int64_t t0 = esp_timer_get_time();
        int r = Sd_AskJob(k, byuid ? NULL : a, byuid && a ? num(a) : 0, bf, "");
        if (!r) r = wait_op();
        printf("sd: job %s e=%d \"%s\" in %lld ms\n", argv[2], r, _T.result, (long long)(esp_timer_get_time() - t0) / 1000);
        return 0;
    }
    if (!strcmp(c, "khz") && argc >= 3) {
        /* bench: the card clock now (the mounted card) */
        Sd_Lock();
        const esp_err_t e = sdmmc_host_set_card_clk(SDMMC_HOST_SLOT_1, num(argv[2]));
        sdmmc_ll_set_data_timeout(SDMMC_LL_GET_HW(0), 0xFFFFFFu);
        Sd_Unlock();
        printf("sd: khz e=0x%x %lu\n", e, (unsigned long)num(argv[2]));
        return 0;
    }
    if (!strcmp(c, "wloop") && argc >= 4 && !strcmp(argv[3], "yes")) {   /* sd wloop N yes [SECTORS] */
        /* bench: N raw 8-sector writes from LBA 100 (an unformatted bench card only): errors counted */
        uint8_t *b = heap_caps_malloc(8 * 512, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        const uint32_t n = num(argv[2]), k = argc >= 5 ? num(argv[4]) : 8;
        uint32_t bad = 0;
        if (b) { memset(b, 0x5A, 8 * 512); for (uint32_t i = 0; i < n; i++) bad += Sd_RawWrite(100 + 8 * (i % 900), b, k > 8 ? 8 : k) != 0; heap_caps_free(b); }
        printf("sd: wloop e=0 %lu writes, %lu failed\n", (unsigned long)n, (unsigned long)bad);
        return 0;
    }
    if (!strcmp(c, "wraw") && argc >= 5 && !strcmp(argv[4], "yes")) {
        /* bench: sd wraw LBA COUNT yes (raw writes of a pattern; never on a card with files) */
        uint8_t *b = heap_caps_malloc(8 * 512, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        const uint32_t lba = num(argv[2]), n = num(argv[3]) > 8 ? 8 : num(argv[3]);
        if (b) { memset(b, 0xA5, 8 * 512); (void)Sd_RawWrite(lba, b, n); heap_caps_free(b); }
        return 0;
    }
    if (!strcmp(c, "rm") && argc >= 3) {
        int r = Sd_Ask(kSdOp_Delete, argv[2]);
        if (!r) r = wait_op();
        printf("sd: rm e=%d \"%s\"\n", strncmp(_T.result, "GAME  DELETED", 13) ? 1 : 0, _T.result);
        return 0;
    }
    if (!strcmp(c, "mv") && argc >= 4) {
        const int e = Sd_Mount() != 0 || Sd_Move(argv[2], argv[3]);
        if (!e) (void)SdLib_Refresh(0);
        printf("sd: mv e=%d %s -> %s\n", e ? 1 : 0, argv[2], argv[3]);
        return 0;
    }
    if (!strcmp(c, "wtest") && argc >= 4) {
        const uint32_t kib = num(argv[3]);
        uint8_t *b = heap_caps_malloc(8192, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!b) { printf("sd: wtest e=1 no memory\n"); return 0; }
        sd_wr_t *w = malloc(sizeof *w);
        int e = !w || Sd_Mount() != 0 || Sd_WriteBegin(w, argv[2], kib * 1024u);
        const int64_t t0 = esp_timer_get_time();
        for (uint32_t o = 0; !e && o < kib * 1024u; o += 8192u) {
            const uint32_t k = kib * 1024u - o < 8192u ? kib * 1024u - o : 8192u;
            for (uint32_t i = 0; i < k; i += 4) { const uint32_t v = (o + i) / 4; memcpy(b + i, &v, 4); }
            e = Sd_WriteAt(w, o, b, k);
        }
        if (!e) e = Sd_WriteCommit(w); else if (w) Sd_WriteAbort(w);
        const int64_t t1 = esp_timer_get_time();
        FIL *f = malloc(sizeof *f);
        uint32_t size = 0, bad = 0;
        if (!e && f && Sd_ReadOpen(f, argv[2], &size) == 0) {
            for (uint32_t o = 0; o < size; o += 8192u) {
                uint32_t got = 0;
                if (Sd_Read(f, o, b, 8192u, &got) || !got) { bad++; break; }
                for (uint32_t i = 0; i + 4 <= got; i += 4) { uint32_t v; memcpy(&v, b + i, 4); if (v != (o + i) / 4) { bad++; break; } }
            }
            Sd_Close(f);
        } else e = 1;
        const int64_t t2 = esp_timer_get_time();
        printf("sd: wtest e=%d %lu KiB: write %lld ms (%.2f MB/s), read back %lld ms (%.2f MB/s), %lu bad\n", e || bad || size != kib * 1024u, (unsigned long)kib,
               (long long)(t1 - t0) / 1000, (t1 - t0) ? kib * 1024.0 / (t1 - t0) : 0.0, (long long)(t2 - t1) / 1000, (t2 - t1) ? kib * 1024.0 / (t2 - t1) : 0.0, (unsigned long)bad);
        free(w); free(f); heap_caps_free(b);
        return 0;
    }
    if ((!strcmp(c, "rtest") || !strcmp(c, "sum")) && argc >= 3) {
        uint8_t *b = heap_caps_malloc(8192, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        FIL *f = malloc(sizeof *f);
        uint32_t size = 0, crc = 0;
        int e = !b || !f || Sd_Mount() != 0 || Sd_ReadOpen(f, argv[2], &size) != 0;
        const int64_t t0 = esp_timer_get_time();
        if (!e) {
            for (uint32_t o = 0; o < size; o += 8192u) {
                uint32_t got = 0;
                if (Sd_Read(f, o, b, 8192u, &got) || !got) { e = 1; break; }
                if (c[0] == 's') crc = sdc_crc32(crc, b, got);
            }
            Sd_Close(f);
        }
        const int64_t us = esp_timer_get_time() - t0;
        printf("sd: %s e=%d %lu bytes in %lld ms (%.2f MB/s) crc32 %08lx\n", c, e, (unsigned long)size, (long long)us / 1000, us ? size / (double)us : 0.0, (unsigned long)crc);
        free(f); heap_caps_free(b);
        return 0;
    }
    if (!strcmp(c, "hex") && argc >= 3) {
        /* lane mygames1 (bench): a file's bytes as hex, 64 a line ("sd: hex OFF HEX"), from OFF (0) for LEN (all): the card's
           saves backed up byte for byte over the console before a test writes beside them */
        FIL *f = malloc(sizeof *f);
        uint8_t b[64];
        uint32_t size = 0;
        int e = !f || Sd_Mount() != 0 || Sd_ReadOpen(f, argv[2], &size) != 0;
        const int opened = !e;
        const uint32_t off = argc >= 4 ? num(argv[3]) : 0, len = argc >= 5 ? num(argv[4]) : size;
        uint32_t crc = 0;
        for (uint32_t o = off; !e && o < size && o < off + len; o += sizeof b) {
            uint32_t got = 0;
            const uint32_t k = off + len - o < sizeof b ? off + len - o : sizeof b;
            if (Sd_Read(f, o, b, k, &got) || !got) { e = 1; break; }
            crc = sdc_crc32(crc, b, got);
            char h[2 * sizeof b + 1];
            for (uint32_t i = 0; i < got; i++) snprintf(h + 2 * i, 3, "%02x", b[i]);
            printf("sd: hex %lu %s\n", (unsigned long)o, h);
        }
        if (opened) Sd_Close(f);
        printf("sd: hex e=%d %lu bytes crc32 %08lx\n", e, (unsigned long)size, (unsigned long)crc);
        free(f);
        return 0;
    }
    if (!strcmp(c, "import") && argc >= 5) {
        /* the USB route: the installer stages a game in this ESP32's flash (as a cart install does), then this copies it */
        const uint32_t a = num(argv[2]), len = num(argv[3]);
        char name[64], path[SDC_PATH_MAX];
        uint8_t *b = heap_caps_malloc(4096, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        sd_wr_t *w = malloc(sizeof *w);
        uint32_t crc = 0;
        int e = !b || !w || len < 0x150 || len > SDC_ROM_MAX || sdc_clean_file(argv[4], "", name, sizeof name) || !sdc_is_game_name(name) ||
                Sd_Mount() != 0 || Sd_FreeName(SDC_GAMES_DIR, name, path, sizeof path) || Sd_WriteBegin(w, path, len);
        const int64_t t0 = esp_timer_get_time();
        for (uint32_t o = 0; !e && o < len; o += 4096u) {
            const uint32_t k = len - o < 4096u ? len - o : 4096u;
            e = esp_flash_read(NULL, b, a + o, k) != ESP_OK || Sd_WriteAt(w, o, b, k);
            crc = sdc_crc32(crc, b, k);
        }
        if (!e) e = Sd_WriteCommit(w); else if (w && w->open) Sd_WriteAbort(w);
        if (!e) (void)SdLib_Refresh(0);
        printf("sd: import e=%d %s %lu bytes crc32 %08lx in %lld ms\n", e ? 1 : 0, e ? "" : path, (unsigned long)len, (unsigned long)crc, (long long)(esp_timer_get_time() - t0) / 1000);
        free(w); heap_caps_free(b);
        return 0;
    }
    if (!strcmp(c, "imp") && argc >= 4) {
        /* stateless (the staging writes restart this ESP32 between parts): the part file is PATH.new on the card */
        int e = 1;
        char tmp[SDC_PATH_MAX + 8];
        if (!strcmp(argv[2], "begin")) {
            char name[64], path[SDC_PATH_MAX];
            e = sdc_clean_file(argv[3], "", name, sizeof name) || !sdc_is_game_name(name) || Sd_Mount() != 0 || !Sd_Writable() ||
                Sd_FreeName(SDC_GAMES_DIR, name, path, sizeof path);
            if (!e) {
                snprintf(tmp, sizeof tmp, "%s.new", path);
                FIL *f = malloc(sizeof *f);
                Sd_Lock();
                e = !f || f_open(f, tmp, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK;
                if (!e) (void)f_close(f);
                Sd_Unlock();
                free(f);
            }
            printf("sd: imp begin e=%d path %s\n", e, e ? "-" : path);
        } else if (!strcmp(argv[2], "part") && argc >= 7) {
            const uint32_t a = num(argv[4]), len = num(argv[5]), off = num(argv[6]);
            snprintf(tmp, sizeof tmp, "%s.new", argv[3]);
            uint8_t *bf = heap_caps_malloc(4096, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
            FIL *f = malloc(sizeof *f);
            const int64_t t0 = esp_timer_get_time();
            Sd_Lock();
            e = !bf || !f || Sd_Mount() != 0 || f_open(f, tmp, FA_WRITE | FA_OPEN_EXISTING) != FR_OK;
            if (!e) {
                e = f_size(f) != off || f_lseek(f, off) != FR_OK;
                for (uint32_t o = 0; !e && o < len; o += 4096u) {
                    const uint32_t k = len - o < 4096u ? len - o : 4096u;
                    UINT w = 0;
                    e = esp_flash_read(NULL, bf, a + o, k) != ESP_OK || f_write(f, bf, k, &w) != FR_OK || w != k;
                }
                if (f_close(f) != FR_OK) e = 1;
            }
            Sd_Unlock();
            free(f); heap_caps_free(bf);
            printf("sd: imp part e=%d %lu bytes at %lu (%lld ms)\n", e, (unsigned long)len, (unsigned long)off, (long long)(esp_timer_get_time() - t0) / 1000);
        } else if (!strcmp(argv[2], "zpart") && argc >= 6 && _Unpack) {
            /* lane gamecomp1: the staged bytes ADDR..ADDR+LEN are ONE whole SGZ1 container: the game unpacked into PATH.new
               (which must be empty: just begun), every check made; then `sd imp end PATH CRC32` as for plain parts */
            const uint32_t a = num(argv[4]), len = num(argv[5]);
            snprintf(tmp, sizeof tmp, "%s.new", argv[3]);
            FIL *f = malloc(sizeof *f);
            uint32_t rs = 0;
            const int64_t t0 = esp_timer_get_time();
            Sd_Lock();
            e = !f || Sd_Mount() != 0 || f_open(f, tmp, FA_WRITE | FA_OPEN_EXISTING) != FR_OK;
            if (!e) {
                e = f_size(f) != 0;
                if (!e) e = _Unpack(a, len, zp_put, f, &rs) ? 3 : 0;
                if (f_close(f) != FR_OK && !e) e = 1;
                if (e == 3) { FIL *g = malloc(sizeof *g); if (g && f_open(g, tmp, FA_CREATE_ALWAYS | FA_WRITE) == FR_OK) (void)f_close(g); free(g); }   /* (empty again: a retry starts clean) */
            }
            Sd_Unlock();
            free(f);
            printf("sd: imp zpart e=%d %lu bytes (%lld ms)\n", e, (unsigned long)(e ? 0 : rs), (long long)(esp_timer_get_time() - t0) / 1000);
        } else if (!strcmp(argv[2], "end") && argc >= 5) {
            snprintf(tmp, sizeof tmp, "%s.new", argv[3]);
            const uint32_t want = (uint32_t)strtoul(argv[4], NULL, 16);
            uint8_t *bf = heap_caps_malloc(8192, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
            FIL *f = malloc(sizeof *f);
            uint32_t crc = 0, size = 0;
            Sd_Lock();
            e = !bf || !f || Sd_Mount() != 0 || f_open(f, tmp, FA_READ) != FR_OK;
            if (!e) {
                size = (uint32_t)f_size(f);
                UINT g = 0;
                do { if (f_read(f, bf, 8192, &g) != FR_OK) { e = 1; break; } crc = sdc_crc32(crc, bf, g); } while (g == 8192);
                (void)f_close(f);
            }
            if (!e && crc == want) e = f_rename(tmp, argv[3]) != FR_OK;
            else if (!e) { e = 2; (void)f_unlink(tmp); }
            Sd_Unlock();
            free(f); heap_caps_free(bf);
            if (!e) (void)SdLib_Refresh(0);
            printf("sd: imp end e=%d %s %lu bytes crc32 %08lx\n", e, argv[3], (unsigned long)size, (unsigned long)crc);
        } else if (!strcmp(argv[2], "abort")) {
            snprintf(tmp, sizeof tmp, "%s.new", argv[3]);
            Sd_Lock();
            const FRESULT r = Sd_Mount() == 0 ? f_unlink(tmp) : FR_NOT_READY;
            Sd_Unlock();
            e = r != FR_OK && r != FR_NO_FILE;
            printf("sd: imp abort e=%d %s\n", e, argv[3]);
        } else printf("sd: imp e=1 (begin NAME | part PATH ADDR LEN OFF | zpart PATH ADDR LEN | end PATH CRC32HEX | abort PATH)\n");
        return 0;
    }
    printf("sd: st | mount | umount | lib [force] | list [FROM [N]] | ls [DIR] | format [exfat] yes | rm PATH | wtest PATH KIB | rtest PATH | sum PATH | import ADDR LEN NAME | pins | mmc W KHZ\n");
    return 0;
}

void Sd_Register(void)
{
    Sd_CardInit();
    const esp_console_cmd_t cmd = { .command = "sd", .help = "microSD card: sd st|mount|umount|lib|list|ls|format|rm|wtest|rtest|sum|import|pins|mmc", .func = &cmd_sd };
    (void)esp_console_cmd_register(&cmd);
    (void)Sd_Ask(kSdOp_Mount, NULL);                  /* the first look (the task ends when it is done) */
}
