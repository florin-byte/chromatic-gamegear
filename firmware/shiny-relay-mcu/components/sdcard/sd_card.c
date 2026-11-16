/* sd_card.c: lane sdcard1: the microSD card itself. The ESP32's SDMMC host, slot 1, 4 bits (IO_MUX
 * pins 14 CLK, 15 CMD, 2 D0, 4 D1, 12 D2, 13 D3; the board pulls every line up), 20 MHz (writes at 40 MHz
 * failed CRC checks on the bench). FatFs (components/fatfs: the IDF's with exFAT on, 512-byte sectors, one volume, files share
 * the volume's window). Everything is heap and exists only while a card is mounted: the card record,
 * the FATFS object (with its 512-byte window, DMA-capable), the host driver's queue.
 * No card-detect pin: a mounted card is asked (CMD13) before it is used (Sd_Present); a gone card is
 * unmounted. All of it under one recursive lock (any task: the OSD's, the RAM cart's, the HTTP task). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/sdmmc_host.h"
#include "hal/sdmmc_ll.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"
#include "ff.h"
#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#include "sd.h"
#include "sd_core.h"

static SemaphoreHandle_t _Mtx;
static sdmmc_card_t *_Card;
static FATFS *_Fs;
static int _HostOn;
static volatile int _St = kSdSt_None;
static uint32_t _TotalMb, _FreeMb, _Gen;
static uint8_t _Exfat;
static uint8_t _Off;                    /* bench (`sd off`): the unit behaves as if it had no card (RAM only) */
void Sd_BenchOff(int off) { _Off = (uint8_t)(off != 0); if (_Off) Sd_Unmount(); }

void Sd_Lock(void) { if (_Mtx) (void)xSemaphoreTakeRecursive(_Mtx, portMAX_DELAY); }
int Sd_TryLock(uint32_t ms) { return _Mtx && xSemaphoreTakeRecursive(_Mtx, pdMS_TO_TICKS(ms)) == pdTRUE; }
void Sd_Unlock(void) { if (_Mtx) (void)xSemaphoreGiveRecursive(_Mtx); }
void Sd_CardInit(void) { if (!_Mtx) _Mtx = xSemaphoreCreateRecursiveMutex(); }

int Sd_State(void) { return _St; }
int Sd_Mounted(void) { return _St == kSdSt_Ready || _St == kSdSt_ReadOnly; }
int Sd_Writable(void) { return _St == kSdSt_Ready; }
uint32_t Sd_Generation(void) { return _Gen; }
int Sd_IsExfat(void) { return _Exfat; }
void Sd_Space(uint32_t *total_mb, uint32_t *free_mb)
{
    if (total_mb) *total_mb = _TotalMb;
    if (free_mb) *free_mb = _FreeMb;
}
uint32_t Sd_CardMb(void) { return _Card ? (uint32_t)((uint64_t)_Card->csd.capacity * _Card->csd.sector_size / 1048576u) : 0; }

static void host_off(void)
{
    if (_Fs) { (void)f_unmount(""); heap_caps_free(_Fs); _Fs = NULL; }
    if (_Card) { ff_diskio_unregister(0); free(_Card); _Card = NULL; }
    if (_HostOn) { (void)sdmmc_host_deinit(); _HostOn = 0; }
}

static volatile uint8_t _Lent;
/* lane ui110dev: what the card was when it was lent (recv/start's summary: a lent card is still in the unit, and a card send
   mounts it again; before this a receive mode's start said no sd1 once the card had been lent, so the site and GET GAMES
   saw NO CARD) */
static uint8_t _LentWritable;
static int _LentGames = -1;
static uint32_t _LentTotal, _LentFree;
void Sd_Lend(void)
{
    const int g = SdLib_Count();
    Sd_Lock();
    if (Sd_Mounted() || _St == kSdSt_NoFs) {
        _Lent = 1;
        _LentWritable = _St == kSdSt_Ready;
        _LentGames = g;
        _LentTotal = _TotalMb; _LentFree = _FreeMb;
    }
    Sd_Unlock();
    Sd_Unmount();
}
int Sd_LentInfo(int *writable, int *games, uint32_t *total_mb, uint32_t *free_mb)
{
    if (!Sd_Lent()) return 0;
    *writable = _LentWritable; *games = _LentGames; *total_mb = _LentTotal; *free_mb = _LentFree;
    return 1;
}
int Sd_Lent(void) { return _Lent && !Sd_Mounted(); }
void Sd_LentClear(void) { _Lent = 0; }

void Sd_Unmount(void)
{
    Sd_Lock();
    if (_St != kSdSt_None) printf("sd: card unmounted\n");
    host_off();
    _St = kSdSt_None;
    _TotalMb = _FreeMb = 0;
    _Gen++;
    Sd_Unlock();
}

/* the unit's own left-overs in /SAVES: a save's *.new dropped when its save is there (a write cut before the swap)
   or when it is empty, else kept: Sd_ReadOpen takes it (the old save was already removed: it is the whole new one).
   /GAMES's *.new are a USB copy's parts (they live across the restarts between parts: never swept here; a new copy
   or download under the name starts its part file again, `sd imp abort` drops one) */
static void sweep(const char *dir, int all)
{
    FF_DIR *d = malloc(sizeof *d);
    FILINFO *fi = malloc(sizeof *fi);
    char *p = malloc(SDC_PATH_MAX + 16);
    if (d && fi && p && f_opendir(d, dir) == FR_OK) {
        while (f_readdir(d, fi) == FR_OK && fi->fname[0]) {
            const size_t l = strlen(fi->fname);
            if (l < 5 || strcmp(fi->fname + l - 4, ".new") || (fi->fattrib & AM_DIR)) continue;
            if (l > 100) continue;
            snprintf(p, SDC_PATH_MAX + 16, "%s/%.*s", dir, (int)(l - 4), fi->fname);
            FILINFO *t = malloc(sizeof *t);
            const int has = t && f_stat(p, t) == FR_OK;
            free(t);
            if (all || has || fi->fsize == 0) { if (snprintf(p, SDC_PATH_MAX + 16, "%s/%.100s", dir, fi->fname) >= SDC_PATH_MAX + 16) continue; if (f_unlink(p) == FR_OK) printf("sd: %s: a part file left by a cut, dropped\n", p); }
        }
        (void)f_closedir(d);
    }
    free(d); free(fi); free(p);
}

/* the card answered: its FAT volume. 0 = a volume (read-only or not) */
static int volume_up(void)
{
    if (!_Fs && !(_Fs = heap_caps_calloc(1, sizeof(FATFS), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL))) return -1;
    FRESULT r = f_mount(_Fs, "", 1);
    if (r == FR_NO_FILESYSTEM) { printf("sd: no FAT32 or exFAT volume on the card\n"); _St = kSdSt_NoFs; return -2; }
    if (r != FR_OK) { printf("sd: mount: FatFs %d\n", r); _St = kSdSt_Error; return -3; }
    _Exfat = _Fs->fs_type == FS_EXFAT;
    /* writable? one small file written and removed (a locked card, a worn card: read-only) */
    int rw = 0;
    FRESULT m = f_mkdir(SDC_SHINY_DIR);
    if (m == FR_OK || m == FR_EXIST) {
        FIL f;
        UINT w = 0;
        if (f_open(&f, SDC_SHINY_DIR "/RW.TMP", FA_CREATE_ALWAYS | FA_WRITE) == FR_OK) {
            rw = f_write(&f, "SHNY", 4, &w) == FR_OK && w == 4;
            if (f_close(&f) != FR_OK) rw = 0;
            if (f_unlink(SDC_SHINY_DIR "/RW.TMP") != FR_OK) rw = 0;
        }
    }
    if (rw) { (void)f_mkdir(SDC_GAMES_DIR); (void)f_mkdir(SDC_SAVES_DIR); sweep(SDC_SAVES_DIR, 0); }
    _St = rw ? kSdSt_Ready : kSdSt_ReadOnly;
    Sd_RefreshSpace();
    printf("sd: card mounted: %s, %lu MB, %lu MB free%s\n", _Exfat ? "exFAT" : _Fs->fs_type == FS_FAT32 ? "FAT32" : "FAT", (unsigned long)_TotalMb,
           (unsigned long)_FreeMb, rw ? "" : ", READ ONLY");
    return 0;
}

void Sd_RefreshSpace(void)
{
    Sd_Lock();
    DWORD fc = 0;
    FATFS *fs = NULL;
    if (_Fs && f_getfree("", &fc, &fs) == FR_OK && fs) {
        const uint64_t cl = (uint64_t)fs->csize * 512u;
        _FreeMb = (uint32_t)((uint64_t)fc * cl / 1048576u);
        _TotalMb = (uint32_t)((uint64_t)(fs->n_fatent - 2) * cl / 1048576u);
    }
    Sd_Unlock();
}

/* the card up and its volume mounted (idempotent: a mounted card is asked whether it is still there). 0 = mounted */
int Sd_Mount(void)
{
    if (_Off) return 1;
    Sd_Lock();
    if (Sd_Mounted()) {
        const esp_err_t status = sdmmc_get_status(_Card);
        const int there = status == ESP_OK;
        if (there) { Sd_Unlock(); return 0; }
        printf("[%lld ms] sd: the card is gone (CMD13 0x%x)\n", (long long)(esp_timer_get_time() / 1000), status);
        Sd_Unmount();
    }
    host_off();
    const int64_t t0 = esp_timer_get_time();
    esp_err_t e = sdmmc_host_init();
    if (e != ESP_OK) { printf("[%lld ms] sd: mount failed at host_init: 0x%x\n", (long long)(esp_timer_get_time() / 1000), e); Sd_Unlock(); return -1; }
    _HostOn = 1;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.flags = 0;                              /* (the board has its own pull-ups) */
    e = sdmmc_host_init_slot(SDMMC_HOST_SLOT_1, &slot);
    if (e != ESP_OK) { printf("[%lld ms] sd: mount failed at init_slot: 0x%x\n", (long long)(esp_timer_get_time() / 1000), e); host_off(); Sd_Unlock(); return -1; }
    _Card = calloc(1, sizeof *_Card);
    if (!_Card) { printf("[%lld ms] sd: mount failed at alloc: 0x%x\n", (long long)(esp_timer_get_time() / 1000), ESP_ERR_NO_MEM); host_off(); Sd_Unlock(); return -1; }
    /* 20 MHz, default speed: at 40 MHz (high speed) reads were right but writes failed with CRC errors on the bench
       (unit 2's card: 49 of 50 single-block writes; unit 1: now and then), at 20 MHz none in hundreds */
    static const int khz[1] = { SDMMC_FREQ_DEFAULT };
    e = ESP_FAIL;
    for (int i = 0; i < 1 && e != ESP_OK; i++) {
        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        host.slot = SDMMC_HOST_SLOT_1;
        host.max_freq_khz = khz[i];
        memset(_Card, 0, sizeof *_Card);
        e = sdmmc_card_init(&host, _Card);
        if (e == ESP_ERR_TIMEOUT || e == ESP_ERR_NOT_FOUND) break;   /* (no card: no point trying slower) */
    }
    if (e != ESP_OK) {
        printf("[%lld ms] sd: mount failed at card_init: 0x%x\n", (long long)(esp_timer_get_time() / 1000), e);
        free(_Card); _Card = NULL;
        host_off();
        if (_St != kSdSt_None) _Gen++;
        _St = kSdSt_None;
        Sd_Unlock();
        return 1;                                /* no card (or one that does not answer) */
    }
    /* the host's data timeout: the IDF sets 100 ms, and a card may stay busy longer than that between the blocks of
       a write (the spec allows 250 ms for SDHC, 500 ms for SDXC): a write at a slow moment failed with 0x107
       (FORMAT CARD, a download). The register's most: 2^24 card clocks (419 ms at 40 MHz). */
    sdmmc_ll_set_data_timeout(SDMMC_LL_GET_HW(0), 0xFFFFFFu);
    ff_diskio_register_sdmmc(0, _Card);
    _Gen++;
    const int r = volume_up();
    printf("sd: card %lu MB at %d kHz, 4 bits: %s in %lld ms\n", (unsigned long)Sd_CardMb(), _Card->real_freq_khz,
           r == 0 ? "mounted" : r == -2 ? "no volume (FORMAT CARD makes one)" : "not mounted", (long long)(esp_timer_get_time() - t0) / 1000);
    if (r && r != -2) {                          /* (a card with no volume stays up: FORMAT CARD needs it) */
        if (_Fs) { heap_caps_free(_Fs); _Fs = NULL; }
    }
    Sd_Unlock();
    return r;
}

/* still there? (CMD13) A gone card is unmounted. 1 = mounted and there */
int Sd_Present(void)
{
    Sd_Lock();
    int there = 0;
    if (_Card && (Sd_Mounted() || _St == kSdSt_NoFs)) {
        const esp_err_t status = sdmmc_get_status(_Card);
        there = status == ESP_OK;
        if (!there) { printf("[%lld ms] sd: the card is gone (CMD13 0x%x)\n", (long long)(esp_timer_get_time() / 1000), status); Sd_Unmount(); }
    }
    Sd_Unlock();
    return there && Sd_Mounted();
}

/* FORMAT CARD's view of the card: the volume from 4 MiB on (the SD rule: the partition and its data area on the
   card's 4 MiB erase-unit boundaries, as the SD Association's formatter makes them; FatFs alone puts it at 63) */
#define SD_PART_LBA 8192u
static DSTATUS ofs_init(unsigned char p) { (void)p; return 0; }
static DSTATUS ofs_status(unsigned char p) { (void)p; return 0; }
static DRESULT ofs_read(unsigned char p, unsigned char *b, uint32_t s, unsigned n) { (void)p; return sdmmc_read_sectors(_Card, b, s + SD_PART_LBA, n) == ESP_OK ? RES_OK : RES_ERROR; }
static DRESULT ofs_write(unsigned char p, const unsigned char *b, uint32_t s, unsigned n)
{
    (void)p;
    esp_err_t e = ESP_FAIL;
    for (int t = 0; t < 3 && e != ESP_OK; t++) e = sdmmc_write_sectors(_Card, b, s + SD_PART_LBA, n);
    return e == ESP_OK ? RES_OK : RES_ERROR;
}
static DRESULT ofs_ioctl(unsigned char p, unsigned char c, void *b)
{
    (void)p;
    if (c == CTRL_SYNC) return RES_OK;
    if (c == GET_SECTOR_COUNT) { *(DWORD *)b = _Card->csd.capacity - SD_PART_LBA; return RES_OK; }
    if (c == GET_SECTOR_SIZE) { *(WORD *)b = 512; return RES_OK; }
    if (c == GET_BLOCK_SIZE) { *(DWORD *)b = SD_PART_LBA; return RES_OK; }
    return RES_PARERR;
}
static const ff_diskio_impl_t _OfsImpl = { ofs_init, ofs_status, ofs_read, ofs_write, ofs_ioctl };

/* FORMAT CARD: one partition, FAT32 up to 32 GB, exFAT above (the SD rules), or exFAT when asked. 0 = done (mounted again) */
int Sd_Format(int exfat, void (*pct)(int))
{
    Sd_Lock();
    if (!_Card) { int m = Sd_Mount(); if (m && m != -2) { Sd_Unlock(); return -1; } }
    if (!_Card) { Sd_Unlock(); return -1; }
    if (_Fs) { (void)f_unmount(""); }
    _St = kSdSt_Busy;
    _Gen++;
    const uint32_t mb = Sd_CardMb();
    MKFS_PARM opt = { .fmt = (uint8_t)((exfat || mb > 32u * 1024u) ? FM_EXFAT : FM_FAT32), .n_fat = 0, .align = 0, .n_root = 0, .au_size = 0 };
    const UINT wl = 4096;
    void *work = heap_caps_malloc(wl, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!work) { _St = kSdSt_Error; Sd_Unlock(); return -2; }
    if (pct) pct(10);
    const int64_t t0 = esp_timer_get_time();
    /* the volume made in the view from 4 MiB on (no partition table of its own), then the card's partition table */
    opt.fmt |= FM_SFD;
    opt.align = SD_PART_LBA;
    ff_diskio_register(0, &_OfsImpl);
    FRESULT r = f_mkfs("", &opt, work, wl);
    ff_diskio_register_sdmmc(0, _Card);
    if (r == FR_OK) {
        uint8_t *m = work;
        memset(m, 0, 512);
        uint8_t *pe = m + 446;
        const uint32_t n = _Card->csd.capacity - SD_PART_LBA;
        pe[1] = 0xFE; pe[2] = 0xFF; pe[3] = 0xFF;                        /* CHS: past 8 GB, the LBA fields count */
        pe[4] = (opt.fmt & FM_EXFAT) ? 0x07 : 0x0C;
        pe[5] = 0xFE; pe[6] = 0xFF; pe[7] = 0xFF;
        pe[8] = (uint8_t)SD_PART_LBA; pe[9] = (uint8_t)(SD_PART_LBA >> 8); pe[10] = (uint8_t)(SD_PART_LBA >> 16); pe[11] = (uint8_t)(SD_PART_LBA >> 24);
        pe[12] = (uint8_t)n; pe[13] = (uint8_t)(n >> 8); pe[14] = (uint8_t)(n >> 16); pe[15] = (uint8_t)(n >> 24);
        m[510] = 0x55; m[511] = 0xAA;
        esp_err_t e = ESP_FAIL;
        for (int t = 0; t < 3 && e != ESP_OK; t++) e = sdmmc_write_sectors(_Card, m, 0, 1);
        if (e != ESP_OK) r = FR_DISK_ERR;
    }
    heap_caps_free(work);
    printf("sd: format %s: FatFs %d in %lld ms\n", (opt.fmt & FM_EXFAT) ? "exFAT" : "FAT32", r, (long long)(esp_timer_get_time() - t0) / 1000);
    if (r != FR_OK) {
        _St = kSdSt_Error;
        if (r == FR_WRITE_PROTECTED) _St = kSdSt_ReadOnly;
        Sd_Unlock();
        return -3;
    }
    if (pct) pct(90);
    if (_Fs) { heap_caps_free(_Fs); _Fs = NULL; }
    const int v = volume_up();
    if (v == 0) (void)f_setlabel("SHINYRELAY");
    _Gen++;
    Sd_Unlock();
    return v;
}

const char *Sd_StateWord(void)
{
    switch (_St) {
    case kSdSt_Ready: return "READY";
    case kSdSt_ReadOnly: return "READ  ONLY";
    case kSdSt_NoFs: return "NOT  FORMATTED";
    case kSdSt_Error: return "CANNOT  BE  READ";
    case kSdSt_Busy: return "BUSY";
    default: return "NO  CARD";
    }
}

/* ---- small file helpers (paths are UTF-8, from the root) ---- */
int Sd_FileSize(const char *path, uint32_t *size)
{
    FILINFO *fi = malloc(sizeof *fi);
    if (!fi) return -1;
    const FRESULT r = f_stat(path, fi);
    if (r == FR_OK && size) *size = (uint32_t)fi->fsize;
    free(fi);
    return r == FR_OK ? 0 : 1;
}

/* raw sectors of the card (bench: the Mac-side check of the volume; buf DMA-capable, 4-byte aligned). 0 = read */
int Sd_RawRead(uint32_t lba, void *buf, uint32_t count)
{
    Sd_Lock();
    const int r = _Card && sdmmc_read_sectors(_Card, buf, lba, count) == ESP_OK ? 0 : -1;
    Sd_Unlock();
    return r;
}

/* bench: raw sector writes (destroys what is there: an unformatted bench card only) */
int Sd_RawWrite(uint32_t lba, const void *buf, uint32_t count)
{
    Sd_Lock();
    const int64_t t0 = esp_timer_get_time();
    const esp_err_t e = _Card ? sdmmc_write_sectors(_Card, buf, lba, count) : ESP_FAIL;
    if (e != ESP_OK) printf("sd: raw write %lu x 512 at %lu: 0x%x in %lld us\n", (unsigned long)count, (unsigned long)lba, e, (long long)(esp_timer_get_time() - t0));
    Sd_Unlock();
    return e == ESP_OK ? 0 : -1;
}
