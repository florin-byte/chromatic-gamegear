/* cf_dev.c: the ESP32 side of cf_esp (lane cartflash1). DRAFT: compiled only with
 * CONFIG_NW_CARTFLASH (menuconfig "Shiny Relay network window > Cart flash", default off).
 * Never run on a device yet.
 *
 * Storage: the "cart" data partition (partitions_cartflash.csv; the stock table has none,
 * so this needs a USB flash with the new table). Layout:
 *   0x00000  meta, 4 KiB: the ROM backup's block index (u16 per 4 KiB block: FFFF = the
 *            block is all FF and not stored, else the slot number)
 *   0x01000  the save RAM backup, 32 KiB
 *   0x09000  ROM backup slots, 4 KiB each, as many as fit below the image region
 *   end - CF_IMG_CAP  the staged image ("Send to my Chromatic" download), CF_IMG_CAP bytes
 * A ModRetro Tetris cart is 256 KiB of game on a 2 MiB chip; the rest reads FF, so its
 * backup takes ~260 KiB of slots. The backup NEVER leaves the device except over the
 * owner's own USB console (`cart export`), never to a server.
 *
 * Journal: NVS namespace "cf", blob "j". FPGA: link packets on UART address 0x10
 * (cmd 5 cart power cycle, cmd 7 boot select, FPGA cartflash1-bootsel only).
 */
#include "sdkconfig.h"
#ifdef CONFIG_NW_CARTFLASH
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_partition.h"
#include "nvs.h"

#include "cf_esp.h"
#include "cf_fetch.h"
#include <stdbool.h>
#include "cwl_online.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "crc8_sae_j1850.h"

static const char *TAG = "cf";
static int _HeapMapOn;                 /* cartspeed2 bench `cart heapmap on`: the map also when receive mode has stopped the link */
static void heapmap_print(void);
#define CF_IMG_CAP   (1024u * 1024u)   /* a 1 MiB MBC5 game (e.g. 8 Mbit); leaves 167 backup slots */
#define CF_STREAM_MIN_HEAP 23500u    /* a streamed install over a TLS image connection (https blob_url, not what the server sends): the handshake's gate (19 KB) + the job's chunk and the connection's (2 KB each) */
#define CF_STREAM_PLAIN_HEAP 14000u  /* ... over the plain http image connection (wifiinst1): the connection's gate (9 KB) + the job's chunk + the journal's NVS commit */
#define CF_STREAM_OPEN_HEAP 6000u    /* ... with the connection already open (kept from the header read): only the job's chunk + a margin */
#define CF_JOB_MIN_HEAP 6144u        /* NVS journal commits + the console; measured 8 KB free in a clean job */
#define CF_META      0x00000u
#define CF_RAM_OFF   0x01000u
#define CF_SLOTS     0x09000u
#define CF_BLK       4096u
#define CF_MAX_BLKS  1024u            /* 4 MiB chips */

static const esp_partition_t *_Part;
static cf_esp_t _Cf;
static uint16_t *_Index;              /* heap, only while a job needs the backup (work_get / work_put) */
#define CF_INDEX_BYTES (CF_MAX_BLKS * sizeof(uint16_t))
static uint16_t _NextSlot;
static int      _IndexDirty;
static uint8_t  *_Blk;
static uint32_t _BlkNo = 0xFFFFFFFFu;
static uint32_t _ImgSize;
static cf_status_t _Last;

/* the save store: the last 32 KiB below the image region (8 fewer backup slots: 159 left, a
   Tetris cart's backup takes 57) */
#define CF_SAV_SIZE  0x8000u
static uint32_t sav_base(void) { return _Part->size - CF_IMG_CAP - CF_SAV_SIZE; }
static uint32_t slots_max(void) { return _Part ? (uint32_t)((_Part->size - CF_IMG_CAP - CF_SAV_SIZE - CF_SLOTS) / CF_BLK) : 0; }

/* The backup index (2 KiB) and one block buffer (4 KiB) were static RAM; with the probe's
 * buffers that was ~20 KB the idle firmware could not give to a TLS handshake (0.17.6 needs
 * 21 KB free). They live on the heap from the first backup access of a job to CfDev_Tick()
 * seeing no job and no probe armed. */
static int work_get(void)
{
    if (_Index && _Blk) return 0;
    if (!_Part) return -1;
    if (!_Index) _Index = malloc(CF_INDEX_BYTES);
    if (!_Blk) _Blk = malloc(CF_BLK);
    if (!_Index || !_Blk) { free(_Index); free(_Blk); _Index = NULL; _Blk = NULL; return -1; }
    if (esp_partition_read(_Part, CF_META, _Index, CF_INDEX_BYTES) != ESP_OK) memset(_Index, 0xFF, CF_INDEX_BYTES);
    _BlkNo = 0xFFFFFFFFu;
    return 0;
}

static void index_flush(void)
{
    if (!_IndexDirty || !_Part || !_Index) return;
    (void)esp_partition_erase_range(_Part, CF_META, CF_BLK);
    (void)esp_partition_write(_Part, CF_META, _Index, CF_INDEX_BYTES);
    _IndexDirty = 0;
}

static int blk_commit(void)
{
    if (_BlkNo == 0xFFFFFFFFu) return 0;
    int ff = 1;
    for (unsigned i = 0; i < CF_BLK; i++) if (_Blk[i] != 0xFF) { ff = 0; break; }
    if (ff) _Index[_BlkNo] = 0xFFFF;
    else {
        if (_NextSlot >= slots_max()) return -1;              /* full: export over USB */
        const uint32_t a = CF_SLOTS + (uint32_t)_NextSlot * CF_BLK;
        if (esp_partition_erase_range(_Part, a, CF_BLK) != ESP_OK || esp_partition_write(_Part, a, _Blk, CF_BLK) != ESP_OK) return -1;
        _Index[_BlkNo] = _NextSlot++;
    }
    _IndexDirty = 1;
    _BlkNo = 0xFFFFFFFFu;
    return 0;
}

/* cf_esp writes the backup front to back, in pieces inside 4 KiB blocks */
static int bak_write(void *u, uint32_t off, const uint8_t *b, uint32_t n)
{
    (void)u;
    if (!_Part || work_get()) return -1;
    if (off == 0) { memset(_Index, 0xFF, CF_INDEX_BYTES); _NextSlot = 0; _BlkNo = 0xFFFFFFFFu; }
    while (n) {
        const uint32_t blk = off / CF_BLK, o = off % CF_BLK;
        if (blk >= CF_MAX_BLKS) return -1;
        if (blk != _BlkNo) {
            if (blk_commit()) return -1;
            _BlkNo = blk;
            memset(_Blk, 0xFF, CF_BLK);
        }
        const uint32_t k = n < CF_BLK - o ? n : CF_BLK - o;
        memcpy(_Blk + o, b, k);
        off += k; b += k; n -= k;
        if (o + k == CF_BLK && blk_commit()) return -1;
    }
    return 0;
}

static int bak_read(void *u, uint32_t off, uint8_t *b, uint32_t n)
{
    (void)u;
    if (!_Part || work_get()) return -1;
    if (blk_commit()) return -1;
    index_flush();
    while (n) {
        const uint32_t blk = off / CF_BLK, o = off % CF_BLK;
        const uint32_t k = n < CF_BLK - o ? n : CF_BLK - o;
        if (blk >= CF_MAX_BLKS || _Index[blk] == 0xFFFF) memset(b, 0xFF, k);
        else if (esp_partition_read(_Part, CF_SLOTS + (uint32_t)_Index[blk] * CF_BLK + o, b, k) != ESP_OK) return -1;
        off += k; b += k; n -= k;
    }
    return 0;
}

static int ram_write(void *u, uint32_t off, const uint8_t *b, uint32_t n)
{
    (void)u;
    if (!_Part || off + n > CF_RAM_SIZE) return -1;
    if (off % CF_BLK == 0) (void)esp_partition_erase_range(_Part, CF_RAM_OFF + off, CF_BLK);
    if (off % CF_BLK + n > CF_BLK) (void)esp_partition_erase_range(_Part, CF_RAM_OFF + (off / CF_BLK + 1) * CF_BLK, CF_BLK);
    return esp_partition_write(_Part, CF_RAM_OFF + off, b, n) == ESP_OK ? 0 : -1;
}

static int ram_read(void *u, uint32_t off, uint8_t *b, uint32_t n)
{
    (void)u;
    return _Part && off + n <= CF_RAM_SIZE && esp_partition_read(_Part, CF_RAM_OFF + off, b, n) == ESP_OK ? 0 : -1;
}

static int sav_write(void *u, uint32_t off, const uint8_t *b, uint32_t n)
{
    (void)u;
    if (!_Part || off + n > CF_SAV_SIZE) return -1;
    if (off % CF_BLK == 0 && esp_partition_erase_range(_Part, sav_base() + off, CF_BLK) != ESP_OK) return -1;
    if (off % CF_BLK + n > CF_BLK && esp_partition_erase_range(_Part, sav_base() + (off / CF_BLK + 1) * CF_BLK, CF_BLK) != ESP_OK) return -1;
    return esp_partition_write(_Part, sav_base() + off, b, n) == ESP_OK ? 0 : -1;
}
static int stage_read(void *u, int sav, uint32_t off, uint8_t *b, uint32_t n);
static int sav_read(void *u, uint32_t off, uint8_t *b, uint32_t n)
{
    (void)u;
    if (!_Part) return stage_read(NULL, 1, off, b, n);     /* the stock layout: the save staged by the computer (usb-cart) */
    if (off + n > CF_SAV_SIZE) return -1;
    return esp_partition_read(_Part, sav_base() + off, b, n) == ESP_OK ? 0 : -1;
}
static uint32_t sav_crc(void)
{
    uint8_t b[256];
    uint32_t c = 0;
    for (uint32_t o = 0; o < CF_SAV_SIZE; o += sizeof b) { if (sav_read(NULL, o, b, sizeof b)) return 0; c = cf_crc32(c, b, sizeof b); }
    return c;
}
static int b64v(int ch) { return ch >= 'A' && ch <= 'Z' ? ch - 'A' : ch >= 'a' && ch <= 'z' ? ch - 'a' + 26 : ch >= '0' && ch <= '9' ? ch - '0' + 52 : ch == '+' ? 62 : ch == '/' ? 63 : -1; }

static int img_read(void *u, uint32_t off, uint8_t *b, uint32_t n)
{
    (void)u;
    if (!_Part || off + n > _ImgSize) return -1;
    return esp_partition_read(_Part, _Part->size - CF_IMG_CAP + off, b, n) == ESP_OK ? 0 : -1;
}

static int jr_load(void *u, cf_journal_t *j)
{
    (void)u;
    nvs_handle_t h;
    if (nvs_open("cf", NVS_READONLY, &h) != ESP_OK) return -1;
    size_t n = sizeof *j;
    const esp_err_t e = nvs_get_blob(h, "j", j, &n);
    uint16_t idx_ok = 0;
    (void)nvs_get_u16(h, "slots", &idx_ok);
    nvs_close(h);
    if (e == ESP_OK && _Part) _NextSlot = idx_ok;        /* the index itself is read when a job needs it (work_get) */
    return e == ESP_OK && n == sizeof *j ? 0 : -1;
}

static int jr_save(void *u, const cf_journal_t *j)
{
    (void)u;
    nvs_handle_t h;
    if (nvs_open("cf", NVS_READWRITE, &h) != ESP_OK) return -1;
    esp_err_t e = nvs_set_blob(h, "j", j, sizeof *j);
    if (e == ESP_OK) e = nvs_set_u16(h, "slots", _NextSlot);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK ? 0 : -1;
}

/* the same packet as cwl_esp.c FpgaSend (UART address 0x10, link commands) */
static void fpga_link(uint8_t cmd, uint8_t byte)
{
    uint8_t buf[6] = { 0x8F, 0x10, 2, cmd, byte, 0 };
    const size_t n = crc8_sae_j1850_encode(buf, 5, buf);
    (void)uart_write_bytes(UART_NUM_1, buf, n);
}

/* The FPGA's patch overlay (cf_esp.h overlay_hold / overlay_state). The patch module of the
   firmware (components/patch, branches with the overlay) defines these; without it there is no
   overlay and nothing to hold. */
__attribute__((weak)) void Patch_CartHold(bool On) { (void)On; }
__attribute__((weak)) int Patch_CartOverlay(bool Ask) { (void)Ask; return 0; }
static void overlay_hold(void *u, int on) { (void)u; Patch_CartHold(on != 0); }
static int overlay_state(void *u, int ask) { (void)u; return Patch_CartOverlay(ask != 0); }
/* The link command numbers depend on the FPGA that is loaded, and are chosen at run time, never
 * by a build option:
 *   18.21 / 18.22 with the cart engine (the "engine" test bitstream): ENGINE_ALLOW 8, BOOT_SELECT 7.
 *     A stock 18.21 / 18.22 ignores both (no such commands): the job then finds no writer (238).
 *   18.23: 7 and 8 are SLAVE_LOCK / SLAVE_UNLOCK and there is no engine: NO cart job may start.
 *   18.24 and later: ENGINE_ALLOW 9, BOOT_SELECT 10.
 * 0 = no cart job on this FPGA (18.23, a version not read yet, or not an 18.x). */
#include "cwl.h"
/* 1.0.3: the FPGA's version word, from the last status packet at address 6, kept once it read as an 18.x:
   the FPGA cannot change under a running ESP32 (every FPGA load restarts it), so a read that is 0 or not an
   18.x later (unit 1, 2026-10-16: receive mode once said UPDATE THIS CHROMATIC FIRST on 18.28) is not taken.
   Before the first good read, up to 2 s are waited for one (the console task; never from an ISR). */
static uint16_t fpga_ver_raw(int wait)
{
    static uint16_t good;
    for (int i = 0; ; i++) {
        const uint16_t v = CWL_FpgaVersionRaw();
        if (v && (v & 0x3F) == 18) { good = v; return v; }
        if (good || !wait || i >= 20) return good ? good : v;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
static int fpga_cmds(uint8_t *eng, uint8_t *boot, unsigned *maj, unsigned *min)
{
    const uint16_t v = fpga_ver_raw(0);
    const unsigned major = v & 0x3F, minor = (v >> 6) & 0x3F;
    if (maj) *maj = major;
    if (min) *min = minor;
    if (v == 0 || major != 18 || minor == 23) return 0;
    if (minor >= 24) { *eng = 9; *boot = 10; } else { *eng = 8; *boot = 7; }
    return 1;
}
int CfDev_FpgaVersion(unsigned *maj, unsigned *min) { uint8_t e, b; return fpga_cmds(&e, &b, maj, min); }   /* 1.1.0: cf_rc_dev.c */
static void boot_select(void *u, int on) { (void)u; uint8_t e, b; if (fpga_cmds(&e, &b, NULL, NULL)) fpga_link(b, on ? 1 : 0); }
static void cart_cycle(void *u) { (void)u; fpga_link(5, 0); }
static void engine_allow(void *u, int on) { (void)u; uint8_t e, b; if (fpga_cmds(&e, &b, NULL, NULL)) fpga_link(e, on ? 1 : 0); }

/* cartspeed1: the FPGA's express cart stream (18.26+): ops / posts on UART address 0x12, the data
 * over the OSD's QSPI bus (main.c OSD_QspiCartWrite), status back on address 0x0C (nw task). */
__attribute__((weak)) int OSD_QspiCartWrite(uint16_t Off, const uint8_t *pData, uint16_t Len) { (void)Off; (void)pData; (void)Len; return -1; }
static int _XsOff;                      /* `cart xs off`: the classic route even on an 18.26 FPGA (bench) */
static uint8_t _XsTiming = 2;           /* `cart xs timing N`: the bus timing a Wi-Fi install asks for: 2 the shortest (measured on unit 2's SST39VF1682,
                                           2026-10-15: 2 MiB, every block verified); any verify error drops the job to 0, the safe timing */
static int xs_avail(void *u)
{
    (void)u;
    unsigned maj = 0, min = 0; uint8_t e, b;
    return !_XsOff && fpga_cmds(&e, &b, &maj, &min) && maj == 18 && min >= CF_XS_FPGA_MINOR && min <= CF_XS_FPGA_MINOR_LAST;
}
static void xs_send(void *u, const uint8_t *p, uint8_t n)
{
    (void)u;
    uint8_t buf[3 + 16 + 1] = { 0x8F, CF_XS_ADDR_TO_FPGA, n };
    if (n > 16) return;
    memcpy(buf + 3, p, n);
    const size_t k = crc8_sae_j1850_encode(buf, 3 + (size_t)n, buf);
    (void)uart_write_bytes(UART_NUM_1, buf, k);
}
static int xs_ring(void *u, uint16_t off, const uint8_t *b, uint16_t n) { (void)u; return OSD_QspiCartWrite(off, b, n); }
static unsigned fpga_ver(void *u)
{
    (void)u;
    unsigned maj = 0, min = 0; uint8_t e, b;
    return fpga_cmds(&e, &b, &maj, &min) ? (maj & 0xFFu) << 8 | (min & 0xFFu) : 0u;
}
#include "cf_rc_dev.h"
#include "cf_sgz.h"
#include "cf_sup.h"
#include "cf_share_dev.h"
#include "sd.h"
#include "sd_core.h"
#include "cf_pack.h"
#ifdef CONFIG_NW_CARTFLASH_WIFI
static int jobs_idle(void);
static int wait_erase(uint32_t ms);
#endif
/* fpgacomp1: the recovery design (FPGA 18.31 layout rf, version bit 13): it boots while no full design header is
   live; it has the boot flash ops and the OSD, no Game Boy core, no RAM cart, no cart bus */
static int fpga_recovery(void) { const uint16_t v = fpga_ver_raw(0); return (v & 0x3F) == 18 && ((v >> 13) & 1); }
int CfDev_FpgaRecovery(void) { return fpga_recovery(); }
/* 1.1.0: this FPGA has the RAM cart (18.30 on; not the recovery design) */
static int fpga_can_play(void) { return (fpga_ver(NULL) & 0xFFu) >= CF_RC_FPGA_MINOR && !fpga_recovery(); }
/* fpgacomp1: this FPGA writes its boot flash in the rf layout (18.31 on, the recovery design too) */
static int fpga_can_fota(void) { return (fpga_ver(NULL) & 0xFFu) >= CF_FOTA_FPGA_MINOR; }
/* The worker's boot-flash snapshot; UI calls never wait for flash operations. */
static cf_fota_layout_t _FlL;
static volatile int _FlOk;                 /* 1 = _FlL was read (this FPGA has the ops) */
/* Captured before any header mutation; a switch only changes the next power-on. */
static int _FpgaSlot = -1, _FpgaSlotRead;
static int _OtherSlotState = CF_CORE_NOT_READY, _NextFpgaSlot = -1;
int CfDev_FpgaSlotPending(void)
{
    /* A cached read or the startup deadline resolves the view; no flash work here. */
    return !_FpgaSlotRead && esp_timer_get_time() < 10000000;
}
static int fpga_slot_cached(void) { return CfDev_FpgaSlotPending() ? -2 : _FpgaSlot; }
int CfDev_FpgaOther(void) { return _OtherSlotState; }
int CfDev_FpgaNext(void) { return _NextFpgaSlot; }
static void fpga_slot_keep(const cf_fota_layout_t *L)
{
    if (_FpgaSlotRead) return;
    _FpgaSlot = !fpga_recovery() && (L->kind == CF_FL_RF || L->kind == CF_FL_RF_FULL) ? cf_fota_slot_of(L->full_base) : -1;
    _FpgaSlotRead = 1;
    printf("diag: core slot %d known after %lu ms\n", _FpgaSlot, (unsigned long)(esp_timer_get_time() / 1000));
}
int CfDev_FpgaSwitchesLeft(void)
{
    /* The worker refreshes this snapshot; drawing must never run flash operations. */
    return _FlOk ? cf_fota_switches_left(&_FlL) : -1;
}
int CfDev_FpgaSlot(void)
{
    /* The HTTP worker captures this before any write; UI calls never run flash ops. */
    return _FpgaSlot;
}
#ifdef CONFIG_NW_CARTFLASH_WIFI
static int fpga_switch(int slot);
#endif
int CfDev_FpgaSwitch(int slot)
{
#ifdef CONFIG_NW_CARTFLASH_WIFI
    return fpga_switch(slot);
#else
    (void)slot;
    return CF_FOTA_E_FLASH;
#endif
}
void CfDev_XsStatus(const uint8_t *p, uint8_t n) { CfRc_Status(p, n); cf_esp_xs_status(&_Cf, p, n); }   /* 1.1.0: the RAM cart / boot flash ops see it too */

/* a cart write needs USB power or a battery that is not red (cf_power_policy) */
#include "battery.h"
#include "cf_install.h"
static int _PowerLowTest;   /* bench (`cart power low`): the battery reads as red. RAM only, cleared by every reboot */
static int CfDev_PowerOk(void)
{
    if (_PowerLowTest) return 0;
    BatteryState_t b;
    Battery_GetState(&b);
    /* no battery in the unit (a rail below 2 V that it cannot run on): it is on USB power, which a cart
       write does not run down (wifiinst1: unit 1 on USB with no batteries read valid 0 and waited forever) */
    return b.NoBattery || cf_power_policy(b.Valid, b.Level_V, b.Low_V, b.Charging);
}
/* bench override (`cart power usb`): the console says the unit runs on USB power. RAM only,
 * cleared by every reboot; never persisted. */
static int _PowerUsb;
/* 1.0.5: the player pressed INSTALL ANYWAY on BATTERY LOW (cf_install power_anyway): this install's write
   goes on the low battery. RAM only; cleared when the install ends (and by every reboot). */
static int _PowerAnyway;
static int power_ok(void *u) { (void)u; return _PowerUsb || _PowerAnyway || CfDev_PowerOk(); }

static void progress(void *u, const cf_status_t *s) { (void)u; _Last = *s; /* TODO(cartflash): OSD panel */ }
static void logm(void *u, const char *m) { (void)u; printf("%s\n", m); fflush(stdout); }   /* always on the console (release log level is WARN) */
/* `cart probe` over USB: one line per probe call on the console (host/cart_probe_usb.py reads
 * it). Called from the nw task: a 1 KiB chunk is ~1.4 KB of text, ~0.12 s at 115200 baud,
 * during which the task waits (the probe waits for the answer anyway). */
static void host_frame(void *u, const uint8_t *f, uint32_t n);
/* control lines take the same path as the frames, so a CPX line (cp/e) never overtakes the
 * frames before it */
static void host_line(void *u, const char *line)
{
    /* cf_esp keeps 2 spare bytes after its line (CF_PB_LINE_N + 2): the newline goes in place,
       so no second ~3 KB copy of the line exists */
    char *const m = (char *)(uintptr_t)line;
    const size_t n = strlen(m);
    m[n] = '\n';
    host_frame(u, (const uint8_t *)m, (uint32_t)(n + 1));
    m[n] = 0;
}
/* a binary probe frame straight into the console UART driver: one call, so no other task's
 * output lands inside it, and no "\n" -> "\r\n" translation (the stdout VFS would do that) */
/* Frames go through a 4 KiB stream buffer to a console task, so the nw task does not sit
 * out each ~1 KB frame at 115200 baud (the REPL installs UART0 with no TX buffer, so a direct
 * uart_write_bytes blocks): the window fetches chunk n+1 while chunk n is on the console.
 * Back-pressure: when the buffer is full the nw task waits, so nothing is ever dropped.
 * One frame = one xStreamBufferSend + one uart_write_bytes of the whole frame, so no other
 * task's output lands inside a frame. */
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
static StreamBufferHandle_t _ConSb;
static uint8_t *_ConF;
/* The path costs ~10.5 KB of heap (stream 4 KB, frame 3 KB, task stack 3 KB). It used to stay
 * for good after the first `cart usb` / probe (unit 1, 2026-10-11: 20 KB free before a job, 9 KB
 * after). Now the task takes it down itself after ~6 s with no cart job and nothing to send.
 * _ConMx: the writers (nw task, console task) and the take-down never meet; it also makes the
 * stream buffer's one-writer rule true. */
static SemaphoreHandle_t _ConMx;
static uint32_t _ArmHeap, _ArmMin, _JobLow;
static uint8_t _ArmJob;
static volatile int _ConKeep;          /* a job or the probe is armed, or a USB job runs (CfDev_Tick) */
static void con_lock(void) { if (_ConMx) (void)xSemaphoreTake(_ConMx, portMAX_DELAY); }
static void con_unlock(void) { if (_ConMx) (void)xSemaphoreGive(_ConMx); }
#define CF_CON_SB    4096u            /* one frame + part of the next */
#define CF_CON_FRAME 3072u            /* probe frames are <= 2059 bytes, CPX lines <= 2970 */
static void con_task(void *arg)
{
    (void)arg;
    uint8_t *const f = _ConF;
    int idle = 0;
    for (;;) {
        uint8_t len2[2];
        if (xStreamBufferReceive(_ConSb, len2, 2, pdMS_TO_TICKS(2000)) != 2) {
            if (_ConKeep) { idle = 0; continue; }
            if (++idle < 3) continue;
            con_lock();                                   /* no writer is inside a send now */
            if (_ConKeep || !xStreamBufferIsEmpty(_ConSb)) { con_unlock(); idle = 0; continue; }
            StreamBufferHandle_t sb = _ConSb;
            _ConSb = NULL;
            vStreamBufferDelete(sb);
            free(_ConF); _ConF = NULL;
            con_unlock();
            vTaskDelete(NULL);                            /* the stack goes back with the task */
        }
        idle = 0;
        const uint16_t n = (uint16_t)(len2[0] | len2[1] << 8);
        size_t got = 0;
        while (got < n) got += xStreamBufferReceive(_ConSb, f + got, n - got, portMAX_DELAY);
        fflush(stdout);
        (void)uart_write_bytes(UART_NUM_0, f, n);
    }
}
/* The console path is made when a probe is armed, never on first use from the nw task (on
 * 2026-10-11 a lazy xStreamBufferCreate returned NULL with ~4 KB of heap left beside a
 * running SHLINK game, and the receive asserted). 0 = ready; -1 = not enough memory. */
static int con_ensure(void)
{
    if (!_ConMx) _ConMx = xSemaphoreCreateMutex();
    if (!_ConMx) return -1;
    _ConKeep = 1;                      /* until the next CfDev_Tick says otherwise: the caller is about to arm */
    con_lock();
    int r = 0;
    if (!_ConSb) {
        r = -1;
        if (esp_get_free_heap_size() >= CF_CON_SB + CF_CON_FRAME + 3072u + 4096u) {   /* + a margin */
            _ConF = malloc(CF_CON_FRAME);
            StreamBufferHandle_t sb = _ConF ? xStreamBufferCreate(CF_CON_SB, 1) : NULL;
            if (!sb) { free(_ConF); _ConF = NULL; }
            else {
                _ConSb = sb;
                if (xTaskCreatePinnedToCore(con_task, "cf_con", 3072, NULL, 3, NULL, 1) != pdPASS) {
                    vStreamBufferDelete(sb); _ConSb = NULL; free(_ConF); _ConF = NULL;
                } else r = 0;
            }
        }
    }
    con_unlock();
    return r;
}
/* 1 = queued for the console task; 0 = there is no console path (the caller may print instead) */
static int con_send(const uint8_t *f, uint32_t n)
{
    con_lock();
    if (!_ConSb) { con_unlock(); return 0; }   /* no console path (refused at arm, or taken down): drop, never assert */
    if (n > CF_CON_FRAME) n = CF_CON_FRAME;
    const uint8_t len2[2] = { (uint8_t)n, (uint8_t)(n >> 8) };
    (void)xStreamBufferSend(_ConSb, len2, 2, portMAX_DELAY);
    size_t sent = 0;
    while (sent < n) sent += xStreamBufferSend(_ConSb, f + sent, n - sent, portMAX_DELAY);
    con_unlock();
    return 1;
}
#ifdef CONFIG_NW_CARTFLASH_WIFI
static int pt_sinking(void);
static void pt_sink(const uint8_t *f, uint32_t n);
static int bk_sinking(void);
static void bk_sink(const uint8_t *f, uint32_t n);
#endif
static void host_frame(void *u, const uint8_t *f, uint32_t n)
{
    (void)u;
#ifdef CONFIG_NW_CARTFLASH_WIFI
    if (pt_sinking()) { pt_sink(f, n); return; }      /* 1.0.5: a patch install reads this cart into the staging area */
    if (bk_sinking()) { bk_sink(f, n); return; }      /* 1.0.6: a backup to the website reads it there too */
#endif
    (void)con_send(f, n);
}

#ifdef CONFIG_NW_CARTFLASH_WIFI
/* ---------------------------------------------------------------- `cart fetch`: the Wi-Fi download
 * (cf_fetch.c) into the image region, over the gateway (nw_dev.c NW_CfRange). Journal: NVS
 * "cf" / "f". A fetched image is staged only after it was read back from the flash. */
int NW_CfRange(const char *rel, uint32_t off, uint16_t max, const uint8_t **data, uint16_t *len, uint32_t *total);
const char *NW_CfLastWhy(void);
static int fe_range(void *u, const char *rel, uint32_t off, uint16_t max, const uint8_t **d, uint16_t *len, uint32_t *total)
{
    (void)u;
    return NW_CfRange(rel, off, max, d, len, total);
}
static uint32_t img_base(void) { return _Part->size - CF_IMG_CAP; }
static int fe_erase(void *u, uint32_t off, uint32_t n) { (void)u; return (_Part && off + n <= CF_IMG_CAP && esp_partition_erase_range(_Part, img_base() + off, n) == ESP_OK) ? 0 : -1; }
static int fe_write(void *u, uint32_t off, const uint8_t *b, uint32_t n) { (void)u; return (_Part && off + n <= CF_IMG_CAP && esp_partition_write(_Part, img_base() + off, b, n) == ESP_OK) ? 0 : -1; }
static int fe_read(void *u, uint32_t off, uint8_t *b, uint32_t n) { (void)u; return (_Part && off + n <= CF_IMG_CAP && esp_partition_read(_Part, img_base() + off, b, n) == ESP_OK) ? 0 : -1; }
static int fe_jl(void *u, void *j, uint32_t n)
{
    (void)u;
    nvs_handle_t h;
    if (nvs_open("cf", NVS_READONLY, &h) != ESP_OK) return -1;
    size_t k = n;
    const esp_err_t e = nvs_get_blob(h, "f", j, &k);
    nvs_close(h);
    return e == ESP_OK && k == n ? 0 : -1;
}
static int fe_js(void *u, const void *j, uint32_t n)
{
    (void)u;
    nvs_handle_t h;
    if (nvs_open("cf", NVS_READWRITE, &h) != ESP_OK) return -1;
    esp_err_t e = nvs_set_blob(h, "f", j, n);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK ? 0 : -1;
}
static void fe_sleep(void *u, uint32_t ms) { (void)u; vTaskDelay(pdMS_TO_TICKS(ms)); }
static cf_fetch_host_t fe_host(void)
{
    const cf_fetch_host_t h = { NULL, CF_IMG_CAP, fe_range, fe_erase, fe_write, fe_read, fe_jl, fe_js, fe_sleep, logm };
    return h;
}
/* a fetched image survives a reboot: staged again if the flash still holds exactly it */
static void fetch_restage(void)
{
    if (!_Part) return;
    const cf_fetch_host_t h = fe_host();
    uint32_t crc = 0;
    const uint32_t sz = cf_fetch_staged(&h, &crc);
    if (sz) { _ImgSize = sz; _Cf.h.img_size = sz; printf("cart: image staged from the last fetch: %lu bytes, crc32 %08lx\n", (unsigned long)sz, (unsigned long)crc); }
}


/* from the nw task, after cf_esp_tick: give the job's buffers back once nothing is armed */
#endif /* CONFIG_NW_CARTFLASH_WIFI */
cf_esp_t *CfDev_Engine(void) { return &_Cf; }
#ifndef CONFIG_NW_CARTFLASH_WIFI
/* (cf_install.c is not in this build) the power rule for a cart write: on the charger, or a valid
   reading above the battery kind's "low" mark (the icon is not red) */
int cf_power_policy(int valid, float level_v, float low_v, int charging) { return charging ? 1 : (valid && level_v > low_v); }
#endif
#include "cf_install.h"
#include "cf_usb.h"
#ifdef CONFIG_NW_CARTFLASH_WIFI
static cf_inst_t _Inst;
static uint32_t _RxHeap0, _RxHeapHeld, _RxHeapLow;
static int _ProbeHold;                  /* `cart rxprobe`: receive mode's link hold is on for the bench probe */   /* receive mode: free heap before the hold, after it, lowest (`cart inst`) */
static void net_housekeep(void);
#endif
static cf_usb_t _Usb;
static uint32_t now_ms(void);
#ifdef CONFIG_NW_CARTFLASH_WIFI
/* The Wi-Fi install needs the cart engine and the boot image select PERMANENTLY in the FPGA (its
   flash): 18.24 or later (18.25 = 18.21 + both, wifiinst1). On 18.21 / 18.22 / 18.23, or before the
   FPGA's version is known, the device never polls: the server then never counts it as able to take
   a send (link_device last_cart_poll_ts), so the app and the site do not offer one. (The USB cart
   page's temporary engine image reports 18.21: no poll under it either, and none is needed.) */
static int wifi_fpga_ok(void)
{
    const uint16_t v = fpga_ver_raw(1);
    const unsigned major = v & 0x3F, minor = (v >> 6) & 0x3F;
    return v != 0 && major == 18 && minor >= 24;
}
#endif
#ifdef CONFIG_NW_CARTFLASH_WIFI
static void dl_cleanup(void);
#endif
static void clr_tick(void);
#ifdef CONFIG_NW_CARTFLASH_WIFI
static void ota_tick(void);
static void play_start_tick(void);
#endif
#ifdef CONFIG_NW_CARTFLASH_WIFI
static void sd_takeback_tick(void);
#endif
static volatile int _GgWant;   /* lane ui110dev: GET GAMES asked for its receive mode (taken by the nw task) */
void CfDev_Tick(void)
{
#ifdef CONFIG_NW_CARTFLASH_WIFI
    if (_GgWant) { _GgWant = 0; cf_install_catalog(&_Inst); }   /* lane ui110dev: GET GAMES asked for receive mode */
    cf_install_tick(&_Inst, now_ms());   /* nothing is called unless the player asked (receive mode) or a report is owed */
#ifdef CONFIG_NW_CARTFLASH_WIFI
    ota_tick();                          /* 1.1.0: the update (only when asked) and a new version's confirm */
    play_start_tick();                   /* 1.1.0: a verified play send starts once its report has gone */
    sd_takeback_tick();                  /* fwshrink1: the card back after a load / an install */
#endif
    if (_Inst.rx) { const uint32_t fh = (uint32_t)esp_get_free_heap_size(); if (!_RxHeapLow || fh < _RxHeapLow) _RxHeapLow = fh; }
#endif
    cf_usb_tick(&_Usb, now_ms());
    clr_tick();
    /* cartspeed2: the express stream's state (~1 KB, made when the job began) goes when the job is over:
       left on the heap it split the space a TLS handshake needs right after an install */
    if (_Cf.xs && !_Cf.armed && !_Cf.done_pending) { free(_Cf.xs); _Cf.xs = NULL; }
    _ConKeep = _Cf.armed || _Cf.probe_armed || _Cf.done_pending || _Usb.job != 0;
#ifdef CONFIG_NW_CARTFLASH_WIFI
    net_housekeep();
    dl_cleanup();
#endif
    {
        /* the heap when a job was armed and its low point while it ran (`cart status`) */
        static int was;
        const int on = _Cf.armed || _Cf.probe_armed;
        const uint32_t fh = esp_get_free_heap_size();
        if (on && !was) { _ArmHeap = fh; _ArmMin = esp_get_minimum_free_heap_size(); _JobLow = fh; _ArmJob = _Cf.j.job; }
        if (on && fh < _JobLow) _JobLow = fh;
        was = on;
    }
    if (_Cf.armed || _Cf.probe_armed || (!_Index && !_Blk)) return;
    if (_BlkNo != 0xFFFFFFFFu) (void)blk_commit();
    index_flush();
    free(_Index); _Index = NULL;
    free(_Blk); _Blk = NULL;
}

/* ---------------------------------------------------------------- "Send to my Chromatic"
 * (cf_install.c): poll the account's pending cart install, read its header, ask on the OSD,
 * STREAM it straight to the cart (cf_esp.c CF_JOB_STREAM), report. Driven from the nw task by
 * CfDev_Tick. Nothing is staged or backed up on the ESP32 (owner rulings 2026-10-11).
 *
 * The stream's own HTTPS connection (cf_net_*), and why it is its own (unit 1, cart-wifi-test,
 * 2026-10-11: InstrFetchProhibited in esp_http_client_connect about every 64 KiB, free heap
 * down to 588 B): the shared gateway client closes itself after every request once the free
 * heap is under 14 KB (nw_dev.c kLowHeap), so under a download every range opened a NEW
 * connection, and a connect that runs out of heap half way calls a transport that was never
 * made. Here: ONE keep-alive connection for the whole image, opened only with enough free heap
 * (a wait, never a try), every allocation checked, and nothing else talks TLS meanwhile (the
 * 'downloading' report goes out before the job is armed; CwlOnline_TlsHold holds the rest). */
#include "cf_install.h"
#include "cf_sha256.h"
#include "cf_usb.h"
#include "cwl.h"
#include "cwl_online.h"
#include "nw_dev.h"
#include "cwl_sec.h"
#include "cwl_wdt.h"
#include "esp_crt_bundle.h"
#include "cwl_tls.h"
#include "cwl_time.h"
#include "esp_http_client.h"
#include "esp_wifi.h"
#include "mbedtls/aes.h"
#include "mbedtls/platform_util.h"
const char *NW_CfGatewayBase(void);
void NW_CfOpenMenu(void);
void NW_FwOpenMenu(void);
void NW_CfCloseGateway(void);
static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
__attribute__((unused)) static void heap_note(const char *stage)
{
    printf("cf: heap [%s] free %lu, min %lu, largest block %lu\n", stage, (unsigned long)esp_get_free_heap_size(),
           (unsigned long)esp_get_minimum_free_heap_size(), (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
}

#ifdef CONFIG_NW_CARTFLASH_WIFI

/* ---- the stream connection */
enum { NET_IDLE, NET_WANT, NET_READY, NET_GONE };
#define CF_NET_TLS_FREE   19000u       /* the handshake's measured peak for this host, ~15.2 KB, + margin */
#define CF_NET_TLS_BLOCK  8192u        /* and its largest single piece must find room */
#define CF_NET_PLAIN_HEAP 9000u
static struct {
    volatile int on;                   /* started (net_start) and not yet closed by the HTTP task */
    volatile int state, stop;
    uint32_t next_try, backoff;
    int fresh;                         /* the connection is being opened: note the heap once */
    volatile uint32_t off;
    volatile uint16_t len;
    uint32_t size;                     /* the whole image: every answer's total must be this */
    char url[CF_BLOB_URL_MAX + 1];     /* the blob URL from the poll (plain http: the token in it is the only credential) */
    mbedtls_aes_context aes;           /* AES-256 with the install's key (RAM only; freed + zeroed on close) */
    uint8_t iv[16];                    /* the first counter block */
    int aes_on;
    uint8_t *buf;                      /* CF_PROG_CHUNK, heap, for the job */
    esp_http_client_handle_t cli;
    unsigned n_get, n_conn, n_err, n_heap_waits, min_heap;
    char why[48];
} _Net;

/* 1.1.7: the real time from our server's Date when nothing set the clock yet */
static esp_err_t net_date_ev(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_HEADER && e->header_key && e->header_value && !strcasecmp(e->header_key, "Date")) (void)CwlTime_FromHttpDate(e->header_value);
    return ESP_OK;
}
static void net_close(void)
{
    if (_Net.cli) { esp_http_client_close(_Net.cli); esp_http_client_cleanup(_Net.cli); _Net.cli = NULL; }
}
/* the key goes when the install's connection does */
static void net_forget_key(void)
{
    if (_Net.aes_on) { mbedtls_aes_free(&_Net.aes); _Net.aes_on = 0; }
    mbedtls_platform_zeroize(&_Net.aes, sizeof _Net.aes);
    mbedtls_platform_zeroize(_Net.iv, sizeof _Net.iv);
    mbedtls_platform_zeroize(_Net.url, sizeof _Net.url);
}
/* AES-256-CTR over the image bytes off..off+n, in place (cf_ctr_xor: the counter rule, host-tested) */
static int net_ecb(void *u, const uint8_t in[16], uint8_t out[16]) { (void)u; return mbedtls_aes_crypt_ecb(&_Net.aes, MBEDTLS_AES_ENCRYPT, in, out); }
static int net_decrypt(uint32_t off, uint8_t *b, uint32_t n)
{
    if (!_Net.aes_on) return -1;
    return cf_ctr_xor(net_ecb, NULL, _Net.iv, off, b, n);
}

/* One request on the cart install's ONE connection (keep-alive: the header read, the reports and
 * the whole image go over the same TLS session). range != NULL: a ranged GET, the body goes to
 * dst (cap bytes). body != NULL: a POST of JSON, the answer goes to dst. scheme: "Device " for
 * the gateway, "Bearer " for the device API. Returns the HTTP status, or -1 (not now: try later;
 * the reason is in _Net.why). */
static int net_req(const char *url, const char *scheme, const char *range, const char *body, char *dst, int cap, int *got)
{
    *got = 0;
    if (!CWL_IsOnline()) {
        CWL_SetNetworkWanted(true);
        CWL_NetworkUse(false, true);
        snprintf(_Net.why, sizeof _Net.why, "wifi down");
        return -1;
    }
    const int tls = !strncmp(url, "https:", 6);
    if (_Net.cli == NULL) {
        /* A new connection = a TLS handshake. Measured for this host on unit 1 (0.18.0-rc1,
           "heap 23736 before, low 8524"): it takes ~15.2 KB at its peak. Open only with that
           plus a margin free, and with a largest block that can hold its biggest single piece;
           otherwise wait (never try: a failed allocation inside the handshake has crashed). */
        const uint32_t free_now = esp_get_free_heap_size();
        const uint32_t big = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
        if (tls ? (free_now < CF_NET_TLS_FREE || big < CF_NET_TLS_BLOCK) : free_now < CF_NET_PLAIN_HEAP) {
            _Net.n_heap_waits++;
            snprintf(_Net.why, sizeof _Net.why, "heap %lu / block %lu", (unsigned long)free_now, (unsigned long)big);
            return -1;
        }
        heap_note("before the handshake");
        esp_http_client_config_t cfg = {
            .url = url, .timeout_ms = 8000, .buffer_size = 1024, .buffer_size_tx = 1536,
            .keep_alive_enable = true, .method = HTTP_METHOD_GET, .event_handler = net_date_ev,
        };
        if (tls) cfg.crt_bundle_attach = CwlTls_Attach;
        _Net.cli = esp_http_client_init(&cfg);
        if (_Net.cli == NULL) { snprintf(_Net.why, sizeof _Net.why, "client alloc"); return -1; }
        _Net.n_conn++;
        _Net.fresh = 1;
    } else if (esp_http_client_set_url(_Net.cli, url) != ESP_OK) { net_close(); snprintf(_Net.why, sizeof _Net.why, "url"); return -1; }
    esp_http_client_handle_t c = _Net.cli;
    esp_err_t e = ESP_OK;
    int have_tok = 1;
    if (scheme) {
        /* (the TLS gateway route only: the plain blob connection never carries a token) */
        const size_t sl = strlen(scheme);
        char *auth = malloc(800);
        if (!auth) { snprintf(_Net.why, sizeof _Net.why, "auth alloc"); return -1; }
        memcpy(auth, scheme, sl);
        have_tok = CwlOnline_CopyToken(auth + sl, 800 - sl) > 0;
        e = have_tok ? esp_http_client_set_header(c, "Authorization", auth) : ESP_FAIL;
        memset(auth, 0, 800);
        free(auth);
    } else (void)esp_http_client_delete_header(c, "Authorization");
    (void)esp_http_client_set_method(c, body ? HTTP_METHOD_POST : HTTP_METHOD_GET);
    (void)esp_http_client_delete_header(c, "Range");
    (void)esp_http_client_delete_header(c, "Content-Type");
    if (e == ESP_OK && range) e = esp_http_client_set_header(c, "Range", range);
    if (e == ESP_OK && body) e = esp_http_client_set_header(c, "Content-Type", "application/json");
    if (e != ESP_OK || esp_http_client_set_header(c, "User-Agent", "ShinyChromatic-cart/2") != ESP_OK) {
        snprintf(_Net.why, sizeof _Net.why, have_tok ? "header alloc" : "not paired");
        net_close();
        return -1;
    }
    const int blen = body ? (int)strlen(body) : 0;
    const int wdt = CwlWdt_LongBegin();          /* a new connection's handshake: seconds without a yield */
    e = esp_http_client_open(c, blen);
    CwlWdt_LongEnd(wdt);
    if (_Net.fresh) { _Net.fresh = 0; heap_note("after the handshake"); }
    if (e != ESP_OK) { snprintf(_Net.why, sizeof _Net.why, "open %s", esp_err_to_name(e)); net_close(); return -1; }
    if (blen && esp_http_client_write(c, body, blen) != blen) { snprintf(_Net.why, sizeof _Net.why, "write"); net_close(); return -1; }
    const int64_t clen = esp_http_client_fetch_headers(c);
    const int code = esp_http_client_get_status_code(c);
    if (clen < 0) { snprintf(_Net.why, sizeof _Net.why, "no answer"); net_close(); return -1; }
    int n = 0;
    while (n < cap) {
        const int r = esp_http_client_read(c, dst + n, cap - n);
        if (r <= 0) break;
        n += r;
    }
    int drained = 0;
    (void)esp_http_client_flush_response(c, &drained);
    if (!esp_http_client_is_complete_data_received(c)) net_close();   /* never reuse a half-read connection */
    *got = n;
    return code;
}

/* one ranged GET of the CIPHERTEXT into _Net.buf, decrypted there: 0 ok, 1 try again later, 2 the server says no.
   The blob URL is plain http (wire format section 2): no Authorization header, no cookie. */
static int net_get(uint32_t off, uint16_t len)
{
    if (!_Net.url[0]) { snprintf(_Net.why, sizeof _Net.why, "no blob url"); return 2; }
    char hv[48];
    snprintf(hv, sizeof hv, "bytes=%lu-%lu", (unsigned long)off, (unsigned long)(off + len - 1));
    int n = 0;
    const int code = net_req(_Net.url, NULL, hv, NULL, (char *)_Net.buf, (int)len, &n);
    if (code < 0) return 1;
    if (code != 206 && code != 200) {
        snprintf(_Net.why, sizeof _Net.why, "http %d", code);
        net_close();
        return (code == 404 || code == 401 || code == 403 || code == 410 || code == 416) ? 2 : 1;
    }
    if (n != (int)len) { snprintf(_Net.why, sizeof _Net.why, "short %d/%u", n, (unsigned)len); net_close(); return 1; }
    if (code == 200 && !(off == 0 && len == _Net.size)) { snprintf(_Net.why, sizeof _Net.why, "range ignored"); net_close(); return 1; }
    if (net_decrypt(off, _Net.buf, len)) { snprintf(_Net.why, sizeof _Net.why, "decrypt"); return 1; }
    _Net.n_get++;
    return 0;
}

/* The connection's work runs on the game traffic's HTTP task (nw_dev.c HttpTask calls
 * CfDev_HttpWork every pass and polls every 10 ms while it returns 1): no task of its own, so
 * no 6 KB stack beside the TLS session (unit 1, 2026-10-11: ~20 KB free on the patch build).
 * A cart job runs the boot stub, not a game: that task has nothing else to do then. Only it
 * opens, uses and closes the connection. */
static int dl_work(void);
static int dl_running(void);
static volatile int _StageErasing;
static int stage_erase_step(void);
static int bk_work(void);
static int up_work(void);                          /* lane savecloud1: SAVE TO MY GAMES's upload */
static int ota_dl_work(void);
static int fota_work(void);
int CfDev_HttpWork(void)
{
    if (_StageErasing) return stage_erase_step();   /* cartspeed2 (nothing else reads or writes the staging area meanwhile) */
    if (bk_work()) return 1;                        /* 1.0.6: a backup's hash and upload (its own plain connection) */
    if (ota_dl_work()) return 1;                    /* 1.1.0: an update into the idle slot */
    if (fota_work()) return 1;                      /* 1.1.0: the FPGA boot flash (never on the nw task) */
    if (up_work()) return 1;                        /* lane savecloud1: a kept game's save up to My games (this task has the TLS stack) */
    if (!_Net.on) return 0;
    if (_Net.stop) { net_close(); net_forget_key(); _Net.on = 0; return 0; }
    if (dl_running()) return dl_work();          /* wifispeed1: the whole image in one GET, into the staging area */
    if (_Net.state != NET_WANT) return 1;
    if ((int32_t)(now_ms() - _Net.next_try) < 0) return 1;
    const uint32_t off = _Net.off;
    const uint16_t len = _Net.len;
    const int r = net_get(off, len);
    const uint32_t fh = esp_get_free_heap_size();
    if (!_Net.min_heap || fh < _Net.min_heap) _Net.min_heap = fh;
    if (_Net.stop) return 1;
    if (r == 0) { if (_Net.off == off && _Net.len == len) _Net.state = NET_READY; _Net.backoff = 500; return 1; }
    if (r == 2) { _Net.state = NET_GONE; return 1; }
    _Net.n_err++;
    if ((_Net.n_err & 7) == 1) printf("cf: stream: waiting at %lu (%s; free heap %lu)\n", (unsigned long)off, _Net.why, (unsigned long)fh);
    if (_Net.backoff < 500) _Net.backoff = 500;
    _Net.next_try = now_ms() + _Net.backoff;      /* no sleep here: the task stays the game traffic's */
    if (_Net.backoff < 8000) _Net.backoff *= 2;
    return 1;
}

static int net_start(const cf_offer_t *o)
{
    if (_Net.on) return _Net.stop ? -1 : 0;       /* the last one is still ending: later */
    free(_Net.buf);
    net_forget_key();
    memset(&_Net, 0, sizeof _Net);
    snprintf(_Net.url, sizeof _Net.url, "%s", o->blob_url);
    _Net.size = o->size;
    mbedtls_aes_init(&_Net.aes);
    if (mbedtls_aes_setkey_enc(&_Net.aes, o->key, 256)) { mbedtls_aes_free(&_Net.aes); return -1; }
    memcpy(_Net.iv, o->iv, 16);
    _Net.aes_on = 1;
    _Net.buf = malloc(CF_PROG_CHUNK);
    if (!_Net.buf) return -1;
    NW_CfCloseGateway();                          /* the game traffic's idle connection gives its memory back */
    _Net.on = 1;
    return 0;
}
/* the install's connection is let go when nothing of an install is under way any more (CfDev_Tick) */
static void net_housekeep(void)
{
    /* the plain connection is let go as soon as the job is over (and after the header read): the
       reports then go over the device's TLS session on the link task, never beside it */
    if (_Net.on && !_Net.stop && !_Cf.armed && !_Cf.done_pending && _Inst.st != CI_PEEK && !dl_running()) _Net.stop = 1;
    if (!_Net.on && _Net.stop && _Net.buf) { free(_Net.buf); _Net.buf = NULL; heap_note("connection closed"); }
}
/* cf_host_t.chunk for the gateway source */
static int net_chunk(uint32_t off, uint16_t n, uint8_t *dst)
{
    if (!_Net.on || !_Net.buf) return 0;
    if (_Net.state == NET_GONE) return -1;
    if (_Net.state == NET_READY && _Net.off == off && _Net.len == n) { if (dst) memcpy(dst, _Net.buf, n); _Net.state = NET_IDLE; return 1; }
    if (_Net.state == NET_IDLE || _Net.off != off || _Net.len != n) {
        _Net.off = off; _Net.len = n; _Net.state = NET_WANT;
    }
    return 0;
}
#endif /* CONFIG_NW_CARTFLASH_WIFI */
/* ---- the staging area: the ROM and the save the computer wrote into this flash in bootloader
 * mode (cf_usb.h CF_STAGE_*). Outside every data partition: the unused end of the app partition
 * and the 960 KiB behind it. Read with esp_flash; erased by a short-lived task (the whole area
 * takes about 20 s: not something to do on the nw task). */
#include "esp_flash.h"
#include "esp_ota_ops.h"
#include "esp_image_format.h"
static volatile int _StageErasing;
static int stage_read(void *u, int sav, uint32_t off, uint8_t *b, uint32_t n)
{
    (void)u;
    const uint32_t base = sav ? CF_STAGE_SAV_OFF : CF_STAGE_ROM_OFF, cap = sav ? CF_RAM_SIZE : CF_STAGE_ROM_MAX;
    if (off + n > cap) return -1;
    /* `stage rom` hashes up to 2 MiB in one go in the console task: let the idle task run every
     * 64 KiB, or the 5 s task watchdog prints its warning in the middle of the answer */
    if (!sav && off && !(off & 0xFFFFu)) vTaskDelay(1);
    return esp_flash_read(NULL, b, base + off, n) == ESP_OK ? 0 : -1;
}
/* cartspeed2: staging reads for the cart write (and the USB stage hash) through two memory-mapped
 * 64 KiB windows instead of esp_flash_read: a read of the flash chip stops the cache on BOTH cores
 * (and every task but the IRAM interrupts) for each call, and the express stream made ~4,000 such
 * 1 KiB reads per 2 MiB while a Wi-Fi download ran beside it (its receive buffers overflowed). A
 * window is mapped once per block (that does stop the cache, once). Only blocks that are complete
 * are read this way (the cart write waits for img_avail; the download's own blank checks and its
 * re-hash use esp_flash_read), the windows are dropped whenever the staging area is erased or a
 * download starts over, and the per-block crc32 of the downloaded stream is compared with what the
 * cart write reads (img_blk_crc): a stale window can never reach block 0. */
#include "spi_flash_mmap.h"
static struct { uint32_t base; const uint8_t *p; spi_flash_mmap_handle_t h; uint32_t used; } _Mw[2];
static uint32_t _MwTick;
static SemaphoreHandle_t _MwMx;
static void mw_drop(void)
{
    if (_MwMx) xSemaphoreTake(_MwMx, portMAX_DELAY);
    for (int i = 0; i < 2; i++) { if (_Mw[i].p) spi_flash_munmap(_Mw[i].h); _Mw[i].p = NULL; _Mw[i].base = 0; }
    if (_MwMx) xSemaphoreGive(_MwMx);
}
/* bytes [addr, addr + n) of the flash (inside the staging area) into b: 0 ok */
static int mw_read(uint32_t addr, uint8_t *b, uint32_t n);
/* lane gamecomp1: the staged container read through the mapped windows (1 KiB esp_flash_read calls: 4 MiB in 24 s on unit 2,
   while the radio runs); dropped after each unpack so a later write to the staging area is never read stale */
int CfDev_FlashReadMm(uint32_t addr, uint8_t *b, uint32_t n) { return mw_read(addr, b, n); }
void CfDev_FlashMmDrop(void) { mw_drop(); }
static int mw_read(uint32_t addr, uint8_t *b, uint32_t n)
{
    if (!_MwMx) return esp_flash_read(NULL, b, addr, n) == ESP_OK ? 0 : -1;   /* (made at boot: CfDev_Init) */
    xSemaphoreTake(_MwMx, portMAX_DELAY);
    int r = 0;
    while (n && !r) {
        const uint32_t base = addr & ~0xFFFFu, k = (base + 0x10000u - addr) < n ? (base + 0x10000u - addr) : n;
        int w = -1;
        for (int i = 0; i < 2; i++) if (_Mw[i].p && _Mw[i].base == base) w = i;
        if (w < 0) {
            w = !_Mw[0].p ? 0 : !_Mw[1].p ? 1 : (_Mw[0].used <= _Mw[1].used ? 0 : 1);
            if (_Mw[w].p) { spi_flash_munmap(_Mw[w].h); _Mw[w].p = NULL; }
            const void *v = NULL;
            if (spi_flash_mmap(base, 0x10000u, SPI_FLASH_MMAP_DATA, &v, &_Mw[w].h) != ESP_OK || !v) { r = esp_flash_read(NULL, b, addr, n) == ESP_OK ? 0 : -1; break; }
            _Mw[w].p = (const uint8_t *)v; _Mw[w].base = base;
        }
        _Mw[w].used = ++_MwTick;
        memcpy(b, _Mw[w].p + (addr - base), k);
        b += k; addr += k; n -= k;
    }
    xSemaphoreGive(_MwMx);
    return r;
}
static int stage_read_mm(void *u, int sav, uint32_t off, uint8_t *b, uint32_t n)
{
    (void)u;
    const uint32_t base = sav ? CF_STAGE_SAV_OFF : CF_STAGE_ROM_OFF, cap = sav ? CF_RAM_SIZE : CF_STAGE_ROM_MAX;
    if (off + n > cap) return -1;
    if (!sav && off && !(off & 0xFFFFu)) vTaskDelay(1);
    return mw_read(base + off, b, n);
}
/* 0 = the running firmware image ends below the staging area */
static int stage_room(void *u)
{
    (void)u;
    static int known = -1;
    if (known >= 0) return known;
    const esp_partition_t *p = esp_ota_get_running_partition();
    esp_image_metadata_t m;
    memset(&m, 0, sizeof m);
    const esp_partition_pos_t pos = { .offset = p ? p->address : 0, .size = p ? p->size : 0 };
    if (!p || esp_image_get_metadata(&pos, &m) != ESP_OK) return 1;      /* not known: do not stage */
    /* 1.1.0: the running image must touch neither the save's place nor the ROM's (the idle slot) */
    const uint32_t a = p->address, e = p->address + m.image_len;
    const int hit_sav = a < CF_STAGE_SAV_OFF + CF_RAM_SIZE && e > CF_STAGE_SAV_OFF;
    const int hit_rom = a < CF_STAGE_ROM_OFF + CF_STAGE_ROM_MAX && e > CF_STAGE_ROM_OFF;
    known = (hit_sav || hit_rom) ? 1 : 0;
    if (known) printf("cart: this firmware image (%06lx-%06lx) overlaps the staging area (save %06lx, ROM %06lx): no staged install\n",
                      (unsigned long)a, (unsigned long)e, (unsigned long)CF_STAGE_SAV_OFF, (unsigned long)CF_STAGE_ROM_OFF);
    return known;
}
/* Erasing and writing the staging area from the running firmware.
 *
 * The stock partition table gives the factory app 0x10000 + 0x300000, so 0x208000-0x310000 is
 * INSIDE the running app's partition (behind the image), and the IDF refuses any raw erase /
 * write there: with CONFIG_SPI_FLASH_DANGEROUS_WRITE_ABORTS it aborts (usb-cart-2 and -3 on a
 * unit: abort in esp_flash_erase_region, the staged ROM and save stayed in the flash). That
 * guard stays as it is for everything else in the firmware. The staging area gets its own
 * view of the main flash chip: the same chip, driver, lock and cache handling, with ONE
 * function replaced, the "may this region be written" check, by ours: exactly
 * 0x208000-0x400000, and only when the firmware image ends below it (stage_room). Every
 * erase and write of the staging area goes through stage_chip(); none uses the default chip. */
static esp_err_t stage_region_ok(void *arg, size_t start, size_t size)
{
    (void)arg;
    /* 1.1.0: nothing is staged while a new version is not confirmed (the other slot is its way back) */
    if (cf_stage_hold) return ESP_ERR_NOT_SUPPORTED;
    /* 1.1.0: exactly the save's 32 KiB, or the ROM's place (the slot that is not running) */
    const int in_sav = start >= CF_STAGE_SAV_OFF && start + size <= CF_STAGE_SAV_OFF + CF_RAM_SIZE;
    const int in_rom = start >= CF_STAGE_ROM_OFF && start + size <= CF_STAGE_ROM_OFF + CF_STAGE_ROM_MAX;
    if (!(in_sav || in_rom)) return ESP_ERR_NOT_SUPPORTED;
    return stage_room(NULL) ? ESP_ERR_NOT_SUPPORTED : ESP_OK;
}
static esp_flash_t *stage_chip(void)
{
    static esp_flash_os_functions_t os;
    static esp_flash_t chip;
    static int made;
    if (!made) {
        if (!esp_flash_default_chip || !esp_flash_default_chip->os_func) return NULL;
        os = *esp_flash_default_chip->os_func;
        os.region_protected = stage_region_ok;
        chip = *esp_flash_default_chip;
        chip.os_func = &os;
        made = 1;
    }
    return &chip;
}
/* for the Wi-Fi install (the download goes to the same area); `off` from 0x208000 */
__attribute__((unused)) static int stage_write(uint32_t flash_off, const uint8_t *b, uint32_t n)
{
    esp_flash_t *ch = stage_chip();
    return ch && esp_flash_write(ch, b, flash_off, n) == ESP_OK ? 0 : -1;
}
static volatile int _StageEraseFailed;
/* cartspeed2: the erase of the staged copy runs after the install has said `done` (the page does not
   wait for it). A restart in the middle must not leave the copy behind: the record NVS "cf"/"clr"
   (crc32 of the staged ROM's first 4 KiB) is written before the erase starts and removed when the
   area reads back blank; the ROM is erased from the top down with its first 64 KiB LAST, so after a
   restart the first 4 KiB still match the record until the very end (CfDev_Init finishes the job). */
static uint32_t _ClrAt, _ClrCmds;      /* a clear owed from before the restart: due at; the console's staging commands then */
static int stage_head(uint32_t *crc)   /* 1 = the staged ROM's first 4 KiB are all FF */
{
    uint8_t b[256];
    uint32_t x = 0;
    int ff = 1;
    for (uint32_t o = 0; o < 4096u; o += sizeof b) {
        if (esp_flash_read(NULL, b, CF_STAGE_ROM_OFF + o, sizeof b) != ESP_OK) return 1;
        for (unsigned i = 0; i < sizeof b && ff; i++) if (b[i] != 0xFF) ff = 0;
        x = cf_crc32(x, b, sizeof b);
    }
    *crc = x;
    return ff;
}
static void clr_mark(int on, uint32_t crc)
{
    nvs_handle_t h;
    if (nvs_open("cf", NVS_READWRITE, &h) != ESP_OK) return;
    if (on) (void)nvs_set_u32(h, "clr", crc); else (void)nvs_erase_key(h, "clr");
    (void)nvs_commit(h);
    nvs_close(h);
}
static int clr_get(uint32_t *crc)
{
    nvs_handle_t h;
    if (nvs_open("cf", NVS_READONLY, &h) != ESP_OK) return -1;
    const esp_err_t e = nvs_get_u32(h, "clr", crc);
    nvs_close(h);
    return e == ESP_OK ? 0 : -1;
}
static int blank64(uint32_t a)        /* 1 = the 64 KiB at a read all FF (no erase needed) */
{
    uint8_t b[256];
    for (uint32_t o = 0; o < 0x10000u; o += sizeof b) {
        if (esp_flash_read(NULL, b, a + o, sizeof b) != ESP_OK) return 0;
        for (unsigned i = 0; i < sizeof b; i++) if (b[i] != 0xFF) return 0;
    }
    return 1;
}
/* cartspeed2: the erase runs in steps on the nw HTTP task (CfDev_HttpWork, one 64 KiB block or 16 KiB of
   read-back per call) instead of a task of its own: its 4 KiB stack came out of the heap right after an
   install, exactly when the 'installed' report and the next receive need a TLS handshake (unit 1: that
   report got -1 with 712 bytes left). */
static struct { uint32_t a, t0; uint8_t ph; uint8_t bad; uint16_t n_er; } _Er;
static void stage_erase(void *u)
{
    (void)u;
    if (_StageErasing) return;
    _ClrAt = 0;
    mw_drop();
    { uint32_t x = 0; if (!stage_head(&x)) clr_mark(1, x); }
    memset(&_Er, 0, sizeof _Er);
    _Er.t0 = now_ms();
    _StageErasing = 1;
}
/* one step (the HTTP task): 1 = more to do */
static int stage_erase_step(void)
{
    if (!_StageErasing) return 0;
    esp_flash_t *ch = stage_chip();
    if (!ch) _Er.bad = 1;
    if (_Er.bad) goto end;
    if (_Er.ph == 0) {                                /* the save */
        if (esp_flash_erase_region(ch, CF_STAGE_SAV_OFF, CF_RAM_SIZE) != ESP_OK) _Er.bad = 1;
        _Er.ph = 1; _Er.a = CF_STAGE_ROM_OFF + CF_STAGE_ROM_MAX - 0x10000u;
        return 1;
    }
    if (_Er.ph == 1) {                                /* the ROM, 64 KiB at a time from the top down, its first block last */
        if (!blank64(_Er.a)) { if (esp_flash_erase_region(ch, _Er.a, 0x10000u) != ESP_OK) _Er.bad = 1; _Er.n_er++; }
        if (_Er.a <= CF_STAGE_ROM_OFF) { _Er.ph = 2; _Er.a = CF_STAGE_SAV_OFF; } else _Er.a -= 0x10000u;
        return 1;
    }
    {                                                 /* "nothing of the ROM or the save stays on the Chromatic" is a promise: look */
        /* 1.1.0: the save, then the ROM (they are apart when ota_1 runs: the ROM is below the save) */
        uint8_t b[256];
        for (uint32_t k = 0; k < 64u && !_Er.bad; k++, _Er.a += sizeof b) {
            if (_Er.ph == 2 && _Er.a >= CF_STAGE_SAV_OFF + CF_RAM_SIZE) { _Er.ph = 3; _Er.a = CF_STAGE_ROM_OFF; }
            if (_Er.ph == 3 && _Er.a >= CF_STAGE_ROM_OFF + CF_STAGE_ROM_MAX) break;
            if (esp_flash_read(NULL, b, _Er.a, sizeof b) != ESP_OK) { _Er.bad = 1; break; }
            for (unsigned i = 0; i < sizeof b; i++) if (b[i] != 0xFF) { _Er.bad = 1; break; }
        }
        if (!_Er.bad && !(_Er.ph == 3 && _Er.a >= CF_STAGE_ROM_OFF + CF_STAGE_ROM_MAX)) return 1;
    }
end:
    if (_Er.bad) printf("cart: the staging area did NOT erase: a copy of the ROM / save is still in this unit's flash (cart usb clear)\n");
    else { clr_mark(0, 0); printf("cart: the staged copy is erased (%u blocks of 64 KiB, %lu ms)\n", (unsigned)_Er.n_er, (unsigned long)(now_ms() - _Er.t0)); }
    _StageEraseFailed = _Er.bad;
    _StageErasing = 0;
    return 0;
}
/* 1 while the erase runs; then 0 = erased and read back blank, -1 = it failed */
static int stage_busy(void *u) { (void)u; return _StageErasing ? 1 : _StageEraseFailed ? -1 : 0; }

/* the staged ROM for a Wi-Fi install (source 3): an image may be a full 2 MiB whose last 64 KiB are
   FF (the cart's protected top block, which stays blank); the staging area holds 0x1F0000 bytes, the
   download checks that tail is all FF (and hashes it), and it reads back as FF here */
static int wstage_read2(uint32_t off, uint8_t *dst, uint32_t n, int mm)
{
    uint32_t k = 0;
    if (off < CF_STAGE_ROM_MAX) {
        k = CF_STAGE_ROM_MAX - off < n ? CF_STAGE_ROM_MAX - off : n;
        if ((mm ? stage_read_mm : stage_read)(NULL, 0, off, dst, k)) return -1;
    }
    if (k < n) memset(dst + k, 0xFF, n - k);
    return 0;
}
static int wstage_read(uint32_t off, uint8_t *dst, uint32_t n) { return wstage_read2(off, dst, n, 0); }
#ifdef CONFIG_NW_CARTFLASH_WIFI
/* ---- wifispeed1: the image in ONE plain-http GET, decrypted into the staging area, its sha256 +
 * crc32 checked BEFORE the cart is touched; then the stream job writes the cart from there (source
 * CF_SRC_WIFI_STAGED, fast verify: CRC16 per bank on the Game Boy side against the checked bytes).
 * Before (1.0.3-rc2): one ranged GET per 2 KiB chunk, asked only when the job needed it (stop and
 * wait, ~2 KB/s), and the whole cart read back over the 115,200-baud FPGA link at the end.
 * A drop: the same GET again from the byte it stopped at (Range: bytes=N-), the hashes carry on.
 * A restart: the journal's unfinished install comes back with its offer; the staged bytes are
 * hashed again and used if they are the image, else downloaded again. The staging area is erased
 * (and read back blank) when the install is over. Runs on the nw HTTP task (CfDev_HttpWork). */
#include "mbedtls/sha256.h"
enum { DL_IDLE, DL_RUN, DL_CHECK, DL_OK, DL_FAIL };
#define DL_BUF        2048u                    /* one read + one flash write (heap is tight beside the TCP window) */
#define DL_SLICE      (32u * 1024u)            /* bytes per HTTP-task turn (the task is the game traffic's too) */
#define DL_STALL_MS   (10u * 60u * 1000u)      /* no byte for this long: the download stops (the install pauses) */
#define DL_BLK        0x10000u                 /* the staging area is checked blank / erased per 64 KiB */
typedef struct {
    volatile int st;
    char id[33];
    uint32_t size, crc_want, crc, have, blank_to;
    uint32_t rom_len;                                  /* 1.0.9: bytes of the image that are the GAME (to the ROM region); the rest is the 32 KiB save (to the save region) */
    int sav_blank;                                     /* 1.0.9: the staging save region is checked blank (or erased) for this download */
    uint8_t sha_want[32];
    mbedtls_sha256_context sha;
    int sha_on, tries, dirty;
    esp_http_client_handle_t cli;
    uint8_t *buf;
    uint32_t t0, t_hdr, t_done, last_byte, next_try, backoff;
    uint32_t ms_net, ms_flash, ms_erase, ms_crypt;   /* where the time goes */
    uint32_t ms_open, ms_hdr, n_reads, low_heap;       /* the connection's open, its headers; reads; lowest free heap */
    /* AES-CTR state carried from read to read (one hardware AES run per read, not one call per 16 bytes) */
    uint32_t ctr_at;                                   /* the image offset the state below is for (~0u: not set) */
    size_t   nc_off;
    uint8_t  nonce[16], stream[16];
    unsigned n_conn, n_err, n_erase;
    int ps_save;                                       /* gamecomp1: the Wi-Fi power save to put back, + 1 (0: none) */
    char why[48];
    uint32_t epoch;                                    /* cartspeed2: +1 at every hash restart (the staged bytes before it are void) */
    uint32_t ms_aes, ms_sha, ms_crc, n_skip;           /* the crypt share split; 256-byte pages not written (all FF on a blank area) */
    uint32_t bcrc[32];                                 /* cartspeed2: crc32 of the image's bytes in each 64 KiB block, as they came (img_blk_crc) */
    int to_ram, ram_ready;                             /* 1.1.0: a "play" send bigger than the staging area: straight into PSRAM (CfRc_Ram*) */
    int to_sd, sd_have;                                /* lane sdcard1: a send to the microSD card (written as it comes, crash safe) / already there */
    sd_wr_t *sdw;
    char sd_path[SDC_PATH_MAX];
    int z, zcard;                                      /* lane gamecomp1: the staged image is an SGZ1 container / for the card (unpacked there) */
    int to_lib, lib_end;                               /* lane gamecomp1: written straight into the FPGA flash's library (a stream), its end asked */
    cf_lib_tx_t tx;
    /* lane mygames1: a play send's save after the game's object (bytes sv_from.. of the download, sv_len of them): into the
       save RAM (PSRAM, sv_ram: its writer set up) or, for the card, its own file (svw: /SAVES/<the game file's name>.sav).
       sv_only: the game is on the card already, only the save is downloaded (its crc32 is then the check) */
    uint32_t sv_from, sv_len;
    int sv_ram, sv_only;
    sd_wr_t *svw;
    char sv_path[SDC_PATH_MAX];
} dl_state_t;
/* heap, only from an install's first arm until its staged copy is erased (static RAM is the TLS gate's) */
static dl_state_t *_DlP;
/* cartspeed2: the cart write starts while the image still downloads (the express stream only: it
   waits per 64 KiB block for the bytes, and block 0 + the save RAM wait for the whole image's
   crc32 + sha256). `cart xs prog off`: the old order (the whole download first). */
static int _XsProgOff;
static int _DlProg;                    /* the armed job was armed while the download ran */
static uint32_t _ArmEpoch;             /* the download epoch the armed job reads */
#define DL_PROG_MIN 0x10000u           /* the first 64 KiB staged before an early arm (the job's first look at block 0) */
#define _Dl (*_DlP)
static int dl_running(void) { return _DlP && (_Dl.st == DL_RUN || _Dl.st == DL_CHECK); }
static void dl_close(void)
{
    if (_Dl.cli) { esp_http_client_close(_Dl.cli); esp_http_client_cleanup(_Dl.cli); _Dl.cli = NULL; }
}
static void dl_hash_reset(void)
{
    _Dl.epoch++;
    memset(_Dl.bcrc, 0, sizeof _Dl.bcrc);
    mw_drop();
    if (_Dl.sha_on) mbedtls_sha256_free(&_Dl.sha);
    mbedtls_sha256_init(&_Dl.sha);
    (void)mbedtls_sha256_starts(&_Dl.sha, 0);
    _Dl.sha_on = 1;
    _Dl.crc = 0;
    _Dl.have = 0;
    _Dl.ctr_at = ~0u;
    if (_Dl.to_lib) cf_lib_tx_rewind(&_Dl.tx);   /* lane gamecomp1: the library's sectors written again from the start */
    _Dl.sv_ram = 0;                               /* lane mygames1: the save RAM's writer set up again at the save's first byte */
    if (_Dl.sv_only) _Dl.have = _Dl.sv_from;      /* ... and only the save is downloaded */
}
/* gamecomp1 (as fpgacomp1 dc494d1 for an update): a play / save download runs with Wi-Fi power save off. In modem sleep
   (the link's WIFI_PS_MAX_MODEM) the access point holds each piece of the answer for the next beacon: unit 1 2026-10-22, a
   1 MiB raw send came at 2,298 B/s (514 reads in 446 s of network time) */
static void dl_ps_off(void)
{
    wifi_ps_type_t ps;
    if (_Dl.ps_save || esp_wifi_get_ps(&ps) != ESP_OK) return;
    printf("cf: download: Wi-Fi power save %d at the start%s\n", (int)ps, ps != WIFI_PS_NONE ? ": off for the download" : "");
    if (ps != WIFI_PS_NONE && esp_wifi_set_ps(WIFI_PS_NONE) == ESP_OK) _Dl.ps_save = (int)ps + 1;
}
static void dl_ps_back(void)
{
    if (_Dl.ps_save) { (void)esp_wifi_set_ps((wifi_ps_type_t)(_Dl.ps_save - 1)); _Dl.ps_save = 0; }   /* the radio's own rule again */
}
static void dl_end(int st, const char *why)
{
    dl_close();
    dl_ps_back();
    if (st != DL_OK && _Dl.sdw) { if (_Dl.sdw->open) Sd_WriteAbort(_Dl.sdw); free(_Dl.sdw); _Dl.sdw = NULL; }
    if (st != DL_OK && _Dl.svw) { if (_Dl.svw->open) Sd_WriteAbort(_Dl.svw); free(_Dl.svw); _Dl.svw = NULL; }
    if (st != DL_OK && _Dl.sv_ram && !_Dl.to_ram) { CfRc_RamRelease(); _Dl.sv_ram = 0; }   /* (lane mygames1: the core back to the cartridge) */
    free(_Dl.buf); _Dl.buf = NULL;
    _Dl.st = st;
    _Dl.t_done = now_ms();
    if (why) snprintf(_Dl.why, sizeof _Dl.why, "%s", why);
}
/* the hashes of 0..size against the offer's */
static void dl_final(void)
{
    uint8_t d[32];
    (void)mbedtls_sha256_finish(&_Dl.sha, d);
    mbedtls_sha256_free(&_Dl.sha); _Dl.sha_on = 0;
    const uint32_t ms = now_ms() - _Dl.t0;
    const int hash_ok = _Dl.crc == _Dl.crc_want && (_Dl.sv_only || !memcmp(d, _Dl.sha_want, 32));   /* (lane mygames1: a save alone: its crc32) */
    if (hash_ok && _Dl.svw) {
        /* lane mygames1: the save that came with a card game takes its place beside it; a save that was there keeps its bytes
           under another name (nothing of the player's is lost) */
        if (Sd_Exists(_Dl.sv_path)) {
            char old[SDC_PATH_MAX], stem[64];
            const char *b = strrchr(_Dl.sv_path, '/');
            snprintf(stem, sizeof stem, "%.*s OLD.sav", (int)(strlen(b + 1) > 4 ? strlen(b + 1) - 4 : 0), b + 1);
            if (Sd_FreeName(SDC_SAVES_DIR, stem, old, sizeof old) == 0 && Sd_Rename(_Dl.sv_path, old) == 0) printf("cf: download: the save there before is kept as %s\n", old);
        }
        const int se = Sd_WriteCommit(_Dl.svw);
        free(_Dl.svw); _Dl.svw = NULL;
        if (se) { dl_end(DL_FAIL, "card"); _Net.stop = 1; return; }
        printf("cf: download: its save on the card as %s\n", _Dl.sv_path);
    }
    if (hash_ok && _Dl.sv_only) {
        Sd_RefreshSpace();
        printf("cf: download: the save alone (the game is on the card already), crc32 ok, %lu ms\n", (unsigned long)ms);
        if (!CfRc_SdPlaying() && Sd_Busy() == kSdOp_None) Sd_Lend();
        dl_end(DL_OK, "ok");
        _Net.stop = 1;
        return;
    }
    if (hash_ok && _Dl.to_sd && _Dl.sdw) {
        /* lane sdcard1: the whole file is right: it takes its place on the card */
        const int ce = Sd_WriteCommit(_Dl.sdw);
        free(_Dl.sdw); _Dl.sdw = NULL;
        if (ce) { printf("cf: download: the card did not take %s\n", _Dl.sd_path); mbedtls_sha256_free(&_Dl.sha); _Dl.sha_on = 0; dl_end(DL_FAIL, "card"); _Net.stop = 1; return; }
        Sd_RefreshSpace();
        printf("cf: download: on the card as %s\n", _Dl.sd_path);
        /* the report's call comes next and wants the heap a new connection takes (~21 KB): the card's goes back
           (a game played from the card after the report mounts it again) */
        if (!CfRc_SdPlaying() && Sd_Busy() == kSdOp_None) Sd_Lend();
    }
    if (hash_ok) {
        printf("cf: download: %lu bytes in %s, crc32 + sha256 ok: %lu ms (%lu B/s; first byte after %lu ms; net %lu [open %lu, headers %lu, %lu reads], decrypt %lu [aes %lu, sha %lu, crc %lu], flash %lu [%lu FF pages skipped], erase %lu ms; %u connections, %u retries, %u erases; lowest free heap %lu)%s\n",
               (unsigned long)_Dl.size, _Dl.to_sd ? "the microSD card" : _Dl.to_ram ? "the FPGA's RAM" : "the staging area", (unsigned long)ms, (unsigned long)((uint64_t)_Dl.size * 1000u / (ms ? ms : 1)), (unsigned long)(_Dl.t_hdr - _Dl.t0),
               (unsigned long)_Dl.ms_net, (unsigned long)_Dl.ms_open, (unsigned long)_Dl.ms_hdr, (unsigned long)_Dl.n_reads,
               (unsigned long)_Dl.ms_crypt, (unsigned long)(_Dl.ms_aes / 1000u), (unsigned long)(_Dl.ms_sha / 1000u), (unsigned long)(_Dl.ms_crc / 1000u),
               (unsigned long)_Dl.ms_flash, (unsigned long)_Dl.n_skip, (unsigned long)_Dl.ms_erase, _Dl.n_conn, _Dl.n_err, _Dl.n_erase, (unsigned long)_Dl.low_heap,
               _DlProg ? " (the cart was written meanwhile)" : "");
        dl_end(DL_OK, "ok");
        _Net.stop = 1;                             /* the connection and the key go: the cart is written from the staged bytes */
        return;
    }
    printf("cf: download: the staged image is NOT the image (crc32 %08lx, wanted %08lx): %s\n", (unsigned long)_Dl.crc,
           (unsigned long)_Dl.crc_want, ++_Dl.tries < 2 ? "downloaded again" : "given up");
    if (_Dl.tries < 2) { dl_hash_reset(); _Dl.blank_to = 0; _Dl.ram_ready = 0; _Dl.st = DL_RUN; return; }
    dl_end(DL_FAIL, "the image did not hash right");
}
/* the 64 KiB block that holds byte `off` of the staged ROM is blank (checked), or erased now */
static int dl_blank(uint32_t off)
{
    if (off < _Dl.blank_to) return 0;
    const uint32_t blk = off & ~(DL_BLK - 1u);
    uint8_t b[256];
    int dirty = 0;
    for (uint32_t a = blk; a < blk + DL_BLK && !dirty; a += sizeof b) {
        if (stage_read(NULL, 0, a, b, sizeof b)) return -1;
        for (unsigned i = 0; i < sizeof b; i++) if (b[i] != 0xFF) { dirty = 1; break; }
    }
    if (dirty) {
        const uint32_t t = now_ms();
        esp_flash_t *ch = stage_chip();
        if (!ch || esp_flash_erase_region(ch, CF_STAGE_ROM_OFF + blk, DL_BLK) != ESP_OK) return -1;
        _Dl.ms_erase += now_ms() - t;
        _Dl.n_erase++;
    }
    _Dl.blank_to = blk + DL_BLK;
    return 0;
}
/* a restart came back to an unfinished install: are the staged bytes this image? (hashed in slices) */
/* 1.0.9: byte `off` of the downloaded image, staged: the game's bytes in the ROM region, the save's in the save region */
static int dl_stage_read(uint32_t off, uint8_t *b, uint32_t n)
{
    if (off >= _Dl.rom_len) return stage_read(NULL, 1, off - _Dl.rom_len, b, n);
    return wstage_read(off, b, n);
}
/* 1.0.9: the staging save region blank (checked), erased when it is not */
static int dl_sav_blank(void)
{
    uint8_t b[256];
    for (uint32_t a = 0; a < CF_RAM_SIZE; a += sizeof b) {
        if (stage_read(NULL, 1, a, b, sizeof b)) return -1;
        for (unsigned i = 0; i < sizeof b; i++) if (b[i] != 0xFF) {
            esp_flash_t *ch = stage_chip();
            return (!ch || esp_flash_erase_region(ch, CF_STAGE_SAV_OFF, CF_RAM_SIZE) != ESP_OK) ? -1 : 0;
        }
    }
    return 0;
}
static int dl_check_slice(void)
{
    const uint32_t end = _Dl.have + DL_SLICE < _Dl.size ? _Dl.have + DL_SLICE : _Dl.size;
    while (_Dl.have < end) {
        const uint32_t k = end - _Dl.have < DL_BUF ? end - _Dl.have : DL_BUF;
        const uint32_t kk = (_Dl.have < _Dl.rom_len && _Dl.have + k > _Dl.rom_len) ? _Dl.rom_len - _Dl.have : k;   /* (never across the game / save edge) */
        if (dl_stage_read(_Dl.have, _Dl.buf, kk)) { dl_hash_reset(); _Dl.st = DL_RUN; return 1; }
        (void)mbedtls_sha256_update(&_Dl.sha, _Dl.buf, kk);
        _Dl.crc = cf_crc32(_Dl.crc, _Dl.buf, kk);
        if (_Dl.have < _Dl.rom_len) _Dl.bcrc[_Dl.have >> 16] = cf_crc32(_Dl.bcrc[_Dl.have >> 16], _Dl.buf, kk);
        _Dl.have += kk;
    }
    if (_Dl.have < _Dl.size) return 1;
    uint8_t d[32];
    (void)mbedtls_sha256_finish(&_Dl.sha, d);
    if (_Dl.crc == _Dl.crc_want && !memcmp(d, _Dl.sha_want, 32)) {
        mbedtls_sha256_free(&_Dl.sha); _Dl.sha_on = 0;
        printf("cf: download: the staging area already holds the image (crc32 + sha256 ok, %lu ms): no download\n", (unsigned long)(now_ms() - _Dl.t0));
        dl_end(DL_OK, "staged");
        _Net.stop = 1;
        return 0;
    }
    printf("cf: download: the staging area does not hold the image: downloading it\n");
    dl_hash_reset();
    _Dl.blank_to = 0;
    _Dl.st = DL_RUN;
    return 1;
}
/* decrypt bytes off..off+n of the image in place: mbedtls' CTR over the whole read (counter block j =
   iv + j, the same rule as cf_ctr_xor, host-tested); the state continues when reads are back to back */
static int dl_decrypt(uint32_t off, uint8_t *b, uint32_t n)
{
    if (!_Net.aes_on) return -1;
    if (_Dl.ctr_at != off) {                   /* (re)start at any offset: counter = iv + off / 16 */
        memcpy(_Dl.nonce, _Net.iv, 16);
        uint32_t add = off >> 4;
        for (int i = 15; i >= 0 && add; i--) { const uint32_t v = _Dl.nonce[i] + (add & 0xFFu); _Dl.nonce[i] = (uint8_t)v; add = (add >> 8) + (v >> 8); }
        _Dl.nc_off = 0;
        if (off & 15u) {
            if (mbedtls_aes_crypt_ecb(&_Net.aes, MBEDTLS_AES_ENCRYPT, _Dl.nonce, _Dl.stream)) return -1;
            for (int i = 15; i >= 0; i--) if (++_Dl.nonce[i]) break;
            _Dl.nc_off = off & 15u;
        }
    }
    if (mbedtls_aes_crypt_ctr(&_Net.aes, n, &_Dl.nc_off, _Dl.nonce, _Dl.stream, b, b)) return -1;
    _Dl.ctr_at = off + n;
    return 0;
}
/* cartspeed2: the staging area is blank where the download writes (dl_blank): 256-byte flash pages
   that are all FF are not written (a ROM's padding costs nothing); the rest in runs */
static int stage_write_nonff(uint32_t flash_off, const uint8_t *b, uint32_t n)
{
    uint32_t i = 0, run = 0, run_n = 0;
    while (i < n) {
        const uint32_t pg = 256u - ((flash_off + i) & 255u);
        const uint32_t k = n - i < pg ? n - i : pg;
        int ff = 1;
        for (uint32_t q = 0; q < k; q++) if (b[i + q] != 0xFF) { ff = 0; break; }
        if (ff) {
            if (run_n && stage_write(flash_off + run, b + run, run_n)) return -1;
            run_n = 0;
            if (_DlP) _Dl.n_skip++;                     /* (lane sdcard1: also used with no download: the card dump, write to cart) */
        } else {
            if (!run_n) run = i;
            run_n += k;
        }
        i += k;
    }
    return run_n ? stage_write(flash_off + run, b + run, run_n) : 0;
}
/* one turn of the download on the HTTP task: 1 = call again soon */
static int dl_work(void)
{
    if (!_Dl.buf && !(_Dl.buf = malloc(DL_BUF))) { snprintf(_Dl.why, sizeof _Dl.why, "no memory"); return 1; }
    if (_Dl.st == DL_CHECK) return dl_check_slice();
    const uint32_t now = now_ms();
    if ((int32_t)(now - _Dl.last_byte) >= (int32_t)DL_STALL_MS) {
        printf("cf: download: nothing for %u s at %lu of %lu (%s): stopped\n", (unsigned)(DL_STALL_MS / 1000u), (unsigned long)_Dl.have,
               (unsigned long)_Dl.size, _Dl.why);
        dl_end(DL_FAIL, "no data");
        return 0;
    }
    if ((int32_t)(now - _Dl.next_try) < 0) return 1;
    if (_Dl.cli == NULL) {
        if (!CWL_IsOnline()) { CWL_SetNetworkWanted(true); CWL_NetworkUse(false, true); snprintf(_Dl.why, sizeof _Dl.why, "wifi down"); goto retry; }
        if (esp_get_free_heap_size() < CF_NET_PLAIN_HEAP) { _Net.n_heap_waits++; snprintf(_Dl.why, sizeof _Dl.why, "heap"); goto retry; }
        esp_http_client_config_t cfg = {
            .url = _Net.url, .timeout_ms = 8000, .buffer_size = 2048, .buffer_size_tx = 512,
            .keep_alive_enable = true, .method = HTTP_METHOD_GET, .event_handler = net_date_ev,
        };
        if (!strncmp(_Net.url, "https:", 6)) cfg.crt_bundle_attach = CwlTls_Attach;   /* (the server's blob URL is plain http) */
        _Dl.cli = esp_http_client_init(&cfg);
        if (!_Dl.cli) { snprintf(_Dl.why, sizeof _Dl.why, "client alloc"); goto retry; }
        char hv[40];
        snprintf(hv, sizeof hv, "bytes=%lu-", (unsigned long)_Dl.have);
        if (esp_http_client_set_header(_Dl.cli, "Range", hv) != ESP_OK ||
            esp_http_client_set_header(_Dl.cli, "User-Agent", "ShinyChromatic-cart/3") != ESP_OK) { snprintf(_Dl.why, sizeof _Dl.why, "header alloc"); goto drop; }
        _Dl.n_conn++;
        dl_ps_off();
        const uint32_t t = now_ms();
        const int wdt = CwlWdt_LongBegin();
        const esp_err_t e = esp_http_client_open(_Dl.cli, 0);
        CwlWdt_LongEnd(wdt);
        _Dl.ms_open += now_ms() - t;
        if (e != ESP_OK) { snprintf(_Dl.why, sizeof _Dl.why, "open %s", esp_err_to_name(e)); goto drop; }
        const uint32_t th = now_ms();
        const int64_t clen = esp_http_client_fetch_headers(_Dl.cli);
        _Dl.ms_hdr += now_ms() - th;
        const int code = esp_http_client_get_status_code(_Dl.cli);
        _Dl.ms_net += now_ms() - t;
        if (code == 404 || code == 410 || code == 403 || code == 401 || code == 416) {
            printf("cf: download: the server says %d: the image is gone\n", code);
            dl_end(DL_FAIL, "gone");
            return 0;
        }
        if (!((code == 206) || (code == 200 && _Dl.have == 0)) || clen != (int64_t)(_Dl.size - _Dl.have)) {
            snprintf(_Dl.why, sizeof _Dl.why, "http %d, %lld bytes for %lu", code, (long long)clen, (unsigned long)(_Dl.size - _Dl.have));
            goto drop;
        }
        if (!_Dl.t_hdr) _Dl.t_hdr = now_ms();
        printf("cf: download: %s at %lu of %lu (connection %u, free heap %lu)\n", _Dl.have ? "resumed" : "started", (unsigned long)_Dl.have,
               (unsigned long)_Dl.size, _Dl.n_conn, (unsigned long)esp_get_free_heap_size());
    }
    {
        const uint32_t slice_end = _Dl.have + DL_SLICE;
        while (_Dl.have < _Dl.size && _Dl.have < slice_end) {
            const int in_rom = _Dl.have < _Dl.rom_len;
            if (_Dl.to_ram && !_Dl.ram_ready) {          /* 1.1.0: PSRAM, the core held, before the first byte */
                const int re = CfRc_RamBegin();
                if (re) { printf("cf: download: the FPGA's RAM cannot take the game (%d)\n", re); dl_end(DL_FAIL, "the FPGA's RAM cannot be written"); return 0; }
                _Dl.ram_ready = 1;
            }
            if (!_Dl.to_ram && !_Dl.to_sd && !_Dl.to_lib && in_rom && _Dl.have < CF_STAGE_ROM_MAX && dl_blank(_Dl.have)) { dl_end(DL_FAIL, "the staging area cannot be written"); return 0; }
            if (!in_rom && !_Dl.sav_blank && !_Dl.to_sd && !_Dl.sv_len) {   /* 1.0.9: the save goes to the staging save region (blank, checked) */
                if (dl_sav_blank()) { dl_end(DL_FAIL, "the staging area cannot be written"); return 0; }
                _Dl.sav_blank = 1;
            }
            /* a read never crosses a 64 KiB block (the block it lands in is blank), nor the game / save edge */
            const uint32_t to_blk = DL_BLK - (_Dl.have & (DL_BLK - 1u));
            uint32_t want = _Dl.size - _Dl.have;
            if (want > DL_BUF) want = DL_BUF;
            if (want > to_blk) want = to_blk;
            if (in_rom && want > _Dl.rom_len - _Dl.have) want = _Dl.rom_len - _Dl.have;
            uint32_t got = 0, t = now_ms();
            while (got < want) {
                const int r = esp_http_client_read(_Dl.cli, (char *)_Dl.buf + got, (int)(want - got));
                if (r <= 0) break;
                got += (uint32_t)r;
                _Dl.n_reads++;
                { const uint32_t fh = esp_get_free_heap_size(); if (!_Dl.low_heap || fh < _Dl.low_heap) _Dl.low_heap = fh; }
            }
            _Dl.ms_net += now_ms() - t;
            if (got == 0) { snprintf(_Dl.why, sizeof _Dl.why, "the connection ended at %lu", (unsigned long)_Dl.have); goto drop; }
            t = now_ms();
            int64_t u0 = esp_timer_get_time();
            if (dl_decrypt(_Dl.have, _Dl.buf, got)) { snprintf(_Dl.why, sizeof _Dl.why, "decrypt"); dl_end(DL_FAIL, "decrypt"); return 0; }
            int64_t u1 = esp_timer_get_time();
            (void)mbedtls_sha256_update(&_Dl.sha, _Dl.buf, got);
            int64_t u2 = esp_timer_get_time();
            _Dl.crc = cf_crc32(_Dl.crc, _Dl.buf, got);
            if (in_rom && !_Dl.to_ram && !_Dl.to_sd) _Dl.bcrc[_Dl.have >> 16] = cf_crc32(_Dl.bcrc[_Dl.have >> 16], _Dl.buf, got);   /* (a read never crosses a block; the game's bytes only) */
            const int64_t u3 = esp_timer_get_time();
            _Dl.ms_aes += (uint32_t)(u1 - u0); _Dl.ms_sha += (uint32_t)(u2 - u1); _Dl.ms_crc += (uint32_t)(u3 - u2);   /* (microseconds) */
            _Dl.ms_crypt += now_ms() - t;
            t = now_ms();
            if (_Dl.sv_len && !in_rom) {
                /* lane mygames1: the save that came with the game: its card file, or the save RAM */
                const uint32_t so = _Dl.have - _Dl.sv_from;
                if (_Dl.svw) { if (Sd_WriteAt(_Dl.svw, so, _Dl.buf, got)) { dl_end(DL_FAIL, "card"); return 0; } }
                else {
                    if (!_Dl.sv_ram) {
                        if (!_Dl.to_ram && CfRc_RamBegin()) { dl_end(DL_FAIL, "the FPGA's RAM cannot be written"); return 0; }
                        _Dl.sv_ram = 1;
                        if (CfRc_RamRegion(CF_RC_RAM_BASE, CF_RC_RAM_MAX)) { dl_end(DL_FAIL, "the FPGA's RAM cannot be written"); return 0; }
                    }
                    if (CfRc_RamPut(so, _Dl.buf, got)) { dl_end(DL_FAIL, "the FPGA's RAM cannot be written"); return 0; }
                }
            } else if (_Dl.to_lib) {
                const int le = CfRc_LibTxWrite(&_Dl.tx, _Dl.buf, got);
                if (le) { printf("cf: download: the library did not take the bytes at %lu (%d)\n", (unsigned long)_Dl.have, le); dl_end(DL_FAIL, "library"); return 0; }
            } else if (_Dl.to_sd) {
                if (!_Dl.sdw || Sd_WriteAt(_Dl.sdw, _Dl.have, _Dl.buf, got)) { dl_end(DL_FAIL, "card"); return 0; }
            } else if (_Dl.to_ram) {
                if (CfRc_RamPut(_Dl.have, _Dl.buf, got)) { dl_end(DL_FAIL, "the FPGA's RAM cannot be written"); return 0; }
            } else if (!in_rom) {
                if (_Dl.have - _Dl.rom_len + got > CF_RAM_SIZE || stage_write(CF_STAGE_SAV_OFF + (_Dl.have - _Dl.rom_len), _Dl.buf, got)) { dl_end(DL_FAIL, "the staging area cannot be written"); return 0; }
            } else if (_Dl.have >= CF_STAGE_ROM_MAX) {
                /* the cart's protected top block: a full 2 MiB image must be FF there (not stored) */
                for (uint32_t i = 0; i < got; i++) if (_Dl.buf[i] != 0xFF) { printf("cf: download: the image has data in the cart's protected top block\n"); dl_end(DL_FAIL, "does not fit"); return 0; }
            } else if (stage_write_nonff(CF_STAGE_ROM_OFF + _Dl.have, _Dl.buf, got)) { dl_end(DL_FAIL, "the staging area cannot be written"); return 0; }
            _Dl.ms_flash += now_ms() - t;
            if (!_Dl.to_ram && !_Dl.to_sd && !_Dl.to_lib) _Dl.dirty = 1;   /* 1.1.0: a PSRAM (or card) download stages nothing (the kept game stays) */
            _Dl.have += got;
            _Dl.last_byte = now_ms();
            _Dl.backoff = 500;
        }
    }
    if (_Dl.have >= _Dl.size) {
        if (_Dl.sv_ram ? CfRc_RamEnd(_Dl.sv_len) : (_Dl.to_ram && CfRc_RamEnd(_Dl.size))) { dl_end(DL_FAIL, "the FPGA's RAM cannot be written"); return 0; }
        dl_close(); dl_final(); return 1;
    }
    return 1;
drop:
    dl_close();
retry:
    _Dl.n_err++;
    if ((_Dl.n_err & 7) == 1) printf("cf: download: waiting at %lu of %lu (%s; free heap %lu)\n", (unsigned long)_Dl.have, (unsigned long)_Dl.size, _Dl.why,
                                     (unsigned long)esp_get_free_heap_size());
    if (_Dl.backoff < 500) _Dl.backoff = 500;
    _Dl.next_try = now_ms() + _Dl.backoff;
    if (_Dl.backoff < 8000) _Dl.backoff *= 2;
    return 1;
}
/* ci_arm: start (or go on with, or check) the download of this offer. CF_ARM_WAIT while it runs;
   0 when the staging area holds the image; CF_ARM_NO_DATA when it stopped for good */
static int dl_for(const cf_offer_t *o)
{
    if (!_DlP && !(_DlP = calloc(1, sizeof *_DlP))) return CF_ARM_WAIT;   /* (memory: a moment later) */
    const int same = !strcmp(_Dl.id, o->id) && _Dl.size == o->size && (_Dl.crc_want == o->crc32 || (_Dl.sv_only && _Dl.crc_want == o->save_crc32));
    if (o->kind == CF_KIND_ROMSAVE && (o->rom_size < 0x8000u || o->rom_size > CF_STAGE_ROM_MAX || o->size != o->rom_size + CF_RAM_SIZE)) return CF_D_BAD_IMAGE;
    if (o->kind == CF_KIND_SAVE && !o->sd && o->size != CF_RAM_SIZE) return CF_D_BAD_IMAGE;
    if (same && _Dl.st == DL_OK) return 0;
    if (same && dl_running()) return CF_ARM_WAIT;
    if (same && _Dl.st == DL_FAIL) {               /* said once; the next arm (a resume) downloads again */
        const int r = !strcmp(_Dl.why, "gone") ? CF_D_GONE : !strcmp(_Dl.why, "does not fit") ? CF_D_BAD_IMAGE : !strcmp(_Dl.why, "card") ? CF_ARM_CARD_FAIL : CF_ARM_NO_DATA;
        _Dl.st = DL_IDLE; _Dl.id[0] = 0;
        return r;
    }
    const int card = o->sd != CF_SD_NONE;            /* lane sdcard1: for the microSD card */
    /* lane gamecomp1: an SGZ1 container is staged whole (it is at most the staging area) and unpacked from there (into PSRAM
       when it plays, onto the card as the plain game file when it is for the card) */
    const int to_sd = card && !o->z;                 /* written onto the card as it comes */
    const uint32_t osz = o->kind == CF_KIND_PLAY ? CF_OBJ_SIZE(o) : o->size;   /* lane mygames1: the game's part (a save may follow it) */
    const int to_ram = !card && !o->z && o->kind == CF_KIND_PLAY && osz > CF_STAGE_ROM_MAX;   /* 1.1.0: too big to keep: into PSRAM */
    /* lane gamecomp1: a container for a unit with a library goes straight into the library when it fits there (kept, then
       played from it); else into the staging area (played in RAM only) when it fits that; else it cannot be taken */
    cf_lib_tx_t *ptx = (!card && o->z && o->kind == CF_KIND_PLAY && o->lib != CF_LIB_NONE) ? malloc(sizeof *ptx) : NULL;   /* (lane ui110dev: "none" keeps nothing) */   /* (heap: the nw stack is small, static RAM short) */
    const int to_lib = ptx && CfRc_LibPlanStream(osz, o->title, ptx) == 0;
    if (to_lib) _Dl.tx = *ptx;
    free(ptx);
    if (o->z && !card && !to_lib && osz > CF_STAGE_ROM_MAX) { printf("cf: play: %lu bytes do not fit this Chromatic's library now\n", (unsigned long)osz); return CF_ARM_LIB_FULL; }
    const uint32_t file_size = o->z ? o->rom_size : osz, file_crc = o->z ? o->rom_crc32 : (o->kind == CF_KIND_PLAY ? CF_OBJ_CRC(o) : o->crc32);   /* (the file on the card) */
    if (osz == 0) return CF_D_BAD_IMAGE;
    {   /* m110t: too big for the place is said as that (CF_ARM_TOO_BIG), not as a damaged send */
        const uint32_t max = to_sd ? (o->kind == CF_KIND_SAVE ? CF_SD_SAVE_MAX : o->sd == CF_SD_KEEP ? CF_SD_GAME_MAX : CF_PLAY_MAX) : (to_ram || to_lib) ? CF_PLAY_MAX : 0x200000u;
        if (osz > max) { printf("cf: %lu bytes: more than the %lu its place takes\n", (unsigned long)osz, (unsigned long)max); return o->kind == CF_KIND_PLAY ? CF_ARM_TOO_BIG : CF_D_BAD_IMAGE; }
    }
    if (cf_hex32(o->sha256, _Dl.sha_want)) return CF_D_BAD_IMAGE;
    char sdp[SDC_PATH_MAX] = "";
    int sd_have = 0;
    if (card) {
        if (Sd_Mount() != 0 || !Sd_Writable()) { printf("cf: card send: no card that takes it\n"); return CF_ARM_NO_CARD; }
        uint32_t fmb = 0;
        Sd_Space(NULL, &fmb);
        if ((uint64_t)fmb * 1048576u < (uint64_t)file_size + 0x40000u) { printf("cf: card send: the card is full (%lu MB free)\n", (unsigned long)fmb); return CF_ARM_CARD_FULL; }
        if (o->kind == CF_KIND_SAVE) snprintf(sdp, sizeof sdp, SDC_SAVES_DIR "/%s", o->file);
        else {
            /* the same game already there under this name: not downloaded again */
            snprintf(sdp, sizeof sdp, SDC_GAMES_DIR "/%s", o->file);
            uint32_t have_sz = 0;
            if (Sd_Exists(sdp) && Sd_FileSize(sdp, &have_sz) == 0 && have_sz == file_size) {
                FIL *f = malloc(sizeof *f);
                uint8_t *b = malloc(4096);
                uint32_t crc = 0, got = 0, sz = 0;
                if (f && b && Sd_ReadOpen(f, sdp, &sz) == 0) {
                    for (uint32_t off = 0; off < sz; off += got) { if (Sd_Read(f, off, b, 4096, &got) || !got) { crc = ~file_crc; break; } crc = cf_crc32(crc, b, got); }
                    Sd_Close(f);
                    sd_have = crc == file_crc;
                }
                free(f); free(b);
            }
            if (!sd_have && Sd_FreeName(SDC_GAMES_DIR, o->file, sdp, sizeof sdp)) return CF_ARM_CARD_FAIL;
        }
    }
    if (_StageErasing && !to_sd && !sd_have) return CF_ARM_WAIT;     /* the last install's staged copy is still being erased */
    if (!to_ram && !to_sd && !to_lib && stage_room(NULL)) return CF_D_BAD_IMAGE;     /* (a firmware image that reaches into the staging area) */
    if (sd_have && o->kind == CF_KIND_PLAY && o->save_size) {
        /* lane mygames1: the game is there; only its save is downloaded (below, from the save's first byte) */
        printf("cf: card send: %s is on the card already (the same crc32): only its save is downloaded\n", sdp);
    } else if (sd_have) {
        printf("cf: card send: %s is on the card already (the same crc32): not downloaded again\n", sdp);
        snprintf(_Dl.id, sizeof _Dl.id, "%s", o->id);
        _Dl.size = o->size; _Dl.crc_want = o->crc32;
        _Dl.to_sd = 1; _Dl.sd_have = 1; _Dl.to_ram = 0; _Dl.dirty = 0; _Dl.z = 0; _Dl.zcard = 0;
        snprintf(_Dl.sd_path, sizeof _Dl.sd_path, "%s", sdp);
        _Dl.st = DL_OK;
        return 0;
    }
    if (net_start(o)) return CF_ARM_WAIT;           /* the header read's connection still closing: a moment later */
    free(_Net.buf); _Net.buf = NULL;                 /* (the per-chunk buffer: not used by the download, 2 KiB of heap back) */
    if (!to_ram && !to_sd && !to_lib) CfRc_GameGone();    /* 1.1.0: the kept game lives where this download goes (a PSRAM one leaves it) */
    snprintf(_Dl.id, sizeof _Dl.id, "%s", o->id);
    _Dl.size = o->size; _Dl.crc_want = o->crc32;
    /* 1.0.9: a save alone is all save; a game + save is the game, then 32 KiB of save */
    _Dl.rom_len = o->kind == CF_KIND_SAVE ? 0 : o->kind == CF_KIND_ROMSAVE ? o->rom_size : osz;
    /* lane mygames1: a save after the game: where it goes (its card file is opened below) */
    _Dl.sv_len = o->kind == CF_KIND_PLAY ? o->save_size : 0;
    _Dl.sv_from = osz; _Dl.sv_ram = 0;
    _Dl.sv_only = _Dl.sv_len && sd_have;
    if (_Dl.svw) { if (_Dl.svw->open) Sd_WriteAbort(_Dl.svw); free(_Dl.svw); _Dl.svw = NULL; }
    _Dl.sav_blank = 0;
    dl_hash_reset();
    _Dl.tries = 0; _Dl.blank_to = 0;
    _Dl.n_conn = _Dl.n_err = _Dl.n_erase = 0;
    dl_ps_back();                                      /* (a download that never ended: its rule back first) */
    _Dl.ms_net = _Dl.ms_flash = _Dl.ms_erase = _Dl.ms_crypt = 0;
    _Dl.ms_open = _Dl.ms_hdr = _Dl.n_reads = _Dl.low_heap = 0;
    _Dl.ms_aes = _Dl.ms_sha = _Dl.ms_crc = _Dl.n_skip = 0;
    _Dl.ctr_at = ~0u;
    _Dl.t0 = _Dl.last_byte = now_ms(); _Dl.t_hdr = _Dl.t_done = 0;
    _Dl.next_try = 0; _Dl.backoff = 500;
    _Dl.why[0] = 0;
    /* a restart in the middle of this install: the staged bytes may already be the image */
    char jid[33]; uint8_t src = 0;
    _Dl.st = (!_Dl.sv_len && cf_esp_stream_pending(&_Cf, jid, NULL, &src) && src == CF_SRC_WIFI_STAGED && !strcmp(jid, o->id)) ? DL_CHECK : DL_RUN;
    _Dl.dirty = 1;
    _Dl.to_ram = to_ram; _Dl.ram_ready = 0;
    if (to_ram) { _Dl.st = DL_RUN; _Dl.dirty = 0; }   /* nothing staged: nothing to check first, nothing to erase after */
    _Dl.to_sd = to_sd; _Dl.sd_have = 0;
    _Dl.z = o->z; _Dl.zcard = card && o->z;
    _Dl.to_lib = to_lib; _Dl.lib_end = 0;
    if (to_lib) { _Dl.st = DL_RUN; _Dl.dirty = 0; }   /* (nothing staged: nothing to check first or erase after) */
    if (_Dl.zcard) snprintf(_Dl.sd_path, sizeof _Dl.sd_path, "%s", sdp);
    if (to_sd && !_Dl.sv_only) {
        _Dl.st = DL_RUN; _Dl.dirty = 0;
        snprintf(_Dl.sd_path, sizeof _Dl.sd_path, "%s", sdp);
        if (_Dl.sdw) { if (_Dl.sdw->open) Sd_WriteAbort(_Dl.sdw); free(_Dl.sdw); }
        _Dl.sdw = malloc(sizeof *_Dl.sdw);
        if (!_Dl.sdw || Sd_WriteBegin(_Dl.sdw, sdp, o->kind == CF_KIND_SAVE ? 0 : osz)) { free(_Dl.sdw); _Dl.sdw = NULL; _Dl.id[0] = 0; return CF_ARM_CARD_FAIL; }
    }
    if (card && _Dl.sv_len) {
        /* lane mygames1: the save's own file beside the game: /SAVES/<the game file's name without its extension>.sav */
        const char *b = strrchr(sdp, '/');
        b = b ? b + 1 : sdp;
        const char *dot = strrchr(b, '.');
        snprintf(_Dl.sv_path, sizeof _Dl.sv_path, SDC_SAVES_DIR "/%.*s.sav", (int)(dot ? dot - b : (int)strlen(b)), b);
        _Dl.svw = malloc(sizeof *_Dl.svw);
        if (!_Dl.svw || Sd_WriteBegin(_Dl.svw, _Dl.sv_path, _Dl.sv_len)) { free(_Dl.svw); _Dl.svw = NULL; if (_Dl.sdw) { if (_Dl.sdw->open) Sd_WriteAbort(_Dl.sdw); free(_Dl.sdw); _Dl.sdw = NULL; } _Dl.id[0] = 0; return CF_ARM_CARD_FAIL; }
    }
    if (_Dl.sv_only) {
        /* the game is on the card as it is: nothing staged, nothing unpacked; its save alone comes */
        _Dl.have = _Dl.sv_from; _Dl.crc_want = o->save_crc32;
        _Dl.to_sd = 1; _Dl.sd_have = 1; _Dl.zcard = 0; _Dl.to_lib = 0; _Dl.to_ram = 0; _Dl.dirty = 0; _Dl.st = DL_RUN;
        snprintf(_Dl.sd_path, sizeof _Dl.sd_path, "%s", sdp);
    }
    printf("cf: download of %lu bytes into %s%s\n", (unsigned long)o->size, to_sd ? _Dl.sd_path : to_lib ? "the FPGA flash's library (kept as it comes)" : to_ram ? "the FPGA's RAM (a game to play, too big to keep)" : "the staging area",
           _Dl.st == DL_CHECK ? " (checking what is there first)" : "");
    return CF_ARM_WAIT;
}
/* the download's share of INSTALLING NN% (the first tenth) */
static int dl_pct(void) { return (_DlP && _Dl.st == DL_RUN && _Dl.size) ? (int)((uint64_t)_Dl.have * (_Dl.to_lib ? 95u : 10u) / _Dl.size) : -1; }   /* 1.1.2: a keep written as it comes is the whole job */
static void pt_cleanup(void);
/* the install is over (or was given up): the staged copy goes (erased, read back blank) */
static void dl_cleanup(void)
{
    if (!_DlP || dl_running() || _Cf.armed || _Cf.done_pending || _Net.on || _StageErasing) return;
    char jid[33]; uint8_t src = 0;
    if (cf_esp_stream_pending(&_Cf, jid, NULL, &src) && src == CF_SRC_WIFI_STAGED) return;   /* unfinished: kept for the resume */
    if (_Inst.st != CI_IDLE && _Inst.st != CI_RX_EXPIRED && _Inst.st != CI_RX_FAIL) return;   /* a receive / install is under way */
    if (!_Dl.dirty) {
        /* fwshrink1: a download that staged nothing (into PSRAM, the card, the library) or whose staged copy stays (the kept
           game): only its state goes (~0.8 KB). It used to stay until the next install, and SYSTEM > UPDATE said NOT NOW
           (a play download) after every Wi-Fi play */
        if (_Dl.sha_on) mbedtls_sha256_free(&_Dl.sha);
        free(_Dl.buf);
        free(_DlP); _DlP = NULL;
        return;
    }
    if (_Dl.sha_on) mbedtls_sha256_free(&_Dl.sha);
    free(_Dl.buf);
    free(_DlP); _DlP = NULL;
    pt_cleanup();
    stage_erase(NULL);
    printf("cf: download: the staged copy is being erased\n");
}

/* ---- 1.0.5 (owner 2026-10-19): a PATCH install (offer kind "patch"), Tetris PARTY BATTLE over Wi-Fi.
 * The site sends ONLY a patch bundle (cf_bps.h: every patch of one release, each pinned to its source and
 * target sha256); no ROM bytes leave or reach the unit. The unit, in the staging area of its own flash:
 *   DL     the bundle downloaded + hashed as any image (offset 0)
 *   ERASE  the source and target regions erased
 *   READ   its OWN cart read (the USB backup's read job, frames into the staging area at PT_SRC_OFF): the
 *          first source-size bytes hashed; any byte past them up to the chip's end not FF = not that cart
 *   PICK   the entry whose source sha256 is the cart's (none: WRONG CART; the cart IS a target: ALREADY)
 *   APPLY  cf_bps_apply streaming (source + patch from flash, the target to PT_TGT_OFF, 4 KiB in RAM),
 *          its sha256 must be the entry's target sha256
 *   MOVE   the target to offset 0, read back and hashed; the download state then says "this image is
 *          staged" (size / crc32 / sha256 of the TARGET) and the ordinary staged write runs, journalled,
 *          read back against the target's crc32 + sha256, the save RAM KEPT (ram_clear 0)
 * A restart after the write began: NVS "cf"/"ptgt" names the target of this install id, so the resume
 * checks the staged target and goes on (the cart no longer holds the source by then). */
#include "cf_bps.h"
#define PT_TGT_OFF 0x080000u               /* staging offsets (from CF_STAGE_ROM_OFF) */
#define PT_SRC_OFF 0x100000u
#define PT_SIDE_MAX 0x080000u              /* source and target: at most 512 KiB each */
enum { PT_IDLE, PT_DL, PT_ERASE, PT_READ, PT_APPLY, PT_MOVE, PT_READY, PT_ERR };
typedef struct {
    volatile int st, task_rc;
    volatile int task_busy;
    char id[33];
    int sink, n, pick, err;
    uint32_t src_len, tgt_max, next, t0;
    int tail_dirty, frames;
    cf_sha256_t sha;
    uint8_t src_sha[32];
    cf_pb_entry_t e;                       /* the picked entry */
    uint32_t tgt_size, tgt_crc;
    uint8_t tgt_sha[32];
    uint32_t bcrc[8];
    const char *why;
} pt_t;
static pt_t *_Pt;
typedef struct { char id[33]; uint32_t size, crc; uint8_t sha[32]; } pt_rec_t;
static void pt_rec_save(const pt_rec_t *r)
{
    nvs_handle_t h;
    if (nvs_open("cf", NVS_READWRITE, &h) != ESP_OK) return;
    if (r) (void)nvs_set_blob(h, "ptgt", r, sizeof *r); else (void)nvs_erase_key(h, "ptgt");
    (void)nvs_commit(h);
    nvs_close(h);
}
static int pt_rec_load(pt_rec_t *r)
{
    nvs_handle_t h;
    size_t n = sizeof *r;
    if (nvs_open("cf", NVS_READONLY, &h) != ESP_OK) return -1;
    const esp_err_t e = nvs_get_blob(h, "ptgt", r, &n);
    nvs_close(h);
    return e == ESP_OK && n == sizeof *r ? 0 : -1;
}
/* the read job's frames (host_frame while _Pt->sink): ROM data into the staging area, in order */
static void pt_ff_hash(uint32_t upto)
{
    static const uint8_t ff[64] = { [0 ... 63] = 0xFF };
    while (_Pt->next < upto) { const uint32_t k = upto - _Pt->next < 64u ? upto - _Pt->next : 64u; cf_sha256_update(&_Pt->sha, ff, k); _Pt->next += k; }
}
static void pt_sink(const uint8_t *f, uint32_t n)
{
    if (n >= 15 && f[0] == 0xA5 && f[1] == 'C' && f[2] == 'B' && f[3] == 'U' && f[4] == 'R') {
        const uint32_t off = (uint32_t)f[5] | (uint32_t)f[6] << 8 | (uint32_t)f[7] << 16 | (uint32_t)f[8] << 24;
        const uint32_t len = (uint32_t)f[9] | (uint32_t)f[10] << 8;
        _Pt->frames++;
        if (off + len <= _Pt->src_len && off >= _Pt->next) {
            pt_ff_hash(off);
            cf_sha256_update(&_Pt->sha, f + 11, len);
            if (stage_write(CF_STAGE_ROM_OFF + PT_SRC_OFF + off, f + 11, len)) _Pt->err = 1;
            _Pt->next = off + len;
        } else if (off >= _Pt->src_len) {
            /* data past the source: a bank that is not blank. A cart that only HAS FF there sends a frame of FF
               when its bank was not skipped (a bad blank read): those bytes are looked at */
            for (uint32_t i = 0; i < len; i++) if (f[11 + i] != 0xFF) { _Pt->tail_dirty = 1; break; }
        } else _Pt->err = 1;   /* out of order / straddling: not expected from the read job */
        return;
    }
    if (n > 10 && !memcmp(f, "CBU1 blank", 10)) return;     /* (112 of them on a Tetris cart: not echoed) */
    if (n > 10 && !memcmp(f, "CBU1 dirty", 10)) _Pt->tail_dirty = 1;
    printf("%.*s", (int)n, (const char *)f);
}
static int pt_src_read(void *u, uint32_t o, uint8_t *b, uint32_t n) { (void)u; return stage_read(NULL, 0, PT_SRC_OFF + o, b, n); }
static int pt_pat_read(void *u, uint32_t o, uint8_t *b, uint32_t n) { (void)u; return stage_read(NULL, 0, _Pt->e.off + o, b, n); }
static int pt_tgt_read(void *u, uint32_t o, uint8_t *b, uint32_t n) { (void)u; return stage_read(NULL, 0, PT_TGT_OFF + o, b, n); }
static int pt_tgt_write(void *u, uint32_t o, const uint8_t *b, uint32_t n) { (void)u; vTaskDelay(1); return stage_write(CF_STAGE_ROM_OFF + PT_TGT_OFF + o, b, n); }
static esp_err_t pt_erase(uint32_t off, uint32_t n)
{
    esp_flash_t *ch = stage_chip();
    if (!ch) return ESP_FAIL;
    for (uint32_t a = off; a < off + n; a += DL_BLK) { const esp_err_t e = esp_flash_erase_region(ch, CF_STAGE_ROM_OFF + a, DL_BLK); if (e != ESP_OK) return e; vTaskDelay(1); }
    return ESP_OK;
}
static uint32_t pt_r64(uint32_t n) { return (n + DL_BLK - 1u) & ~(DL_BLK - 1u); }
/* the worker (erase / apply / move): the flash work is long for the nw task */
static void pt_task(void *arg)
{
    const int what = (int)(intptr_t)arg;
    int rc = 0;
    if (what == PT_ERASE) {
        rc = pt_erase(PT_TGT_OFF, pt_r64(_Pt->tgt_max)) != ESP_OK || pt_erase(PT_SRC_OFF, pt_r64(_Pt->src_len)) != ESP_OK;
    } else if (what == PT_APPLY) {
        uint8_t *work = malloc(CF_BPS_WORK);
        if (!work) rc = 1;
        else {
            const cf_bps_io_t io = { NULL, pt_src_read, pt_pat_read, pt_tgt_read, pt_tgt_write };
            uint32_t tn = 0, tc = 0; uint8_t ts[32];
            const uint32_t t0 = now_ms();
            const int r = cf_bps_apply(&io, _Pt->e.len, _Pt->e.src_size, _Pt->tgt_max, work, &tn, &tc, ts, &_Pt->why);
            free(work);
            printf("cf: patch: applied in %lu ms: %s (%lu bytes, crc32 %08lx; lowest free heap %lu)\n", (unsigned long)(now_ms() - t0), _Pt->why,
                   (unsigned long)tn, (unsigned long)tc, (unsigned long)esp_get_minimum_free_heap_size());
            if (r) rc = r == CF_BPS_E_IO ? 9 : 2;
            else if (tn != _Pt->e.tgt_size || memcmp(ts, _Pt->e.tgt_sha, 32)) { _Pt->why = "the result is not the pinned target"; rc = 2; }
            else { _Pt->tgt_size = tn; _Pt->tgt_crc = tc; memcpy(_Pt->tgt_sha, ts, 32); }
        }
    } else if (what == PT_MOVE) {
        /* the target to offset 0 (over the bundle), then read back from there and hashed */
        uint8_t *b = malloc(2048);
        rc = !b || pt_erase(0, pt_r64(_Pt->tgt_size)) != ESP_OK;
        for (uint32_t o = 0; !rc && o < _Pt->tgt_size; o += 2048) {
            const uint32_t k = _Pt->tgt_size - o < 2048u ? _Pt->tgt_size - o : 2048u;
            rc = stage_read(NULL, 0, PT_TGT_OFF + o, b, k) || stage_write(CF_STAGE_ROM_OFF + o, b, k);
            if ((o & 0x7FFFu) == 0) vTaskDelay(1);
        }
        if (!rc) {
            cf_sha256_t s; uint32_t crc = 0; uint8_t d[32];
            cf_sha256_init(&s); memset(_Pt->bcrc, 0, sizeof _Pt->bcrc);
            for (uint32_t o = 0; !rc && o < _Pt->tgt_size; o += 2048) {
                const uint32_t k = _Pt->tgt_size - o < 2048u ? _Pt->tgt_size - o : 2048u;
                rc = stage_read(NULL, 0, o, b, k);
                cf_sha256_update(&s, b, k); crc = cf_crc32(crc, b, k);
                if (o < 0x80000u) _Pt->bcrc[o >> 16] = cf_crc32(_Pt->bcrc[o >> 16], b, k);
            }
            cf_sha256_final(&s, d);
            if (!rc && (crc != _Pt->tgt_crc || memcmp(d, _Pt->tgt_sha, 32))) { _Pt->why = "the moved copy does not read back"; rc = 9; }
        }
        free(b);
    }
    _Pt->task_rc = rc;
    _Pt->task_busy = 0;
    vTaskDelete(NULL);
}
static int pt_spawn(int what)
{
    _Pt->task_busy = 1; _Pt->task_rc = 0;
    if (xTaskCreate(pt_task, "cf_pt", 3072, (void *)(intptr_t)what, 4, NULL) != pdPASS) { _Pt->task_busy = 0; return -1; }
    return 0;
}
static void pt_hex(const uint8_t *d, char *o) { for (int i = 0; i < 32; i++) sprintf(o + 2 * i, "%02x", d[i]); }
static void pt_end(void) { if (_Pt) { _Pt->sink = 0; free(_Pt); _Pt = NULL; } }
/* arm() for a patch offer: CF_ARM_WAIT while it works; 0 with *t = the TARGET as an offer (staged, _Dl set) */
static int pt_arm(const cf_offer_t *o, cf_offer_t *t)
{
    if (_Pt && strcmp(_Pt->id, o->id)) pt_end();
    if (!_Pt) {
        if (!(_Pt = calloc(1, sizeof *_Pt))) return CF_ARM_WAIT;
        snprintf(_Pt->id, sizeof _Pt->id, "%s", o->id);
        _Pt->st = PT_DL;
        char jid[33]; uint8_t src = 0; pt_rec_t rec;
        if (cf_esp_stream_pending(&_Cf, jid, NULL, &src) && src == CF_SRC_WIFI_STAGED && !strcmp(jid, o->id) && !pt_rec_load(&rec) && !strcmp(rec.id, o->id)) {
            /* the write of this patch's target began before a restart: the target is what is staged */
            _Pt->tgt_size = rec.size; _Pt->tgt_crc = rec.crc; memcpy(_Pt->tgt_sha, rec.sha, 32);
            _Pt->st = PT_READY;
            printf("cf: patch: resuming the write of its result (%lu bytes, crc32 %08lx)\n", (unsigned long)rec.size, (unsigned long)rec.crc);
            goto ready_resume;
        }
        printf("cf: patch: a patch bundle of %lu bytes: download, read this cart, apply here\n", (unsigned long)o->size);
    }
    if (_Pt->task_busy) return CF_ARM_WAIT;
    switch (_Pt->st) {
    case PT_DL: {
        const int d = dl_for(o);
        if (d) return d;
        uint8_t h[CF_PB_HDR], raw[CF_PB_ENTRY];
        if (stage_read(NULL, 0, 0, h, sizeof h) || (_Pt->n = cf_pb_count(h, o->size)) < 1) { printf("cf: patch: not a patch bundle\n"); pt_end(); return CF_ARM_PATCH_BAD; }
        _Pt->src_len = 0; _Pt->tgt_max = 0;
        for (int i = 0; i < _Pt->n; i++) {
            cf_pb_entry_t e;
            if (stage_read(NULL, 0, CF_PB_HDR + (uint32_t)i * CF_PB_ENTRY, raw, sizeof raw) || cf_pb_entry(raw, o->size, &e) ||
                e.src_size > PT_SIDE_MAX || e.tgt_size > PT_SIDE_MAX) { printf("cf: patch: entry %d of the bundle does not check out\n", i); pt_end(); return CF_ARM_PATCH_BAD; }
            if (e.src_size > _Pt->src_len) _Pt->src_len = e.src_size;
            if (e.tgt_size > _Pt->tgt_max) _Pt->tgt_max = e.tgt_size;
        }
        if (pt_spawn(PT_ERASE)) return CF_ARM_WAIT;
        _Pt->st = PT_ERASE;
        _Pt->t0 = now_ms();
        return CF_ARM_WAIT;
    }
    case PT_ERASE: {
        if (_Pt->task_rc) { printf("cf: patch: the staging area did not erase\n"); pt_end(); return CF_D_FAILED; }
        cf_job_t j;
        memset(&j, 0, sizeof j);
        j.job = CF_JOB_USBREAD; j.rd_what = 0; j.rd_skip = 1; j.rd_off = 0; j.rd_len = 0; j.max_retries = 2;
        j.rd_dirty_from = _Pt->src_len;     /* a bank past the source that is not blank ends the read at once (not the cart) */
        cf_sha256_init(&_Pt->sha); _Pt->next = 0; _Pt->tail_dirty = 0; _Pt->err = 0; _Pt->frames = 0;
        _Pt->sink = 1;
        _Cf.st.result = 0xEE;
        const int r = cf_esp_arm(&_Cf, &j);
        if (r) { _Pt->sink = 0; printf("cf: patch: the cart read was refused (%d)\n", r); if (r == CF_D_ABORTED) return CF_ARM_WAIT; pt_end(); return r; }
        printf("cf: patch: reading this cart (%lu bytes hashed, the rest must be FF)\n", (unsigned long)_Pt->src_len);
        _Pt->st = PT_READ;
        return CF_ARM_WAIT;
    }
    case PT_READ: {
        if (_Cf.armed || _Cf.done_pending) return CF_ARM_WAIT;
        _Pt->sink = 0;
        if (_Cf.st.result != CF_D_OK || _Pt->err) { printf("cf: patch: the cart read failed (result %u%s)\n", _Cf.st.result, _Pt->err ? ", staging write" : ""); pt_end(); return CF_D_FAILED; }
        pt_ff_hash(_Pt->src_len);
        cf_sha256_final(&_Pt->sha, _Pt->src_sha);
        char hx[65]; pt_hex(_Pt->src_sha, hx);
        printf("cf: patch: the cart read in %lu ms (%d frames): first %lu bytes sha256 %.16s, %s past them\n", (unsigned long)(now_ms() - _Pt->t0), _Pt->frames,
               (unsigned long)_Pt->src_len, hx, _Pt->tail_dirty ? "NOT blank" : "blank");
        _Pt->pick = -1;
        int already = 0;
        for (int i = 0; i < _Pt->n; i++) {
            uint8_t raw[CF_PB_ENTRY]; cf_pb_entry_t e;
            if (stage_read(NULL, 0, CF_PB_HDR + (uint32_t)i * CF_PB_ENTRY, raw, sizeof raw) || cf_pb_entry(raw, o->size, &e)) continue;
            if (!memcmp(e.tgt_sha, _Pt->src_sha, 32) && e.tgt_size == _Pt->src_len) already = 1;
            if (_Pt->pick < 0 && !memcmp(e.src_sha, _Pt->src_sha, 32) && e.src_size == _Pt->src_len) { _Pt->pick = i; _Pt->e = e; }
        }
        if (already && !_Pt->tail_dirty) { printf("cf: patch: this cart already holds the patched ROM: nothing written\n"); pt_end(); return CF_ARM_ALREADY; }
        if (_Pt->pick < 0 || _Pt->tail_dirty) { printf("cf: patch: this cart holds none of the ROMs the bundle is for: nothing written\n"); pt_end(); return CF_ARM_WRONG_CART; }
        printf("cf: patch: entry %d is for this cart: applying it here\n", _Pt->pick);
        if (pt_spawn(PT_APPLY)) return CF_ARM_WAIT;
        _Pt->st = PT_APPLY;
        return CF_ARM_WAIT;
    }
    case PT_APPLY:
        if (_Pt->task_rc) { printf("cf: patch: %s: nothing written\n", _Pt->why ? _Pt->why : "failed"); const int io = _Pt->task_rc == 9; pt_end(); return io ? CF_D_FAILED : CF_ARM_PATCH_BAD; }
        if (pt_spawn(PT_MOVE)) return CF_ARM_WAIT;
        _Pt->st = PT_MOVE;
        return CF_ARM_WAIT;
    case PT_MOVE: {
        if (_Pt->task_rc) { printf("cf: patch: %s\n", _Pt->why ? _Pt->why : "the staging copy failed"); pt_end(); return CF_D_FAILED; }
        pt_rec_t rec; memset(&rec, 0, sizeof rec);
        snprintf(rec.id, sizeof rec.id, "%s", o->id); rec.size = _Pt->tgt_size; rec.crc = _Pt->tgt_crc; memcpy(rec.sha, _Pt->tgt_sha, 32);
        pt_rec_save(&rec);
        /* the download state now describes the TARGET, staged and checked */
        mw_drop();
        _Dl.size = _Pt->tgt_size; _Dl.crc_want = _Pt->tgt_crc; _Dl.crc = _Pt->tgt_crc; memcpy(_Dl.sha_want, _Pt->tgt_sha, 32);
        memset(_Dl.bcrc, 0, sizeof _Dl.bcrc); memcpy(_Dl.bcrc, _Pt->bcrc, sizeof _Pt->bcrc);
        _Dl.have = _Pt->tgt_size; _Dl.epoch++; _Dl.st = DL_OK; _Dl.dirty = 1;
        char hx[65]; pt_hex(_Pt->tgt_sha, hx);
        printf("cf: patch: the patched ROM is staged: %lu bytes, sha256 %.16s (the pinned target), %lu ms since the download; writing it, the save kept\n",
               (unsigned long)_Pt->tgt_size, hx, (unsigned long)(now_ms() - _Pt->t0));
        _Pt->st = PT_READY;
    }   /* fall through */
    case PT_READY:
    ready_resume:
        *t = *o;
        t->kind = CF_KIND_ROM;
        t->size = _Pt->tgt_size; t->crc32 = _Pt->tgt_crc;
        pt_hex(_Pt->tgt_sha, t->sha256);
        return 0;
    default:
        pt_end();
        return CF_D_FAILED;
    }
}
static int pt_sinking(void) { return _Pt && _Pt->sink; }
static void pt_cleanup(void) { if (_Pt && !_Pt->task_busy) pt_end(); pt_rec_save(NULL); }

/* ---- 1.0.6 (owner 2026-10-20): BACK UP this cart to the sender's browser over Wi-Fi (offer kind "backup").
 * Nothing is written to the cart. In the staging area of this unit's flash (blank, checked, first):
 *   READ   the USB backup's read job, ROM (blank banks named, not sent: FF there already) into the ROM region,
 *          then the save RAM (32 KiB) into the save region
 *   LEN    what to send of the ROM: the header's size when the header is valid and all after it is FF, else
 *          the last byte that is not FF rounded up to 32 KiB (lib/cartusb.js romLength, the USB backup's rule)
 *   HASH   the sha256 of what goes up (ROM, then save), the website checks the whole file with it
 *   UP     POST <blob url>?at=&total=&rom=&crc=[&sha=] with AES-256-CTR ciphertext (the offer's key, counter
 *          from the stream offset), one keep-alive plain-http connection; the server holds a small window
 *          and lets a piece go when the browser has it (503 = the window is full: a moment later; 404 = the
 *          browser stopped it: the backup stops)
 * The staged copy is erased (and read back blank) when it ends, however it ends. Runs on the nw HTTP task. */
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_wifi.h"
enum { BK_PREP, BK_ERASING, BK_ROM, BK_SAV, BK_HASH, BK_UP, BK_DONE, BK_STOP, BK_READFAIL, BK_NETFAIL, BK_FAIL };
#define BK_CHUNK 4096u                         /* one read of the staging area (and of the hash) */
#define BK_PIECE 0x10000u                      /* one POST: every request costs ~1 s on a unit in receive mode (unit 1,
                                                  2026-10-20: 1 KiB and 4 KiB pieces alike), so the pieces are big; the
                                                  body is read, encrypted and sent BK_CHUNK at a time */
#define BK_STALL_MS (90u * 1000u)              /* no piece taken for this long: the upload gives up */
typedef struct {
    volatile int st;
    char id[33];
    char url[CF_BLOB_URL_MAX + 1];
    mbedtls_aes_context aes;
    uint8_t iv[16];
    int aes_on, sink, err;
    volatile int stop;
    uint32_t data_end, rom_len, total, at, hashed, t0, last_ok, next_try, backoff;
    mbedtls_sha256_context sha;
    int sha_on;
    char sha_hex[65];
    int sock;                                  /* the upload's own plain TCP connection (-1 none): TCP_NODELAY, one send per piece */
    int ps_saved;                              /* the Wi-Fi power save found at the upload's start (-1: not changed) */
    uint8_t *buf;
    unsigned n_post, n_wait, n_err, n_conn;
    uint32_t ms_open, ms_write, ms_hdr, ms_read;   /* where the upload's time goes */
} bk_t;
static bk_t *_Bk;
static int bk_sinking(void) { return _Bk && _Bk->sink; }
/* the read job's frames: 'R' ROM into the ROM region, 'S' save into the save region */
static void bk_sink(const uint8_t *f, uint32_t n)
{
    if (n >= 15 && f[0] == 0xA5 && f[1] == 'C' && f[2] == 'B' && f[3] == 'U' && (f[4] == 'R' || f[4] == 'S')) {
        const uint32_t off = (uint32_t)f[5] | (uint32_t)f[6] << 8 | (uint32_t)f[7] << 16 | (uint32_t)f[8] << 24;
        const uint32_t len = (uint32_t)f[9] | (uint32_t)f[10] << 8;
        const int sav = f[4] == 'S';
        if (len > n - 15 || off + len > (sav ? CF_RAM_SIZE : CF_STAGE_ROM_MAX)) { _Bk->err = 1; return; }
        if (stage_write((sav ? CF_STAGE_SAV_OFF : CF_STAGE_ROM_OFF) + off, f + 11, len)) _Bk->err = 1;
        if (!sav) for (uint32_t i = len; i > 0; i--) if (f[10 + i] != 0xFF) { if (off + i > _Bk->data_end) _Bk->data_end = off + i; break; }
        return;
    }
    if (n > 10 && !memcmp(f, "CBU1 blank", 10)) return;
    printf("%.*s", (int)n, (const char *)f);
}
static void bk_close(void)
{
    if (_Bk->sock >= 0) { close(_Bk->sock); _Bk->sock = -1; }
}
static int bk_read(uint32_t at, uint8_t *b, uint32_t n);
static int bk_crypt(uint32_t at, uint8_t *b, uint32_t n);
/* the blob URL's host, port and path (plain http only: the pieces are ciphertext) */
static int bk_url(const char *u, char *host, size_t hcap, char *port, size_t pcap, const char **path)
{
    if (strncmp(u, "http://", 7)) return -1;
    const char *h = u + 7, *sl = strchr(h, '/');
    if (!sl) return -1;
    const char *col = memchr(h, ':', (size_t)(sl - h));
    const size_t hn = (size_t)((col ? col : sl) - h);
    if (!hn || hn >= hcap) return -1;
    memcpy(host, h, hn); host[hn] = 0;
    if (col) { const size_t pn = (size_t)(sl - col - 1); if (!pn || pn >= pcap) return -1; memcpy(port, col + 1, pn); port[pn] = 0; }
    else snprintf(port, pcap, "80");
    *path = sl;
    return 0;
}
static int bk_connect(void)
{
    char host[96], port[8];
    const char *path;
    if (bk_url(_Bk->url, host, sizeof host, port, sizeof port, &path)) return -1;
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *res = NULL;
    if (getaddrinfo(host, port, &hints, &res) || !res) return -1;
    const int fd = socket(res->ai_family, res->ai_socktype, 0);
    if (fd < 0) { freeaddrinfo(res); return -1; }
    const struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    const int one = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);   /* a piece's last segment never waits for an ACK */
    const int wdt = CwlWdt_LongBegin();
    const int c = connect(fd, res->ai_addr, res->ai_addrlen);
    CwlWdt_LongEnd(wdt);
    freeaddrinfo(res);
    if (c) { close(fd); return -1; }
    _Bk->sock = fd;
    _Bk->n_conn++;
    return 0;
}
/* one POST on the kept connection: the HTTP status (the body's first line in ans), or -1 */
static int bk_request(const char *target, uint32_t at, uint32_t n, char *ans, size_t acap)
{
    char host[96], port[8];
    const char *path;
    if (bk_url(_Bk->url, host, sizeof host, port, sizeof port, &path)) return -1;
    if (_Bk->sock < 0 && bk_connect()) return -1;
    char hdr[CF_BLOB_URL_MAX + 320];
    const int hl = snprintf(hdr, sizeof hdr, "POST %s HTTP/1.1\r\nHost: %s%s%s\r\nUser-Agent: ShinyChromatic-cart/3\r\nContent-Type: application/octet-stream\r\n"
                                              "Content-Length: %lu\r\nConnection: keep-alive\r\n\r\n",
                            target, host, strcmp(port, "80") ? ":" : "", strcmp(port, "80") ? port : "", (unsigned long)n);
    if (hl <= 0 || hl >= (int)sizeof hdr) return -1;
    uint32_t tq = now_ms();
    /* the header and the piece go out together (two writes into one TCP send queue, no delay between) */
    if (send(_Bk->sock, hdr, (size_t)hl, MSG_MORE) != hl) { bk_close(); return -1; }
    for (uint32_t o = 0; o < n;) {
        const uint32_t k = n - o < BK_CHUNK ? n - o : BK_CHUNK;
        if (bk_read(at + o, _Bk->buf, k) || bk_crypt(at + o, _Bk->buf, k)) { bk_close(); return -2; }
        for (uint32_t q = 0; q < k;) { const int w = send(_Bk->sock, _Bk->buf + q, k - q, 0); if (w <= 0) { bk_close(); return -1; } q += (uint32_t)w; }
        o += k;
    }
    _Bk->ms_write += now_ms() - tq; tq = now_ms();
    /* the answer, read as it comes (1.0.7: headers of any length; Cloudflare's are ~970 bytes, and 1.0.6 gave up
       at 512, so every piece looked lost on prod while the server had taken it) */
    cf_http_ans_t a;
    cf_http_ans_init(&a);
    char r[256];
    int done = 0;
    while (!done) {
        const int k = recv(_Bk->sock, r, sizeof r, 0);
        if (k <= 0) { bk_close(); return -1; }
        done = cf_http_ans_feed(&a, r, (uint32_t)k);
        if (done < 0) { printf("cf: backup: an answer that is not HTTP (code %d)\n", a.code); bk_close(); return -1; }
    }
    _Bk->ms_hdr += now_ms() - tq;
    snprintf(ans, acap, "%s", a.body);
    if (a.closing) bk_close();
    return a.code;
}
static void bk_end(void)
{
    if (!_Bk) return;
    bk_close();
    if (_Bk->ps_saved >= 0) (void)esp_wifi_set_ps((wifi_ps_type_t)_Bk->ps_saved);   /* the radio's own rule again */
    if (_Bk->aes_on) mbedtls_aes_free(&_Bk->aes);
    if (_Bk->sha_on) mbedtls_sha256_free(&_Bk->sha);
    free(_Bk->buf);
    mbedtls_platform_zeroize(_Bk, sizeof *_Bk);
    free(_Bk); _Bk = NULL;
    stage_erase(NULL);                         /* the copy of the cart does not stay on the unit */
    printf("cf: backup: the staged copy is being erased\n");
}
/* byte `at` of what goes up: the ROM's first rom_len bytes, then the save */
static int bk_read(uint32_t at, uint8_t *b, uint32_t n)
{
    if (at < _Bk->rom_len) return stage_read(NULL, 0, at, b, n);
    return stage_read(NULL, 1, at - _Bk->rom_len, b, n);
}
static void bk_len(void)
{
    uint8_t h[0x50];
    uint32_t n = 0;
    if (!stage_read(NULL, 0, 0x100, h, sizeof h)) {
        uint8_t hc = 0;
        for (int i = 0x34; i < 0x4D; i++) hc = (uint8_t)(hc - h[i] - 1u);
        n = h[0x48] <= 8 ? 0x8000u << h[0x48] : 0;
        if (!(hc == h[0x4D] && n > 0 && n <= CF_STAGE_ROM_MAX && _Bk->data_end <= n)) n = 0;
    }
    if (!n) {
        n = (_Bk->data_end + 0x7FFFu) & ~0x7FFFu;
        if (n < 0x8000u) n = 0x8000u;
        if (n > CF_STAGE_ROM_MAX) n = CF_STAGE_ROM_MAX;
    }
    _Bk->rom_len = n;
    _Bk->total = n + CF_RAM_SIZE;
}
/* AES-256-CTR over bytes at..at+n of the stream, in place (counter = iv + at / 16, as cf_ctr_xor) */
static int bk_crypt(uint32_t at, uint8_t *b, uint32_t n)
{
    uint8_t nonce[16], stream[16]; size_t nc = 0;
    memcpy(nonce, _Bk->iv, 16);
    uint32_t add = at >> 4;
    for (int i = 15; i >= 0 && add; i--) { const uint32_t v = nonce[i] + (add & 0xFFu); nonce[i] = (uint8_t)v; add = (add >> 8) + (v >> 8); }
    if (at & 15u) {
        if (mbedtls_aes_crypt_ecb(&_Bk->aes, MBEDTLS_AES_ENCRYPT, nonce, stream)) return -1;
        for (int i = 15; i >= 0; i--) if (++nonce[i]) break;
        nc = at & 15u;
    }
    const int r = mbedtls_aes_crypt_ctr(&_Bk->aes, n, &nc, nonce, stream, b, b);
    mbedtls_platform_zeroize(stream, sizeof stream);
    return r ? -1 : 0;
}
/* one POST of the piece at _Bk->at: 1 = call again soon */
static int bk_post(void)
{
    const uint32_t now = now_ms();
    if ((int32_t)(now - _Bk->last_ok) >= (int32_t)BK_STALL_MS) { printf("cf: backup: no piece taken for %u s: given up\n", (unsigned)(BK_STALL_MS / 1000u)); _Bk->st = BK_NETFAIL; bk_close(); return 0; }
    if ((int32_t)(now - _Bk->next_try) < 0) return 1;
    if (!CWL_IsOnline()) { CWL_SetNetworkWanted(true); CWL_NetworkUse(false, true); goto retry; }
    const uint32_t k = _Bk->total - _Bk->at < BK_PIECE ? _Bk->total - _Bk->at : BK_PIECE;
    /* a piece never straddles the ROM / save edge (its reads are of one region) */
    const uint32_t kk = (_Bk->at < _Bk->rom_len && _Bk->at + k > _Bk->rom_len) ? _Bk->rom_len - _Bk->at : k;
    uint32_t crc = 0;
    for (uint32_t o = 0; o < kk; o += BK_CHUNK) {
        const uint32_t c = kk - o < BK_CHUNK ? kk - o : BK_CHUNK;
        if (bk_read(_Bk->at + o, _Bk->buf, c)) { _Bk->st = BK_FAIL; return 0; }
        crc = cf_crc32(crc, _Bk->buf, c);
    }
    const char *path = strchr(_Bk->url + 7, '/');
    char target[CF_BLOB_URL_MAX + 160];
    const int last = _Bk->at + kk >= _Bk->total;
    snprintf(target, sizeof target, "%s?at=%lu&total=%lu&rom=%lu&crc=%08lx%s%s", path ? path : "/", (unsigned long)_Bk->at, (unsigned long)_Bk->total,
             (unsigned long)_Bk->rom_len, (unsigned long)crc, last ? "&sha=" : "", last ? _Bk->sha_hex : "");
    if (_Bk->sock < 0 && esp_get_free_heap_size() < CF_NET_PLAIN_HEAP) goto retry;
    char ans[48] = "";
    const int code = bk_request(target, _Bk->at, kk, ans, sizeof ans);
    if (code == -2) { _Bk->st = BK_FAIL; return 0; }
    if (code < 0) goto retry;
    _Bk->n_post++;
    unsigned long nx = 0;
    if (code == 200 && sscanf(ans, "OK %lu", &nx) == 1) {
        _Bk->at = nx > _Bk->total ? _Bk->total : (uint32_t)nx;
        _Bk->last_ok = now_ms(); _Bk->backoff = 300;
        if (_Bk->at >= _Bk->total) { bk_close(); _Bk->st = BK_DONE; return 0; }
        return 1;
    }
    if (code == 409 && sscanf(ans, "AT %lu", &nx) == 1 && nx <= _Bk->total) { _Bk->at = (uint32_t)nx; return 1; }
    if (code == 503 || code == 429) { _Bk->n_wait++; _Bk->next_try = now_ms() + 400u; return 1; }   /* the window is full: the browser is behind */
    if (code == 404 || code == 410) { printf("cf: backup: the server says %d: the website stopped taking it\n", code); bk_close(); _Bk->st = BK_STOP; return 0; }
    printf("cf: backup: piece at %lu: http %d %s\n", (unsigned long)_Bk->at, code, ans);
retry:
    _Bk->n_err++;
    if (_Bk->backoff < 300) _Bk->backoff = 300;
    _Bk->next_try = now_ms() + _Bk->backoff;
    if (_Bk->backoff < 8000) _Bk->backoff *= 2;
    return 1;
}
/* the HTTP task's turn: hash in slices, then the pieces */
static int bk_work(void)
{
    if (!_Bk) return 0;
    if (_Bk->stop && (_Bk->st == BK_HASH || _Bk->st == BK_UP)) { bk_close(); _Bk->st = BK_STOP; return 0; }
    if (_Bk->st == BK_HASH) {
        if (!_Bk->buf && !(_Bk->buf = malloc(BK_CHUNK))) return 1;
        for (int i = 0; i < 8 && _Bk->hashed < _Bk->total; i++) {
            const uint32_t edge = _Bk->hashed < _Bk->rom_len ? _Bk->rom_len : _Bk->total;
            const uint32_t k = edge - _Bk->hashed < BK_CHUNK ? edge - _Bk->hashed : BK_CHUNK;
            if (bk_read(_Bk->hashed, _Bk->buf, k)) { _Bk->st = BK_FAIL; return 0; }
            (void)mbedtls_sha256_update(&_Bk->sha, _Bk->buf, k);
            _Bk->hashed += k;
        }
        if (_Bk->hashed < _Bk->total) return 1;
        uint8_t d[32];
        (void)mbedtls_sha256_finish(&_Bk->sha, d);
        for (int i = 0; i < 32; i++) sprintf(_Bk->sha_hex + 2 * i, "%02x", d[i]);
        _Bk->at = 0; _Bk->last_ok = now_ms(); _Bk->next_try = 0; _Bk->backoff = 300;
        _Bk->st = BK_UP;
        {   /* request / answer, piece after piece: in modem sleep every answer waits for the next beacon (unit 1: 1.0 s a
               piece). Power save is off for the upload only and put back as it was at its end. */
            wifi_ps_type_t ps;
            if (esp_wifi_get_ps(&ps) == ESP_OK) {
                printf("cf: backup: Wi-Fi power save %d at the upload's start%s\n", (int)ps, ps != WIFI_PS_NONE ? ": off for the upload" : "");
                if (ps != WIFI_PS_NONE && esp_wifi_set_ps(WIFI_PS_NONE) == ESP_OK) _Bk->ps_saved = (int)ps;
            }
        }
        printf("cf: backup: %lu bytes to send (ROM %lu + save %u), sha256 %.16s\n", (unsigned long)_Bk->total, (unsigned long)_Bk->rom_len, (unsigned)CF_RAM_SIZE, _Bk->sha_hex);
        return 1;
    }
    if (_Bk->st == BK_UP) return bk_post();
    return 0;
}
static void bk_stop_req(void *u) { (void)u; if (_Bk) _Bk->stop = 1; }
/* arm() for a backup offer: CF_ARM_WAIT while it works, then its end (CF_ARM_BACKED_UP / BK_*) */
static int bk_arm(const cf_offer_t *o)
{
    if (_Bk && strcmp(_Bk->id, o->id)) bk_end();
    if (!_Bk) {
        if (!(_Bk = calloc(1, sizeof *_Bk))) return CF_ARM_WAIT;
        snprintf(_Bk->id, sizeof _Bk->id, "%s", o->id);
        snprintf(_Bk->url, sizeof _Bk->url, "%s", o->blob_url);
        mbedtls_aes_init(&_Bk->aes);
        if (mbedtls_aes_setkey_enc(&_Bk->aes, o->key, 256)) { mbedtls_aes_free(&_Bk->aes); free(_Bk); _Bk = NULL; return CF_D_ABORTED; }
        _Bk->aes_on = 1;
        memcpy(_Bk->iv, o->iv, 16);
        mbedtls_sha256_init(&_Bk->sha); (void)mbedtls_sha256_starts(&_Bk->sha, 0); _Bk->sha_on = 1;
        _Bk->t0 = now_ms();
        _Bk->sock = -1; _Bk->ps_saved = -1;
        _Bk->st = BK_PREP;
        printf("cf: backup: this cart is read into the staging area, then sent to the website\n");
    }
    int r = CF_ARM_WAIT;
    switch (_Bk->st) {
    case BK_PREP:
        if (_Cf.armed || _Cf.done_pending || _StageErasing || _Net.on) return CF_ARM_WAIT;
#ifdef CONFIG_NW_CARTFLASH_WIFI
        if (_DlP) { dl_cleanup(); return CF_ARM_WAIT; }   /* the last install's staged copy goes first */
#endif
        if (_Bk->stop) { r = CF_ARM_BK_STOPPED; break; }
        stage_erase(NULL);                     /* blank, and read back blank (quick when it is already) */
        _Bk->st = BK_ERASING;
        return CF_ARM_WAIT;
    case BK_ERASING: {
        if (_StageErasing) return CF_ARM_WAIT;
        if (_StageEraseFailed) { printf("cf: backup: the staging area did not erase\n"); r = CF_D_FAILED; break; }
        if (_Bk->stop) { r = CF_ARM_BK_STOPPED; break; }
        cf_job_t j;
        memset(&j, 0, sizeof j);
        j.job = CF_JOB_USBREAD; j.rd_what = 0; j.rd_skip = 1; j.rd_off = 0; j.rd_len = CF_STAGE_ROM_MAX; j.max_retries = 2;
        _Bk->sink = 1; _Bk->err = 0; _Bk->data_end = 0;
        _Cf.st.result = 0xEE;
        const int a = cf_esp_arm(&_Cf, &j);
        if (a) { _Bk->sink = 0; printf("cf: backup: the cart read was refused (%d)\n", a); if (a == CF_D_ABORTED) return CF_ARM_WAIT; r = a; break; }
        _Bk->st = BK_ROM;
        return CF_ARM_WAIT;
    }
    case BK_ROM:
    case BK_SAV: {
        if (_Cf.armed || _Cf.done_pending) return CF_ARM_WAIT;
        _Bk->sink = 0;
        if (_Cf.st.result != CF_D_OK || _Bk->err) { printf("cf: backup: the cart read failed (result %u%s)\n", _Cf.st.result, _Bk->err ? ", staging write" : ""); r = CF_ARM_BK_READ; break; }
        if (_Bk->st == BK_ROM) {
            printf("cf: backup: the ROM read in %lu ms (data to %lu)\n", (unsigned long)(now_ms() - _Bk->t0), (unsigned long)_Bk->data_end);
            if (_Bk->stop) { r = CF_ARM_BK_STOPPED; break; }
            cf_job_t j;
            memset(&j, 0, sizeof j);
            j.job = CF_JOB_USBREAD; j.rd_what = 1; j.rd_off = 0; j.rd_len = CF_RAM_SIZE; j.max_retries = 2;
            _Bk->sink = 1;
            _Cf.st.result = 0xEE;
            const int a = cf_esp_arm(&_Cf, &j);
            if (a) { _Bk->sink = 0; if (a == CF_D_ABORTED) { _Bk->st = BK_ROM; return CF_ARM_WAIT; } r = CF_ARM_BK_READ; break; }
            _Bk->st = BK_SAV;
            return CF_ARM_WAIT;
        }
        bk_len();
        _Bk->hashed = 0;
        _Bk->st = BK_HASH;                     /* the HTTP task hashes, then sends (bk_work) */
        return CF_ARM_WAIT;
    }
    case BK_HASH:
    case BK_UP:
        return CF_ARM_WAIT;
    case BK_DONE:
        printf("cf: backup: all %lu bytes reached the website: %lu ms, %u pieces, %u waits for the browser, %u retries, %u connections (send %lu, answer %lu ms)\n",
               (unsigned long)_Bk->total, (unsigned long)(now_ms() - _Bk->t0), _Bk->n_post, _Bk->n_wait, _Bk->n_err, _Bk->n_conn,
               (unsigned long)_Bk->ms_write, (unsigned long)_Bk->ms_hdr);
        r = CF_ARM_BACKED_UP;
        break;
    case BK_STOP: r = CF_ARM_BK_STOPPED; break;
    case BK_NETFAIL: r = CF_ARM_BK_NET; break;
    case BK_READFAIL: r = CF_ARM_BK_READ; break;
    default: r = CF_D_FAILED; break;
    }
    bk_end();
    return r;
}
/* the OSD's row for a backup: reading (no counter of its own), then the share sent */
static void bk_note(char *out, size_t cap)
{
    if (!_Bk) { snprintf(out, cap, "STARTING"); return; }
    if (_Bk->st <= BK_SAV) { snprintf(out, cap, "READING  CART"); return; }
    if (_Bk->st == BK_HASH) { snprintf(out, cap, "GETTING  READY"); return; }
    snprintf(out, cap, "SENDING  %u%%", _Bk->total ? (unsigned)((uint64_t)_Bk->at * 100u / _Bk->total) : 0u);
}
#endif

/* cartspeed2: a clear owed from before a restart (stage_erase), 15 s after the boot unless the staging
   area is taken meanwhile (the page staging again, an unfinished install, a Wi-Fi download) */
static void clr_tick(void)
{
    if (!_ClrAt || (int32_t)(now_ms() - _ClrAt) < 0) return;
    _ClrAt = 0;
    if (_Usb.st_cmds != _ClrCmds || _Cf.armed || _StageErasing || cf_esp_stream_pending(&_Cf, NULL, NULL, NULL)
#ifdef CONFIG_NW_CARTFLASH_WIFI
        || _DlP
#endif
       ) { printf("cart: the clear owed from before the restart gives way (the staging area is in use)\n"); return; }
    printf("cart: the copy of the last install was not fully erased before the restart: erasing it now\n");
    stage_erase(NULL);
}
/* cartspeed2: the staged image for the job, still arriving / being checked (cf_host_t img_avail / img_gate) */
static uint32_t img_avail(void *u)
{
    (void)u;
#ifdef CONFIG_NW_CARTFLASH_WIFI
    if (_Cf.j.job == CF_JOB_STREAM && _Cf.j.strm_src == CF_SRC_WIFI_STAGED && _DlProg) {
        if (!_DlP || _Dl.epoch != _ArmEpoch) return 0;
        if (_Dl.st == DL_OK) return 0xFFFFFFFFu;
        return _Dl.st == DL_RUN ? _Dl.have : 0;
    }
#endif
    return 0xFFFFFFFFu;
}
static int img_gate(void *u)
{
    (void)u;
#ifdef CONFIG_NW_CARTFLASH_WIFI
    if (_Cf.j.job == CF_JOB_STREAM && _Cf.j.strm_src == CF_SRC_WIFI_STAGED && _DlProg) {
        if (!_DlP || _Dl.epoch != _ArmEpoch) return -1;
        if (_Dl.st == DL_OK) return 0;
        return _Dl.st == DL_RUN ? 1 : -1;
    }
#endif
    return 0;
}
/* cartspeed2: the crc32 of the image's bytes in [off, off + n) as the download received them (one whole 64 KiB block) */
static int img_blk_crc(void *u, uint32_t off, uint32_t n, uint32_t *crc)
{
    (void)u;
#ifdef CONFIG_NW_CARTFLASH_WIFI
    if (_Cf.j.job != CF_JOB_STREAM || _Cf.j.strm_src != CF_SRC_WIFI_STAGED || !_DlP || (off & 0xFFFFu) || n != 0x10000u || off >= 0x200000u) return 0;
    if (_DlProg && _Dl.epoch != _ArmEpoch) return 0;
    const uint32_t end = off + n < _Dl.rom_len ? off + n : _Dl.rom_len;   /* 1.0.9: the game's part of the image */
    if (!(_Dl.st == DL_OK || (_Dl.st == DL_RUN && _Dl.have >= end))) return 0;
    *crc = _Dl.bcrc[off >> 16];
    return 1;
#else
    (void)off; (void)n; (void)crc;
    return 0;
#endif
}
static int chunk_hook(void *u, uint32_t off, uint16_t n, uint8_t *dst)
{
    if (_Cf.j.job == CF_JOB_STREAM && _Cf.j.strm_src == 2) return stage_read_mm(NULL, 0, off, dst, n) ? -1 : 1;
    if (_Cf.j.job == CF_JOB_STREAM && _Cf.j.strm_src == CF_SRC_WIFI_STAGED) {
        if (img_avail(NULL) < off + n && off < CF_STAGE_ROM_MAX) return 0;     /* cartspeed2: not downloaded yet */
        return wstage_read2(off, dst, n, 1) ? -1 : 1;
    }
    (void)u;
    if (_Cf.j.job == CF_JOB_USBRAM || (_Cf.j.job == CF_JOB_STREAM && _Cf.j.strm_src == 1)) return cf_usb_chunk(&_Usb, off, n, dst);
#ifdef CONFIG_NW_CARTFLASH_WIFI
    return net_chunk(off, n, dst);
#else
    return -1;                          /* no Wi-Fi install in this build: only the USB source exists */
#endif
}

#ifdef CONFIG_NW_CARTFLASH_WIFI
/* ---- cf_install hooks */
static int ci_api_start(void *u, const char *path, const char *body, char *out, int cap)
{
    (void)u;
    if (_Net.on) return 0;             /* the image connection is still closing: a moment later */
    /* a call on the link task makes its own handshake: the game traffic's idle connection gives way */
    if (esp_get_free_heap_size() < 26000u) NW_CfCloseGateway();
    if (_HeapMapOn && strstr(path, "recv/start")) heapmap_print();   /* cartspeed2 bench: the heap the handshake gets */
    /* lane mygames1: GET GAMES > MY GAMES is the account's: its pages and its orders go with the unit's PAIRING token (the
       server knows the account from it); not paired, not asked (getgames.c says PAIR IN ONLINE FIRST) */
    if (!strncmp(path, "/link_device/my_games", 21) || (strstr(path, "/catalog/order") && strstr(body, "\"mine\":true")))
        return CwlOnline_DeviceCallStart(false, path, body, out, cap) ? 1 : 0;
    /* 1.1.12, SYSTEM > CUSTOM: the custom build is the paired account's choice for this unit: the PAIRING token too */
    if (!strcmp(path, "/link_device/fw/custom")) return CwlOnline_DeviceCallStart(false, path, body, out, cap) ? 1 : 0;
    /* wifirecv1: every call of the install machine goes with the unit's registration token */
    return CwlOnline_RecvCallStart(path, body, out, cap) ? 1 : 0;
}
static int ci_registered(void *u) { (void)u; return CwlOnline_Registered() ? 1 : 0; }
/* lane sdcard1: recv/start says what this unit takes; a mounted card adds sd1 (writable) and its summary */
static void ci_start_body(void *u, char *b, int n)
{
    (void)u;
    char js[96], lj[64];
    const char *caps = "", *lcaps = "";
    Sd_RecvSummary(js, sizeof js, &caps);
    CfRc_LibSummary(lj, sizeof lj, &lcaps);           /* lane gamecomp1: ",lib1" + {"lib":{"games","free"}} when there is a library */
    snprintf(b, (size_t)n, "{\"caps\":\"" CF_RX_CAPS "%s%s\"%s%s}", lcaps, caps, js, lj);
    /* the card's heap (its volume, the host driver, the library's file) goes back for the handshake that follows;
       a card send mounts it again when its download starts (receive mode has stopped any game playing from it) */
    if (!CfRc_SdPlaying() && Sd_Busy() == kSdOp_None) Sd_Lend();
}
static void ci_reg_forget(void *u) { (void)u; CwlOnline_RecvForget(); }
static int wifi_fpga_ok(void);
static int ci_fpga(void *u) { (void)u; return wifi_fpga_ok(); }
static int ci_wifi(void *u)
{
    (void)u;
    char ssid[36];
    CWL_GetSsidText(ssid, sizeof ssid);
    return !ssid[0] ? 0 : CWL_IsOnline() ? 2 : 1;
}
/* receive mode: the link session ends (relay seat, ESP-NOW, LAN), the room is left, the game's
   network window waits, its idle TLS connection is closed: the heap is for the device calls */
static int _CiHoldOn;   /* fwshrink1: receive mode's / the update's hold is on (ci_hold): a playing game was asked to stop */
static int _RxSettled;                       /* gamecomp1: receive mode settled once per hold (the quick report tries hold it again) */
static int _CiStopped;                       /* 1.1.1: this hold stopped a game playing from RAM */
static void ci_hold(void *u, int on)
{
    (void)u;
    _CiHoldOn = on != 0;
    const uint32_t f0 = (uint32_t)esp_get_free_heap_size();
    if (on) {
        _RxHeap0 = f0; _RxHeapHeld = 0; _RxHeapLow = f0;
        /* 1.1.0: a game playing from RAM stops, its heap back. gamecomp1: and the RAM cart's task (its stack, the save's copy)
           gone before the start call's handshake (unit 1 2026-10-22: 18.9 KB right after a staged game stopped, 21.7 KB 5 s
           later: the receive gave up with "no connection") */
        if (CfRc_Yield()) _CiStopped = 1;
        if (!_RxSettled) {
            _RxSettled = 1;
            const uint32_t f1 = (uint32_t)esp_get_free_heap_size();
            const int st = CfRc_LibSettle(5000);
            printf("cf: receive mode: the RAM cart settled%s: free heap %lu -> %lu\n", st ? "" : " (not in 5 s)", (unsigned long)f1, (unsigned long)esp_get_free_heap_size());
        }
    } else _RxSettled = 0;
    CWL_RecvHold(on != 0);
    NW_RecvHold(on != 0);
    (void)CwlOnline_Quiet(0);           /* the online task's kept client (and, at the end, receive mode's kept
                                           connection, wifimem1) goes with its next turn */
    printf("cf: receive mode %s: free heap %lu (largest block %lu)\n", on ? "on, the link stops" : "off, the link starts again", (unsigned long)f0,
           (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
}
/* 1.1.1 (owner QA 2026-10-26: RECEIVE GAME while Emerald Color played from the card: the screen went to the cartridge's game
   with the menu gone; MENU showed the code page): a game playing from RAM that the hold stopped restarted the cartridge's
   game, and that closed the menu while the ESP32 still takes it as shown (main/fpga_rx.c learns it from button packets
   only). As SHARE's menu_back: told, and the page that asked opened again over the cartridge's game. */
/* (a timer: ci_hold runs on the nw task, which must not wait here) waits until the game has stopped, then 1.5 s for the
   cartridge's game to start, then the page is asked for on the nw task (NW_*OpenMenu), the menu first taken as hidden */
static esp_timer_handle_t _ReopenT;
static volatile uint8_t _ReopenWhat, _ReopenPhase, _ReopenPolls;   /* what: 1 DOWNLOAD, 2 GET GAMES, 3 UPDATE */
static void reopen_cb(void *a)
{
    (void)a;
    if (_ReopenPhase == 0) {
        if (CfRc_Playing() && ++_ReopenPolls < 40) { (void)esp_timer_start_once(_ReopenT, 250 * 1000); return; }
        _ReopenPhase = 1;
        (void)esp_timer_start_once(_ReopenT, 1500 * 1000);
        return;
    }
    if (CfRc_Playing()) return;               /* (a new game started meanwhile, a PLAY NOW: its own screen, no menu over it) */
    extern void OSD_SetVisiblityState(const bool IsVisible);
    OSD_SetVisiblityState(false);
    if (_ReopenWhat == 2) NW_GgOpenMenu(); else if (_ReopenWhat == 3) NW_FwOpenMenu(); else NW_CfOpenMenu();
    printf("cf: a game playing from RAM stopped: the menu opened again (%u)\n", _ReopenWhat);
}
static void ci_reopen(uint8_t what)
{
    if (_ReopenT == NULL) {
        const esp_timer_create_args_t a = { .callback = reopen_cb, .name = "ci_reopen" };
        if (esp_timer_create(&a, &_ReopenT) != ESP_OK) return;
    }
    (void)esp_timer_stop(_ReopenT);
    _ReopenWhat = what; _ReopenPhase = 0; _ReopenPolls = 0;
    (void)esp_timer_start_once(_ReopenT, 250 * 1000);
}
/* receive mode's hold (DOWNLOAD, or GET GAMES's order) and the update's: the same, then its page back when a game stopped */
static void ci_hold_rx(void *u, int on)
{
    _CiStopped = 0;
    ci_hold(u, on);
    if (on && _CiStopped) ci_reopen(NW_GgActive() ? 2 : 1);
}
static void ci_hold_ota(void *u, int on)
{
    _CiStopped = 0;
    ci_hold(u, on);
    if (on && _CiStopped) ci_reopen(3);
}
static int ci_held(void *u)
{
    (void)u;
    if (!CWL_RecvHeld()) return 0;
    if (!_RxHeapHeld) { _RxHeapHeld = (uint32_t)esp_get_free_heap_size(); heap_note("receive mode: link stopped"); if (_HeapMapOn) heapmap_print(); }
    return 1;
}
static int ci_api_result(void *u) { (void)u; return CwlOnline_DeviceCallResult(); }
static const char *ci_base(void *u) { (void)u; return NW_CfGatewayBase(); }
/* The header: the image's first 512 bytes. The connection it opens STAYS for the question, the
   reports and the image (one handshake for the whole install); CfDev_Tick lets it go when the
   install's state machine is idle again. */
static volatile int _PeekSt = -1;      /* -1 none, 0 running */
static int ci_peek_start(void *u, const cf_offer_t *o)
{
    (void)u;
    if (_Cf.armed || _Cf.probe_armed) return 0;
    if (o->kind == CF_KIND_PATCH || o->kind == CF_KIND_SAVE) return 0;      /* 1.0.5: a patch bundle has no ROM header to read; 1.0.9: nor a save */
    heap_note("offer");
    if (net_start(o)) return 0;
    _PeekSt = 0;
    return 1;
}
static int ci_peek_result(void *u, char *title17, char *note24)
{
    (void)u;
    if (_PeekSt != 0) return 3;
    const int r = net_chunk(0, 512, NULL);
    if (r == 0) return -1;
    _PeekSt = -1;
    if (r < 0) { _Net.stop = 1; return 2; }
    if (cf_sgz_is(_Net.buf, 512)) {
        /* lane gamecomp1: an SGZ1 container: the game's header bytes 0x134..0x14F are at its offset 48 (no logo there: no
           compat note; a game to play has no cart to fit anyway) */
        uint8_t t[0x50];
        memset(t, 0, sizeof t);
        memcpy(t + 0x34, _Net.buf + 48, 28);
        cf_title_from_header(t, title17);
        note24[0] = 0;
    } else {
        cf_title_from_header(_Net.buf + 0x100, title17);
        snprintf(note24, 24, "%s", cf_compat_text(cf_image_compat(_Net.buf + 0x100)));
    }
    _Net.stop = 1;                     /* closed until the YES: the 'downloading' report goes over TLS meanwhile */
    return 0;
}
static int ci_power(void *u) { (void)u; return _PowerUsb || CfDev_PowerOk(); }   /* (not _PowerAnyway: BATTERY LOW stays honest) */
static void ci_power_anyway(void *u, int on) { (void)u; _PowerAnyway = on ? 1 : 0; }
/* SYSTEM > DOWNLOAD opens by itself: how an install ended (the cart's restart closed the menu),
   INSTALL NOT FINISHED (the game screen is white then) */
void NW_GgOpenMenu(void);
static void ci_open(void *u) { (void)u; if (_Inst.cat) NW_GgOpenMenu(); else NW_CfOpenMenu(); }   /* lane ui110dev: GET GAMES's own install: its page */
/* ---- 1.1.0: a "play" send. A game that fits the staging area is downloaded there (hashed as any install), kept as
 * this Chromatic's game (SYSTEM > PLAY plays it again later, its saves in the store) and played; a bigger one is
 * streamed into PSRAM (each KiB read back, the whole image hashed) and played from there until the power goes.
 * Nothing touches the cart in the slot. The job is done once the image is verified; the RAM cart starts after the
 * install's report has gone (cf_install not busy): a game playing holds heap the report's TLS call needs. */
/* lane gamecomp1: the staged SGZ1 container -> the plain game file on the card (PATH.new, committed when whole and right) */
#include "cf_zc_guard.inc"
typedef struct { sd_wr_t *w; uint32_t t_rd, t_wr; zc_guard_t g; int gave_up; } zc_t;
static int zc_rd(void *u, uint32_t off, uint8_t *b, uint32_t n) { (void)u; return mw_read(CF_STAGE_ROM_OFF + off, b, n); }
static int zc_wr(void *u, uint32_t off, const uint8_t *b, uint32_t n)
{
    zc_t *z = u;
    const int64_t t = esp_timer_get_time() / 1000;
    const int e = Sd_WriteAt(z->w, off, b, n);
    const int64_t now = esp_timer_get_time() / 1000;
    const int k = e ? ZC_OK : zc_guard(&z->g, now, now - t, Sd_Writable());   /* m110t */
    if (k) { z->gave_up = k; printf("cf: play: onto the card: given up at %lu (%s)\n", (unsigned long)off, zc_why(k)); return -1; }
    return e;
}
static int z_to_card(const cf_offer_t *o)
{
    const int64_t t0 = esp_timer_get_time();
    if (Sd_Mount() != 0 || !Sd_Writable()) return CF_ARM_NO_CARD;
    zc_t z = { .w = malloc(sizeof(sd_wr_t)) };
    if (!z.w) return CF_ARM_WAIT;
    z.g.t0_ms = t0 / 1000;
    if (Sd_WriteBegin(z.w, _Dl.sd_path, o->rom_size)) { free(z.w); return CF_ARM_CARD_FAIL; }
    cf_sgz_hdr_t h;
    const uint32_t fh = esp_get_free_heap_size(), big = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    int e = cf_sgz_unpack(zc_rd, zc_wr, &z, CF_OBJ_SIZE(o), &h, NULL);
    mw_drop();
    if (!e && (h.rom_size != o->rom_size || h.rom_crc32 != o->rom_crc32)) e = CF_SGZ_E_ROM;   /* (the offer's game_* agree with the container) */
    if (!e) e = Sd_WriteCommit(z.w) ? CF_SGZ_E_WRITE : 0;
    else if (z.w->open) Sd_WriteAbort(z.w);
    free(z.w);
    printf("cf: play: SGZ1 %lu -> %lu bytes onto the card as %s: %s (%lld ms; free heap before %lu, largest %lu)\n", (unsigned long)o->size, (unsigned long)o->rom_size,
           _Dl.sd_path, cf_sgz_why(e), (long long)((esp_timer_get_time() - t0) / 1000), (unsigned long)fh, (unsigned long)big);
    if (e == CF_SGZ_E_MEM && !z.gave_up) return CF_ARM_WAIT;
    if (e) return e == CF_SGZ_E_WRITE || z.gave_up ? CF_ARM_CARD_FAIL : CF_D_BAD_IMAGE;
    Sd_RefreshSpace();
    if (!CfRc_SdPlaying() && Sd_Busy() == kSdOp_None) Sd_Lend();   /* (the report's call next: the card's heap back) */
    return 0;
}
/* lane gamecomp1: `sd imp zpart`: a container in this ESP32's flash (the USB staging) -> the card's part file */
typedef struct { int (*put)(void *u, uint32_t off, const uint8_t *b, uint32_t n); void *u; uint32_t a; } zu_t;
static int zu_rd(void *u, uint32_t off, uint8_t *b, uint32_t n) { const zu_t *z = u; return mw_read(z->a + off, b, n); }
static int zu_wr(void *u, uint32_t off, const uint8_t *b, uint32_t n) { const zu_t *z = u; return z->put(z->u, off, b, n); }
static int sd_unpack(uint32_t addr, uint32_t len, int (*put)(void *u, uint32_t off, const uint8_t *b, uint32_t n), void *u, uint32_t *rom_size)
{
    zu_t z = { put, u, addr };
    cf_sgz_hdr_t h;
    const uint32_t fh = esp_get_free_heap_size();
    const int e = cf_sgz_unpack(zu_rd, zu_wr, &z, len, &h, NULL);
    mw_drop();
    printf("cf: SGZ1 %lu bytes at %06lx: %s (free heap before %lu)\n", (unsigned long)len, (unsigned long)addr, cf_sgz_why(e), (unsigned long)fh);
    if (!e && rom_size) *rom_size = h.rom_size;
    return e;
}
static volatile int _PlayArmed;
static volatile int _PlayStart;                      /* 1 the kept game, 2 the one in PSRAM: started by CfDev_Tick */
static struct { uint32_t size, crc32; char title[17]; int save; } _PlayOffer;   /* (size, crc32, title of a PSRAM one; lane mygames1: its save in the save RAM) */
/* gamecomp1: the report's TLS handshake needs ~21 KB: the RAM cart's task and buffers (the library closed, the window buffer, an
   idle card) gone first. Unit 1 2026-10-22: 9 KB right after a library keep, 20.8 KB after a staged one with the window
   buffer still held (164 bytes short of the gate for a minute) */
static void report_room(void)
{
    const uint32_t f0 = (uint32_t)esp_get_free_heap_size();
    const int st = CfRc_LibSettle(3000);
    printf("cf: play: settled for the report%s: free heap %lu -> %lu (largest %lu)\n", st ? "" : " (not in 3 s)", (unsigned long)f0,
           (unsigned long)esp_get_free_heap_size(), (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    if (_HeapMapOn) {   /* bench (`cart heapmap on`): what holds the heap the report's handshake gets */
        static const char *const t[] = { "rc", "sd", "cf_pt", "rc_watch", "fc_osd", "nw_http" };
        printf("cf: play: tasks alive at the report:");
        for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) if (xTaskGetHandle(t[i])) printf(" %s", t[i]);
        printf("\n");
        heapmap_print();
    }
}
/* m110t: a library refusal as the install's arm says it (each its own line on the screen) */
static int lib_why(int le) { return le == CF_LIB_E_FULL || le == CF_LIB_E_MANY ? CF_ARM_LIB_FULL : le == CF_LIB_E_ARG ? CF_ARM_CANT_PLAY : CF_ARM_LIB_FAIL; }
static int play_arm(const cf_offer_t *o)
{
    if (!fpga_can_play() && o->sd != CF_SD_KEEP) return CF_D_NO_ENGINE;     /* (the RAM cart is FPGA 18.30's; a game only banked on the card needs none) */
    if (_PlayArmed) return 0;                        /* (asked again while it loads) */
    const int d = dl_for(o);
    if (d) return d;
    if (_Dl.to_lib) {
        /* lane gamecomp1: the container is in the library's sectors and hashed (crc32 + sha256 as it came): the library's end
           (each extent read back by the FPGA's crc32, the game's header read, the index written) before the report */
        if (!_Dl.lib_end) {
            CfRc_SaveComesWith(o->save_size != 0);    /* lane mygames1: the end puts the save that came with it into the game */
            if (CfRc_LibStreamEnd(&_Dl.tx)) { CfRc_SaveComesWith(0); return CF_ARM_WAIT; }
            _Dl.lib_end = 1;
            return CF_ARM_WAIT;
        }
        if (CfRc_LibBusy()) return CF_ARM_WAIT;
        const int le = CfRc_LibResult();
        if (le) { printf("cf: play: the library did not keep %s (%d)\n", o->title, le); _Dl.lib_end = 0; _Dl.st = DL_IDLE; _Dl.id[0] = 0; return lib_why(le); }
        report_room();
        printf("cf: play: %s is kept in the library (uid %lu)%s\n", o->title, (unsigned long)CfRc_LibUid(), o->lib == CF_LIB_KEEP ? ", not started (lib keep)" : ": it plays once the report is sent");
        /* 1.1.2 (owner QA 2026-10-26: Emerald Color kept, the screen dark until switched off and on): the core was handed back
           to the cartridge at the end; that closed the menu: it is opened again on the page that asked, with the result */
        if (CfRc_TakeReleased() && o->lib == CF_LIB_KEEP) ci_reopen(NW_GgActive() ? 2 : 1);
        _PlayOffer.size = CfRc_LibUid();
        _PlayStart = o->lib == CF_LIB_KEEP ? 0 : 7;   /* lane ui110dev: "keep" = kept, not started */
        _PlayArmed = 1;
        return 0;
    }
    if (_Dl.zcard) {
        /* lane gamecomp1: the container is staged and checked (crc32 + sha256): the plain game goes onto the card now,
           before the report (the report then says the truth); every block's crc32 and the game's hashes checked */
        const int ze = z_to_card(o);
        if (ze) return ze;
        _Dl.zcard = 0; _Dl.to_sd = 1; _Dl.sd_have = 1;
    }
    if (_Dl.to_sd) {
        /* lane sdcard1: banked on the card; played from it once the report is sent (sd "play"), the library refreshed */
        report_room();
        printf("cf: play: %s is on the card as %s%s\n", o->title, _Dl.sd_path, o->sd == CF_SD_PLAY ? ": it plays once the report is sent" : "");
        CfRc_SdSetPath(_Dl.sd_path);
        _PlayStart = o->sd == CF_SD_PLAY ? 3 : 4;
    } else if (_Dl.to_ram) {
        report_room();
        printf("cf: play: %s is in the FPGA's RAM (crc32 + sha256 ok): it plays once the report is sent\n", o->title);
        memset(&_PlayOffer, 0, sizeof _PlayOffer);
        _PlayOffer.size = CF_OBJ_SIZE(o); _PlayOffer.crc32 = CF_OBJ_CRC(o); snprintf(_PlayOffer.title, sizeof _PlayOffer.title, "%s", o->title);
        _PlayOffer.save = o->save_size != 0;
        _PlayStart = 2;
    } else if (o->lib == CF_LIB_KEEP) {
        /* lane ui110dev: "keep": kept in the library BEFORE the report (the report then says the truth), not started */
        if (!_Dl.lib_end) {
            CfRc_SaveComesWith(o->save_size != 0);    /* lane mygames1 */
            if (CfRc_LibKeepStaged(CF_STAGE_ROM_OFF, CF_OBJ_SIZE(o), CF_OBJ_CRC(o), o->title, 0)) { CfRc_SaveComesWith(0); return CF_ARM_WAIT; }
            _Dl.lib_end = 1;
            return CF_ARM_WAIT;
        }
        if (CfRc_LibBusy()) return CF_ARM_WAIT;
        _Dl.lib_end = 0;
        const int le = CfRc_LibResult();
        if (le) { printf("cf: play: %s was not kept (%d): nothing started\n", o->title, le); _Dl.st = DL_IDLE; _Dl.id[0] = 0; return lib_why(le); }
        report_room();
        printf("cf: play: %s is kept in the library (uid %lu), not started (lib keep)\n", o->title, (unsigned long)CfRc_LibUid());
        _PlayStart = 0;
    } else if (o->lib == CF_LIB_NONE) {
        /* lane ui110dev: "none": played from the staging area in RAM only, nothing kept */
        report_room();
        printf("cf: play: %s is staged (crc32 + sha256 ok%s): it plays once the report is sent, not kept (lib none)\n", o->title, o->z ? ", SGZ1" : "");
        memset(&_PlayOffer, 0, sizeof _PlayOffer);
        _PlayOffer.size = CF_OBJ_SIZE(o); _PlayOffer.crc32 = CF_OBJ_CRC(o); snprintf(_PlayOffer.title, sizeof _PlayOffer.title, "%s", o->title);
        _PlayOffer.save = o->save_size != 0;
        _Dl.dirty = 0;                               /* (it plays from the staged copy: erased with the next download) */
        _PlayStart = 9;
    } else {
        /* lane gamecomp1: once the report is sent the RAM cart task keeps it in the FPGA flash's library when it fits (and
           plays it from there), else plays it from here in RAM only; a unit with no library keeps it here as 1.1.0 did */
        report_room();
        printf("cf: play: %s is staged (crc32 + sha256 ok%s): it is kept and plays once the report is sent\n", o->title, o->z ? ", SGZ1" : "");
        memset(&_PlayOffer, 0, sizeof _PlayOffer);
        _PlayOffer.size = CF_OBJ_SIZE(o); _PlayOffer.crc32 = CF_OBJ_CRC(o); snprintf(_PlayOffer.title, sizeof _PlayOffer.title, "%s", o->title);
        _PlayOffer.save = o->save_size != 0;
        _Dl.dirty = 0;                               /* (the staged copy stays until the next download: the library copies it, or it is the kept game) */
        _PlayStart = 6;
    }
    _PlayArmed = 1;
    return 0;
}
/* the nw task (CfDev_Tick): a verified play send starts once the install has reported */
static void play_start_tick(void)
{
    if (!_PlayStart || (cf_install_busy(&_Inst) && !cf_install_report_only_waits(&_Inst))) return;   /* (gamecomp1: not for a report that only waits on its timer) */
    const int w = _PlayStart;
    _PlayStart = 0;
    if (w == 2) { if (_PlayOffer.save) CfRc_PlayInRamSave(_PlayOffer.size, _PlayOffer.crc32, _PlayOffer.title, 1); else CfRc_PlayInRam(_PlayOffer.size, _PlayOffer.crc32, _PlayOffer.title); }
    else if (w == 3) CfRc_PlaySd(NULL);              /* (the library refreshes when SYSTEM > GAMES opens: the heap is the game's now) */
    else if (w == 4) (void)Sd_Ask(kSdOp_Refresh, NULL);
    else if (w == 7) CfRc_LibPlay(_PlayOffer.size);   /* lane gamecomp1: (the uid) */
    else if (w == 6 || w == 9) {
        /* (6: kept and played; 9, lane ui110dev: played, nothing kept. lane mygames1: with the save that came along. Busy: a moment later) */
        CfRc_SaveComesWith(_PlayOffer.save);
        if (CfRc_LibKeepStaged(CF_STAGE_ROM_OFF, _PlayOffer.size, _PlayOffer.crc32, _PlayOffer.title, w == 6 ? 1 : 2)) { CfRc_SaveComesWith(0); _PlayStart = w; }
    }
    else CfRc_PlayKept();
}
static int ci_arm(void *u, const cf_offer_t *o)
{
    (void)u;
    /* wifispeed1: the image is downloaded into the staging area and hashed; cartspeed2: on the express
       stream the cart write starts once the first 64 KiB are staged (block by block behind the
       download; block 0 and the save RAM wait for the whole image's crc32 + sha256) */
    /* 1.0.5: a patch bundle: the unit applies it to its own cart's ROM first; the write is then of the result */
    if (o->kind == CF_KIND_BACKUP) return bk_arm(o);   /* 1.0.6: read this cart, send it to the website */
    if (o->kind == CF_KIND_PLAY) return play_arm(o);   /* 1.1.0: play it with no cartridge */
    if (o->kind == CF_KIND_SAVE && o->sd) {
        /* lane sdcard1: a save for a game on the card: downloaded (and hashed) into /SAVES/<file> */
        const int d = dl_for(o);
        if (d) return d;
        printf("cf: save: on the card as %s\n", _Dl.sd_path);
        _PlayArmed = 1;                               /* (the job's result: done) */
        return 0;
    }
    if (o->kind == CF_KIND_SAVE) {
        /* 1.0.9: a save file alone: downloaded (and hashed) into the staging save region, then written into the
           cart's save RAM (the save job: every 1 KiB written and read back by the cart writer); the game is not touched */
        if (_Part) { printf("cf: save: this layout keeps its own save store: refused\n"); return CF_D_BAD_IMAGE; }
        const int d = dl_for(o);
        if (d) return d;
        cf_job_t j;
        memset(&j, 0, sizeof j);
        j.job = CF_JOB_SAVE_PUT; j.ram_clear = 0; j.max_retries = 2;
        _Cf.st.result = 0xEE;
        printf("cf: save: 32 KiB staged (crc32 + sha256 ok): writing it into the cart's save RAM\n");
        return cf_esp_arm(&_Cf, &j);
    }
    cf_offer_t pt_t_;
    const int is_patch = o->kind == CF_KIND_PATCH;
    if (is_patch) {
        const int pr = pt_arm(o, &pt_t_);
        if (pr) return pr;
        o = &pt_t_;
    }
    const int d = dl_for(o);
    int prog = 0;
    if (d == CF_ARM_WAIT && !_XsProgOff && _DlP && _Dl.st == DL_RUN && xs_avail(NULL) &&
        _Dl.have >= (o->size < DL_PROG_MIN ? o->size : DL_PROG_MIN)) prog = 1;
    else if (d != 0) return d;
    cf_job_t j;
    memset(&j, 0, sizeof j);
    j.job = CF_JOB_STREAM; j.ram_clear = is_patch ? 0 : 1; j.max_retries = 2;   /* a patch keeps the save */
    if (o->kind == CF_KIND_ROMSAVE) j.ram_clear = 2;    /* 1.0.9: the save that came with the game (the staging save region) */
    j.strm_src = CF_SRC_WIFI_STAGED; j.strm_chunk = CF_PROG_CHUNK;
    j.fast_verify = 1;                 /* every bank by CRC16 on the Game Boy side against the hashed staged bytes (classic route) */
    j.xs_timing = _XsTiming;           /* cartspeed1: the express stream verifies every block by crc32 itself */
    j.strm_size = o->size; j.strm_crc32 = o->crc32;
    j.strm_have_sha = cf_hex32(o->sha256, j.strm_sha) == 0;
    if (o->kind == CF_KIND_ROMSAVE) {   /* 1.0.9: the cart is written with the game's part, checked against the game's own hashes */
        j.strm_size = o->rom_size; j.strm_crc32 = o->rom_crc32;
        j.strm_have_sha = cf_hex32(o->rom_sha256, j.strm_sha) == 0;
    }
    snprintf(j.strm_id, sizeof j.strm_id, "%s", o->id);
    snprintf(j.strm_title, sizeof j.strm_title, "%s", o->title);
#ifdef CONFIG_NW_CARTFLASH_GBT
    j.gb_timed = 1;
#endif
    heap_note("arm");
    {
        const uint32_t fh = esp_get_free_heap_size();
        if (fh < CF_STREAM_OPEN_HEAP) {
            if (prog) return CF_ARM_WAIT;      /* (after the download, the usual way) */
            printf("cf: stream refused: %lu bytes of free heap, needs %lu\n", (unsigned long)fh, (unsigned long)CF_STREAM_OPEN_HEAP);
            return CF_D_ABORTED;
        }
    }
    _Cf.st.result = 0xEE;
    _DlProg = prog;
    _ArmEpoch = _Dl.epoch;
    if (prog) printf("cf: download: the cart write starts at %lu of %lu bytes staged (express stream, behind the download)\n",
                     (unsigned long)_Dl.have, (unsigned long)_Dl.size);
    const int r = cf_esp_arm(&_Cf, &j);
    if (r) _DlProg = 0;
    return r;
}
static int ci_job(void *u)
{
    (void)u;
    if (_PlayArmed) { _PlayArmed = 0; return CF_D_OK; }   /* 1.1.0: a play send: verified (it starts after the report) */
    if (_Cf.armed || _Cf.done_pending) return -1;
    heap_note("job end");
    return _Cf.st.result;              /* the connection stays for the report; CfDev_Tick closes it after */
}
static int ci_pending(void *u, char *id33, char *title17)
{
    (void)u;
    uint8_t src = 0;
    return cf_esp_stream_pending(&_Cf, id33, title17, &src) && (src == 0 || src == CF_SRC_WIFI_STAGED);
}
static void ci_cancel(void *u)
{
    (void)u;
    uint8_t src = 0;
    const int staged = cf_esp_stream_pending(&_Cf, NULL, NULL, &src) && src == CF_SRC_WIFI_STAGED;
    cf_esp_stream_cancel(&_Cf);
    if (staged && !_DlP && !_StageErasing) { stage_erase(NULL); printf("cf: download: the staged copy of the given-up install is being erased\n"); }
}
static void ci_log(void *u, const char *m) { (void)u; printf("%s\n", m); }
static int ci_call_busy(void *u) { (void)u; return CwlOnline_DeviceCallBusy() ? 1 : 0; }

/* the OSD (download.c via nw_dev.c NW_CartView / NW_CartButton) */
_Static_assert((int)CV_MENU == (int)kNwCv_Menu && (int)CV_CODE == (int)kNwCv_Code && (int)CV_INSTALLING == (int)kNwCv_Installing &&
               (int)CV_UNFINISHED == (int)kNwCv_Unfinished,
               "nw_dev.h kNwCv_* are cf_install.h CV_*");
void CfDev_View(NwCartView_t *out)
{
    cf_view_t v;
    cf_install_view(&_Inst, &v);
    memset(out, 0, sizeof *out);
    out->Kind = v.kind;
    snprintf(out->Title, sizeof out->Title, "%s", v.title);
    snprintf(out->Note, sizeof out->Note, "%s", v.note);
    snprintf(out->Code, sizeof out->Code, "%s", v.code);
    out->Mins = v.mins;
    out->Anyway = v.anyway;
    out->Backup = v.backup;
    if (v.kind == CV_INSTALLING && v.backup) bk_note(out->Note, sizeof out->Note);
    else if (v.kind == CV_INSTALLING && _Pt && _Pt->st >= PT_ERASE && _Pt->st <= PT_MOVE) {
        /* 1.0.5: a patch: the unit reads its own cart, then patches it in its flash */
        snprintf(out->Note, sizeof out->Note, "%s", _Pt->st <= PT_READ ? "READING  CART" : "PATCHING");
    } else if (v.kind == CV_INSTALLING) {
        /* how far the write is (the job's own counters) */
        const int dp = dl_pct();
        if (dp >= 0 && !(_DlProg && _Cf.armed)) snprintf(out->Note, sizeof out->Note, "%d%%", dp);     /* the download: the first tenth */
        else if (_Cf.armed && _Cf.j.job == CF_JOB_STREAM && (_Cf.j.strm_src == 0 || _Cf.j.strm_src == CF_SRC_WIFI_STAGED)) {
            const uint32_t tot = _Last.total_bytes, dn = _Last.done_bytes;
            const unsigned base = (_Cf.j.strm_src == CF_SRC_WIFI_STAGED && !_DlProg) ? 10u : 0u;
            if (tot) snprintf(out->Note, sizeof out->Note, "%u%%", base + (unsigned)((uint64_t)(dn > tot ? tot : dn) * (100u - base) / tot));
            else snprintf(out->Note, sizeof out->Note, "STARTING");
        } else snprintf(out->Note, sizeof out->Note, "STARTING");
    }
}
#include "cf_ota.h"
#include "esp_app_desc.h"
static cf_ota_t *_OtaP;
void CfDev_Button(int a) { if (!(a == 1 && _OtaP && cf_ota_busy(_OtaP))) cf_install_button(&_Inst, a); }   /* (1.1.0: not while an update runs) */
/* lane ui110dev: GET GAMES (osd/system/getgames.c) over the install machine: receive mode for the catalog (asked of the nw
   task), the page's calls between the polls, and whether this receive mode is GET GAMES's */
void CfDev_GgStart(void) { if (!(_OtaP && cf_ota_busy(_OtaP))) _GgWant = 1; }
int  CfDev_GgSide(const char *path, const char *body, char *out, int cap) { return cf_install_side(&_Inst, path, body, out, cap); }
int  CfDev_GgSideDone(int *code) { return cf_install_side_done(&_Inst, code); }
int  CfDev_GgReady(void) { return cf_install_has_code(&_Inst); }
int  CfDev_GgActive(void) { return _Inst.cat; }

/* ---- 1.1.0 (owner 2026-10-20): Shiny Relay's own update over Wi-Fi (cf_ota.h). The image goes into the app
 * slot that is NOT running (the same flash the cart staging uses: never both at once), hashed as it comes and
 * checked by the IDF (esp_ota_end); A on UPDATE READY makes it the boot slot. The new app confirms itself after
 * CF_OTA_CONFIRM_MS with its FPGA link up; until then nothing is staged (no cart job could take the old slot). */
typedef struct { char ver[16]; uint32_t size; uint8_t sha[32]; uint32_t addr; uint8_t parts; } ota_rec_t;   /* parts: b0 app, b1 FPGA */
/* the FPGA part (cf_fota.h, layout rf): "fready" = the design downloaded (sha256 ok) and staged in the ESP32's app
   slot that is not running, at slot (its flash address); "fjob" = its install began (the full design header may be
   invalidated: the recovery design boots until it is done; every start finishes it from the staged copy);
   "fnext" = its header is written (it runs from the next power-on) */
typedef struct { char rel[16]; char ver[8]; uint32_t size, crc, slot; uint8_t sha[32]; uint8_t parts; uint32_t fpga_end; } fota_rec_t;
static volatile int _OtaPending;           /* this boot runs an update that is not confirmed yet */
static int _FotaWaits;                     /* an FPGA update waits for the power-on */
static volatile int _FjBusy, _FjPct, _FjRes = 1, _FjResume;
static uint32_t _FjRetryAt; static int _FjTries;   /* an install that stopped on a flash error: again in 30 s (5 times a start) */
static int fota_install_now(void);   /* an FPGA install runs (its %); its last result (1: none yet) */
static volatile int _FlScan = 1;           /* 1 = read it again */
static uint32_t _FlScanAt;                  /* the next try after a scan that did not read (0 = now) */
static int _FlScanTries;
static volatile int _FcReq;                /* the header commit: 1 asked, 2 done (_FcRes) */
static volatile int _FcRes;
static volatile int _FotaNote;             /* a notice for the nw task to show (cf_ota_notice): 1 / 3 */
static char _FotaNoteVer[16];
static uint32_t _OtaBootMs;
typedef struct {
    volatile int st;                       /* 0 running, OTA_DL_OK, else CF_OTA_E_* (never 1 for ok: CF_OTA_E_NET is 1) */
    const esp_partition_t *part;
    esp_ota_handle_t h;
    int began;
    uint32_t have, size, last, next_try, backoff;
    uint8_t want[32];
    char ver[16];
    char url[CF_OTA_URL_MAX + 1];
    mbedtls_sha256_context sha;
    esp_http_client_handle_t cli;
    uint8_t *buf;
    volatile int cancel;
    /* the FPGA image, first (phase 0): staged in the app slot that is not running (4 KiB blocks), then installed
       into the FPGA's boot flash from there (cf_fota_install); an app part comes after it (the same slot) */
    int phase;
    uint8_t parts;
    uint32_t fsize, fhave, fcrc, fslot, bfill;
    uint8_t fwant[32];
    char fver[8];
    char furl[CF_OTA_URL_MAX + 1];
    uint8_t *blk;
    /* fpgacomp1: where a download's time goes (printed every 64 KiB and at the end of the FPGA part) */
    uint32_t m_t0, m_open_ms, m_read_ms, m_flash_ms, m_opens, m_heapw, m_openfail, m_r0, m_reads, m_waits;
    int ps_saved, ps_done;          /* the Wi-Fi power save the download found (-1: none to put back) */
} ota_dl_t;
static ota_dl_t *_Od;
#define OTA_DL_OK 100
#define OTA_STALL_MS (3u * 60u * 1000u)
static void hex32(const uint8_t *d, char *o) { for (int i = 0; i < 32; i++) sprintf(o + 2 * i, "%02x", d[i]); }
static int unhex32(const char *s, uint8_t *o)
{
    if (strlen(s) != 64) return -1;
    for (int i = 0; i < 32; i++) { unsigned v; if (sscanf(s + 2 * i, "%2x", &v) != 1) return -1; o[i] = (uint8_t)v; }
    return 0;
}
static int ota_rec_load(ota_rec_t *r, const char *key)
{
    nvs_handle_t h; size_t n = sizeof *r;
    if (nvs_open("ota", NVS_READONLY, &h) != ESP_OK) return -1;
    const esp_err_t e = nvs_get_blob(h, key, r, &n);
    nvs_close(h);
    return e == ESP_OK && n == sizeof *r ? 0 : -1;
}
static void ota_rec_save(const ota_rec_t *r, const char *key)
{
    nvs_handle_t h;
    if (nvs_open("ota", NVS_READWRITE, &h) != ESP_OK) return;
    if (r) (void)nvs_set_blob(h, key, r, sizeof *r); else (void)nvs_erase_key(h, key);
    (void)nvs_commit(h);
    nvs_close(h);
}
static int fota_load(fota_rec_t *r, const char *key)
{
    memset(r, 0, sizeof *r);
    nvs_handle_t h; size_t n = sizeof *r;
    if (nvs_open("ota", NVS_READONLY, &h) != ESP_OK) return -1;
    const esp_err_t e = nvs_get_blob(h, key, r, &n);
    nvs_close(h);
    /* Accept pre-slot records too; fpga_end is assigned before the first write. */
    return e == ESP_OK && (n == sizeof *r || n == sizeof *r - sizeof r->fpga_end) ? 0 : -1;
}
static int fota_save(const fota_rec_t *r, const char *key)
{
    nvs_handle_t h;
    if (nvs_open("ota", NVS_READWRITE, &h) != ESP_OK) return -1;
    esp_err_t e = r ? nvs_set_blob(h, key, r, sizeof *r) : nvs_erase_key(h, key);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK ? 0 : -1;
}
static void fpga_str(char out[8]) { const unsigned fv = fpga_ver(NULL); snprintf(out, 8, "%u.%u", (fv >> 8) % 100u, (fv & 0xFFu) % 100u); }
static void ota_close(void) { if (_Od && _Od->cli) { esp_http_client_close(_Od->cli); esp_http_client_cleanup(_Od->cli); _Od->cli = NULL; } }
static void ota_end(int st)
{
    ota_close();
    if (_Od->ps_saved >= 0) { (void)esp_wifi_set_ps((wifi_ps_type_t)_Od->ps_saved); _Od->ps_saved = -1; }   /* the radio's own rule again */
    if (_Od->began && st != OTA_DL_OK) { (void)esp_ota_abort(_Od->h); _Od->began = 0; }
    mbedtls_sha256_free(&_Od->sha);
    free(_Od->buf); _Od->buf = NULL;
    free(_Od->blk); _Od->blk = NULL;
    _Od->st = st;
}
/* the GET (Range from `from`), its answer checked: 0 open, 1 try again later, else the CF_OTA_E_* to end with */
static int ota_open(const char *url, uint32_t from, uint32_t size)
{
    if (!CWL_IsOnline()) { CWL_SetNetworkWanted(true); CWL_NetworkUse(false, true); return 1; }
    if (esp_get_free_heap_size() < CF_NET_PLAIN_HEAP) { _Od->m_heapw++; return 1; }
    const uint32_t mo = now_ms();
    _Od->m_opens++;
    esp_http_client_config_t cfg = { .url = url, .timeout_ms = 10000, .buffer_size = 2048, .buffer_size_tx = 512,
                                     .keep_alive_enable = true, .method = HTTP_METHOD_GET };
    _Od->cli = esp_http_client_init(&cfg);
    if (!_Od->cli) return 1;
    char hv[40];
    snprintf(hv, sizeof hv, "bytes=%lu-", (unsigned long)from);
    (void)esp_http_client_set_header(_Od->cli, "Range", hv);
    (void)esp_http_client_set_header(_Od->cli, "User-Agent", "ShinyChromatic-fw/1");
    const int wdt = CwlWdt_LongBegin();
    const esp_err_t e = esp_http_client_open(_Od->cli, 0);
    CwlWdt_LongEnd(wdt);
    if (e != ESP_OK) { _Od->m_openfail++; _Od->m_open_ms += now_ms() - mo; ota_close(); return 1; }
    const int64_t clen = esp_http_client_fetch_headers(_Od->cli);
    _Od->m_open_ms += now_ms() - mo;
    const int code = esp_http_client_get_status_code(_Od->cli);
    if (code == 404 || code == 410 || code == 403) { printf("cf: update: the server says %d\n", code); return CF_OTA_E_GONE; }
    /* the length may be missing (a chunked answer, as through the proxy and the CDN): the bytes are counted
       here and the whole image is hashed at the end, so only a stated length that is wrong is refused */
    if (!((code == 206) || (code == 200 && from == 0)) || (clen > 0 && clen != (int64_t)(size - from))) {
        printf("cf: update: http %d, %lld bytes for %lu\n", code, (long long)clen, (unsigned long)(size - from));
        ota_close(); return 1;
    }
    printf("cf: update: %s download %s at %lu of %lu\n", _Od->phase ? "app" : "FPGA", from ? "resumed" : "started", (unsigned long)from, (unsigned long)size);
    return 0;
}
/* fpgacomp1: the download's time, by part: opens (and their ms), reads that ended a connection, read / flash ms */
static void ota_meter(const char *what, uint32_t have, uint32_t size)
{
    const uint32_t ms = now_ms() - _Od->m_t0;
    printf("cf: update: %s %lu of %lu in %lu ms (%lu B/s): opens %lu (%lu ms, %lu failed, %lu heap waits), reads %lu (%lu ms, %lu ended it), flash %lu ms, waits %lu\n",
           what, (unsigned long)have, (unsigned long)size, (unsigned long)ms, (unsigned long)(ms ? (uint64_t)have * 1000u / ms : 0),
           (unsigned long)_Od->m_opens, (unsigned long)_Od->m_open_ms, (unsigned long)_Od->m_openfail, (unsigned long)_Od->m_heapw,
           (unsigned long)_Od->m_reads, (unsigned long)_Od->m_read_ms, (unsigned long)_Od->m_r0, (unsigned long)_Od->m_flash_ms, (unsigned long)_Od->m_waits);
}
/* the FPGA image's turn: 0 = all in the slot and checked (phase 1 next), 1 = call again, -1 = ended (st set) */
static int fota_dl_work(void)
{
    if (!_Od->cli) {
        _Od->bfill = 0;                               /* a block not programmed yet comes again */
        free(_Od->blk); _Od->blk = NULL;              /* (the open needs the heap first: the block buffer after it) */
        const int r = ota_open(_Od->furl, _Od->fhave, _Od->fsize);
        if (r == 1) return 1;
        if (r) { ota_end(r); return -1; }
    }
    if (!_Od->blk && !(_Od->blk = malloc(4096))) { ota_close(); return 1; }
    if (_Od->fhave == 0) CfRc_GameGone();             /* the staging slot is where the kept game is */
    for (int k = 0; k < 8 && _Od->fhave < _Od->fsize; k++) {
        const uint32_t left = _Od->fsize - _Od->fhave;
        const uint32_t want = (left < 4096u ? left : 4096u) - _Od->bfill;
        const uint32_t mr = now_ms();
        const int r = esp_http_client_read(_Od->cli, (char *)_Od->blk + _Od->bfill, (int)want);
        _Od->m_read_ms += now_ms() - mr; _Od->m_reads++;
        if (r <= 0) { _Od->m_r0++; ota_close(); return 1; }
        _Od->bfill += (uint32_t)r;
        _Od->last = now_ms();
        if (_Od->bfill < (left < 4096u ? left : 4096u)) continue;
        /* a whole block: into the staging slot (its 64 KiB erase when it starts one) */
        const uint32_t mf = now_ms();
        if ((_Od->fhave & 0xFFFFu) == 0 && esp_partition_erase_range(_Od->part, _Od->fhave, 0x10000u) != ESP_OK) { ota_end(CF_OTA_E_FLASH); return -1; }
        if (esp_partition_write(_Od->part, _Od->fhave, _Od->blk, _Od->bfill) != ESP_OK) { printf("cf: update: the staging slot did not take the bytes at %lu\n", (unsigned long)_Od->fhave); ota_end(CF_OTA_E_FLASH); return -1; }
        (void)mbedtls_sha256_update(&_Od->sha, _Od->blk, _Od->bfill);
        _Od->fcrc = cf_fota_crc32(_Od->fcrc, _Od->blk, _Od->bfill);
        _Od->fhave += _Od->bfill;
        _Od->bfill = 0;
        _Od->backoff = 500;
        _Od->m_flash_ms += now_ms() - mf;
        if ((_Od->fhave & 0xFFFFu) == 0 || _Od->fhave == _Od->fsize) ota_meter("FPGA", _Od->fhave, _Od->fsize);
    }
    if (_Od->fhave < _Od->fsize) return 1;
    ota_close();
    uint8_t d[32];
    (void)mbedtls_sha256_finish(&_Od->sha, d);
    if (memcmp(d, _Od->fwant, 32)) { printf("cf: update: the FPGA image does not hash right\n"); ota_end(CF_OTA_E_HASH); return -1; }
    /* read back from the staging slot (crc32), and is it an image for this chip at all */
    {
        uint32_t got = 0;
        uint8_t first[64], last[64];
        for (uint32_t o = 0; o < _Od->fsize; o += 4096u) {
            const uint32_t n = _Od->fsize - o < 4096u ? _Od->fsize - o : 4096u;
            if (esp_partition_read(_Od->part, o, _Od->blk, n) != ESP_OK) { ota_end(CF_OTA_E_FLASH); return -1; }
            got = cf_fota_crc32(got, _Od->blk, n);
        }
        (void)esp_partition_read(_Od->part, 0, first, 64);
        (void)esp_partition_read(_Od->part, _Od->fsize - 64u, last, 64);
        if (got != _Od->fcrc) { printf("cf: update: the staged FPGA image reads back crc %08lx, want %08lx\n", (unsigned long)got, (unsigned long)_Od->fcrc); ota_end(CF_OTA_E_FLASH); return -1; }
        if (cf_fota_image_ok(first, last, _Od->fsize)) { printf("cf: update: the FPGA image is not a GW5A-25 image of this project\n"); ota_end(CF_OTA_E_IMAGE); return -1; }
    }
    fota_rec_t f; memset(&f, 0, sizeof f);
    snprintf(f.rel, sizeof f.rel, "%s", _Od->ver); snprintf(f.ver, sizeof f.ver, "%s", _Od->fver);
    f.size = _Od->fsize; f.crc = _Od->fcrc; f.slot = _Od->part->address; memcpy(f.sha, _Od->fwant, 32); f.parts = _Od->parts;
    fota_save(&f, "fready");
    printf("cf: update: FPGA %s is staged in %s (%lu bytes, sha256 + read-back ok)\n", _Od->fver, _Od->part->label, (unsigned long)_Od->fsize);
    if (_Od->size) {
        /* an app comes after it into the same slot: the FPGA is installed now, from the staged copy */
        free(_Od->blk); _Od->blk = NULL;
        if (fota_install_now() != 0) { ota_end(CF_OTA_E_FLASH); return -1; }
        _Od->parts = 1;                                /* what is left to install: the app */
    }
    free(_Od->blk); _Od->blk = NULL;
    mbedtls_sha256_free(&_Od->sha);
    mbedtls_sha256_init(&_Od->sha); (void)mbedtls_sha256_starts(&_Od->sha, 0);
    _Od->phase = 1;
    return 0;
}
/* one turn on the HTTP task: 1 = call again soon */
static int ota_dl_work(void)
{
    if (!_Od || _Od->st != 0) return 0;
    if (_Od->cancel) { ota_end(CF_OTA_E_NET); return 0; }
    if (!_Od->ps_done) {
        /* fpgacomp1: in modem sleep (the link's low power rule, WIFI_PS_MAX_MODEM) the access point holds every piece of
           the answer for the next beacon: the FPGA part came at 1.6-2.9 KB/s (unit 1 2026-10-22: 769,371 bytes in 478 s;
           each 4 KiB read 0.6-2.5 s, the server's whole answer in 84 ms). Off for the download, as for a backup upload */
        wifi_ps_type_t ps;
        _Od->ps_done = 1;
        if (esp_wifi_get_ps(&ps) == ESP_OK) {
            printf("cf: update: Wi-Fi power save %d at the download's start%s\n", (int)ps, ps != WIFI_PS_NONE ? ": off for the download" : "");
            if (ps != WIFI_PS_NONE && esp_wifi_set_ps(WIFI_PS_NONE) == ESP_OK) _Od->ps_saved = (int)ps;
        }
    }
    const uint32_t now = now_ms();
    if ((int32_t)(now - _Od->last) >= (int32_t)OTA_STALL_MS) { printf("cf: update: nothing for %u s: stopped\n", (unsigned)(OTA_STALL_MS / 1000u)); ota_end(CF_OTA_E_NET); return 0; }
    if ((int32_t)(now - _Od->next_try) < 0) return 1;
    if (_Od->phase == 0) {
        const int r = fota_dl_work();
        if (r < 0) return 0;
        if (r == 1) goto retry_if_closed;
        if (!_Od->size) {                              /* an FPGA-only update */
            printf("cf: update: %s is ready (A installs it)\n", _Od->ver);
            ota_end(OTA_DL_OK);
            return 0;
        }
    }
    if (!_Od->buf && !(_Od->buf = malloc(2048))) goto retry;
    if (!_Od->began) {
        /* sequential: each 4 KiB sector is erased as it is reached (no 2 MiB erase in one go on this task) */
        CfRc_GameGone();             /* 1.1.0: the update goes where the kept game is */
        fota_save(NULL, "fready");   /* (an FPGA part staged here is installed already, or this run has none) */
        if (esp_ota_begin(_Od->part, OTA_WITH_SEQUENTIAL_WRITES, &_Od->h) != ESP_OK) { ota_end(CF_OTA_E_FLASH); return 0; }
        _Od->began = 1;
    }
    if (!_Od->cli) {
        const int r = ota_open(_Od->url, _Od->have, _Od->size);
        if (r == 1) goto retry;
        if (r) { ota_end(r); return 0; }
    }
    for (int k = 0; k < 16 && _Od->have < _Od->size; k++) {
        const uint32_t want = _Od->size - _Od->have < 2048u ? _Od->size - _Od->have : 2048u;
        uint32_t got = 0;
        while (got < want) { const int r = esp_http_client_read(_Od->cli, (char *)_Od->buf + got, (int)(want - got)); if (r <= 0) break; got += (uint32_t)r; }
        if (!got) { ota_close(); goto retry; }
        if (esp_ota_write(_Od->h, _Od->buf, got) != ESP_OK) { printf("cf: update: the slot did not take the bytes at %lu\n", (unsigned long)_Od->have); ota_end(CF_OTA_E_FLASH); return 0; }
        (void)mbedtls_sha256_update(&_Od->sha, _Od->buf, got);
        _Od->have += got;
        _Od->last = now_ms();
        _Od->backoff = 500;
    }
    if (_Od->have < _Od->size) return 1;
    {
        ota_close();
        uint8_t d[32];
        (void)mbedtls_sha256_finish(&_Od->sha, d);
        if (memcmp(d, _Od->want, 32)) { printf("cf: update: the image does not hash right\n"); ota_end(CF_OTA_E_HASH); return 0; }
        const esp_err_t e = esp_ota_end(_Od->h);          /* the IDF's own check of the image (its header, its sha256) */
        _Od->began = 0;
        if (e != ESP_OK) { printf("cf: update: the image does not check out (%s)\n", esp_err_to_name(e)); ota_end(CF_OTA_E_IMAGE); return 0; }
        ota_rec_t r; memset(&r, 0, sizeof r);
        snprintf(r.ver, sizeof r.ver, "%s", _Od->ver); r.size = _Od->size; memcpy(r.sha, _Od->want, 32); r.addr = _Od->part->address; r.parts = _Od->parts;
        ota_rec_save(&r, "ready");
        printf("cf: update: %s is in %s (%lu bytes, sha256 ok): A installs it\n", _Od->ver, _Od->part->label, (unsigned long)_Od->size);
        ota_end(OTA_DL_OK);
        return 0;
    }
retry_if_closed:
    if (_Od->cli) return 1;                            /* still open: more next turn */
retry:
    _Od->m_waits++;
    if (_Od->backoff < 500) _Od->backoff = 500;
    _Od->next_try = now_ms() + _Od->backoff;
    if (_Od->backoff < 8000) _Od->backoff *= 2;
    return 1;
}
/* what keeps the update from starting now (NULL: nothing) */
static const char *oh_busy_why(void)
{
    if (_Cf.armed) return "a cart job is armed";
    if (_Cf.done_pending) return "a cart job's end is pending";
    if (dl_running() || (_DlP && _Dl.dirty)) return "a play download";   /* (running, or its staged copy waits to be erased) */
    if (_StageErasing) return "the staging area is being erased";
    if (_Bk) return "a cart backup";
    if (_Pt) return "a cart patch";
    if (_Usb.job != 0) return "a USB cart job";
    if (cf_install_busy(&_Inst)) return "an install (receive mode or its report)";
    if (cf_esp_stream_pending(&_Cf, NULL, NULL, NULL)) return "a cart stream";
    if (_OtaPending) return "an update step";
    /* fwshrink1: the RAM cart. A load or a library job (QSPI, PSRAM, the FPGA's flash) always waits for its end. A game that
       only plays does not block the check: the update's hold (ci_hold, as receive mode's) stops it first, its save copied,
       and settles the RAM cart; once the hold is on, a game still playing (it did not stop) blocks the download and the
       install. (Was: any game counted, and one the FPGA kept playing over an ESP32 restart blocked SYSTEM > UPDATE until
       the FPGA was reloaded.) */
    if (CfRc_Loading()) return "a RAM cart load";
    if (CfRc_LibBusy()) return "a library job";
    if (CfRc_Playing() && _CiHoldOn) return "the RAM cart (the game did not stop)";
    if (CfShare_Busy()) return "a share with another Chromatic";   /* lane share1 */
    return NULL;
}
/* fwshrink1: the card lent for a load / an install / receive mode (Sd_Lend) is mounted again once none of them runs */
static void sd_takeback_tick(void)
{
    static uint32_t next;
    const uint32_t now = now_ms();
    if (!Sd_Lent() || (int32_t)(now - next) < 0) return;
    next = now + 2000u;
    if (oh_busy_why() || (_OtaP && cf_ota_busy(_OtaP)) || esp_get_free_heap_size() < 12000u) return;
    (void)Sd_TakeBack();
}
/* lane share1: a share does not start while any of these runs (an install, receive mode, a cart job, an update, a card job) */
int CfDev_ShareOthersBusy(void)
{
    const char *why = oh_busy_why();
    if (!why && _OtaP && cf_ota_busy(_OtaP)) why = "an update";
    if (!why && Sd_Busy() != kSdOp_None) why = "a card job";
    if (why) printf("share: not now: %s\n", why);
    return why != NULL;
}
static int oh_busy(void *u)
{
    (void)u;
    const char *why = oh_busy_why();
    if (why) printf("cf: update: not now: %s\n", why);
    return why != NULL;
}
static int oh_slots(void *u)
{
    (void)u;
    const esp_partition_t *run = esp_ota_get_running_partition();
    return run && (run->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 || run->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1) && esp_ota_get_next_update_partition(NULL);
}
static void flash_sha(uint32_t a, uint32_t n, char *out)
{
    mbedtls_sha256_context s; uint8_t b[256], d[32];
    mbedtls_sha256_init(&s); (void)mbedtls_sha256_starts(&s, 0);
    for (uint32_t o = 0; o < n; o += sizeof b) { if (esp_flash_read(NULL, b, a + o, sizeof b) != ESP_OK) memset(b, 0, sizeof b); (void)mbedtls_sha256_update(&s, b, n - o < sizeof b ? n - o : sizeof b); }
    (void)mbedtls_sha256_finish(&s, d); mbedtls_sha256_free(&s);
    hex32(d, out);
}
static void oh_me(void *u, char ver[16], char fpga[8], char pt[65], char bl[65], char fl[8])
{
    (void)u;
    snprintf(ver, 16, "%.15s", esp_app_get_description()->version);
    fpga_str(fpga);
    /* the FPGA boot flash: "rf" takes an FPGA over Wi-Fi, "rfrec" too (the recovery design runs: any 18.x design is
       wanted), "rffull" no header place left (USB), "legacy" anything else (USB) */
    if (!fpga_can_fota() || !_FlOk) snprintf(fl, 8, "legacy");
    else snprintf(fl, 8, "%s", _FlL.kind == CF_FL_RF ? "rf" : _FlL.kind == CF_FL_RF_REC ? (_FlL.free ? "rfrec" : "rffull") :
                               _FlL.kind == CF_FL_RF_FULL ? "rffull" : "legacy");
    flash_sha(0x8000u, 0xC00u, pt);       /* the partition table as written (3 KiB) */
    flash_sha(0x1000u, 0x7000u, bl);      /* the boot loader's 28 KiB (the image + the FF after it) */
}
static int oh_dl_start(void *u, const cf_ota_offer_t *o)
{
    (void)u;
    if (_Od && _Od->st == 0) return -1;
    if (!_Od && !(_Od = calloc(1, sizeof *_Od))) return -1;
    memset(_Od, 0, sizeof *_Od);
    _Od->part = esp_ota_get_next_update_partition(NULL);
    if (!_Od->part || o->size > _Od->part->size || (o->size && unhex32(o->sha256, _Od->want))) { free(_Od); _Od = NULL; return -1; }
    _Od->parts = (uint8_t)((o->size ? 1 : 0) | (o->fsize ? 2 : 0));
    if (o->fsize) {
        if (!fpga_can_fota() || !_FlOk || (_FlL.kind != CF_FL_RF && _FlL.kind != CF_FL_RF_REC) || _FlL.free <= 0 ||
            unhex32(o->fsha256, _Od->fwant) || o->fsize > CF_FOTA_FULL_MAX || o->fsize < CF_FOTA_IMG_MIN || o->fsize > _Od->part->size) {
            printf("cf: update: this FPGA boot flash cannot take the FPGA image\n"); free(_Od); _Od = NULL; return -1; }
        _Od->fsize = o->fsize;
        snprintf(_Od->furl, sizeof _Od->furl, "%s", o->furl);
        snprintf(_Od->fver, sizeof _Od->fver, "%s", o->fversion);
    }
    _Od->phase = o->fsize ? 0 : 1;
    ota_rec_save(NULL, "ready");          /* the slots are about to change */
    fota_save(NULL, "fready");
    snprintf(_Od->url, sizeof _Od->url, "%s", o->url);
    snprintf(_Od->ver, sizeof _Od->ver, "%s", o->version);
    _Od->size = o->size;
    mbedtls_sha256_init(&_Od->sha); (void)mbedtls_sha256_starts(&_Od->sha, 0);
    _Od->last = now_ms(); _Od->backoff = 500; _Od->m_t0 = now_ms(); _Od->ps_saved = -1;
    printf("cf: update: %s into %s at %06lx\n", o->version, _Od->part->label, (unsigned long)_Od->part->address);
    return 0;
}
static int oh_dl_result(void *u, int *pct)
{
    (void)u;
    if (!_Od) return CF_OTA_E_NET;
    if (_Od->st == 0) {
        const uint64_t tot = (uint64_t)_Od->size + _Od->fsize, dn = (uint64_t)_Od->have + _Od->fhave;
        *pct = tot ? (int)(dn * 100u / tot) : 0;
        return -1;
    }
    const int st = _Od->st;
    free(_Od); _Od = NULL;
    return st == OTA_DL_OK ? 0 : st;
}
static void oh_dl_cancel(void *u) { (void)u; if (_Od) _Od->cancel = 1; }
/* an update in the idle slot that still hashes right (the cart staging may have taken the slot since) */
/* the FPGA part of a waiting update: still staged (its sha256 in the staging slot: a cart staging may have used it) */
static int fready_ok(fota_rec_t *f)
{
    if (fota_load(f, "fready")) return 0;
    char want[65], got[65];
    hex32(f->sha, want);
    flash_sha(f->slot, f->size, got);
    if (strcmp(want, got)) {
        printf("cf: update: the waiting FPGA image is no longer staged: forgotten\n");
        fota_save(NULL, "fready");
        return 0;
    }
    return 1;
}
static int oh_ready(void *u, cf_ota_offer_t *o)
{
    (void)u;
    ota_rec_t r;
    fota_rec_t f;
    int have_e = !ota_rec_load(&r, "ready"), have_f = 0;
    char want[65] = "";
    if (have_e) {
        const esp_partition_t *p = esp_ota_get_next_update_partition(NULL);
        if (!p || p->address != r.addr || r.size > p->size) { ota_rec_save(NULL, "ready"); have_e = 0; }
        else {
            char got[65];
            hex32(r.sha, want);
            flash_sha(p->address, r.size, got);
            if (strcmp(want, got)) { printf("cf: update: the waiting update is no longer in %s (the slot was used): forgotten\n", p->label); ota_rec_save(NULL, "ready"); have_e = 0; }
        }
    }
    if (fota_load(&f, "fready") == 0) have_f = fready_ok(&f);
    /* an update is whole or nothing: the parts it came with must all still be there */
    const uint8_t parts = have_e ? r.parts : have_f ? f.parts : 0;
    if (!parts || ((parts & 1) && !have_e) || ((parts & 2) && !have_f)) {
        if (have_e || have_f) printf("cf: update: a part of the waiting update is gone: forgotten\n");
        ota_rec_save(NULL, "ready"); fota_save(NULL, "fready");
        return 0;
    }
    memset(o, 0, sizeof *o);
    if (have_e) { snprintf(o->version, sizeof o->version, "%s", r.ver); o->size = r.size; memcpy(o->sha256, want, sizeof o->sha256); }
    if (have_f) { if (!have_e) snprintf(o->version, sizeof o->version, "%s", f.rel); snprintf(o->fversion, sizeof o->fversion, "%s", f.ver); o->fsize = f.size; hex32(f.sha, o->fsha256); }
    return 1;
}
static void oh_ready_forget(void *u) { (void)u; ota_rec_save(NULL, "ready"); fota_save(NULL, "fready"); }
static int oh_install(void *u, const cf_ota_offer_t *o)
{
    (void)u;
    const esp_partition_t *p = esp_ota_get_next_update_partition(NULL);
    if (o->size && !p) return -1;
    if (o->fsize) {
        /* the FPGA first: installed from its staged copy by the HTTP task (fota_work) */
        if (_FcReq == 0) { _FcReq = 1; return CF_OTA_PENDING; }
        if (_FcReq == 1) return CF_OTA_PENDING;
        _FcReq = 0;
        if (_FcRes) return -1;
    }
    if (!o->size) return 0;                                   /* FPGA only: no restart */
    const esp_err_t e = esp_ota_set_boot_partition(p);      /* the IDF checks the image once more */
    if (e != ESP_OK) { printf("cf: update: %s cannot be the boot slot (%s)\n", p->label, esp_err_to_name(e)); return -1; }
    ota_rec_t t; memset(&t, 0, sizeof t);
    snprintf(t.ver, sizeof t.ver, "%.15s", esp_app_get_description()->version); t.addr = esp_ota_get_running_partition()->address;
    ota_rec_save(&t, "from");             /* what to say if the new one does not start */
    ota_rec_save(NULL, "ready");
    printf("cf: update: %s is the boot slot now (%s): restarting\n", p->label, o->version);
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return -1;
}
static void oh_log(void *u, const char *m) { (void)u; printf("%s\n", m); }
/* the boot: an unconfirmed update (confirm it once it runs well), or the old one back after one that did not */
static void ota_boot(void)
{
    _OtaBootMs = now_ms();
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        _OtaPending = 1;
        cf_stage_hold = 1;
        printf("cf: update: this is a new version, not confirmed yet (it confirms itself after %u s with the FPGA link up)\n", (unsigned)(CF_OTA_CONFIRM_MS / 1000u));
    }
    ota_rec_t f;
    if (!_OtaPending && !ota_rec_load(&f, "from")) {
        if (run && run->address == f.addr) {
            printf("cf: update: the new version did not start: %s is back\n", f.ver);
            cf_ota_notice(_OtaP, 2, esp_app_get_description()->version);
            /* An FPGA that came with it stays in its slot; FPGA images run with older apps. */
        }
        ota_rec_save(NULL, "from");
    }
}
/* once the FPGA link is up after a start: an FPGA install that did not finish (a cut, a reset) is finished now from its
   staged copy (the recovery design runs meanwhile: it says so on its screen); an FPGA update that runs now is done;
   one that waits for the power-on says so */
static void fota_boot_check(void)
{
    fota_rec_t n, j;
    const int have_n = !fota_load(&n, "fnext"), have_j = !fota_load(&j, "fjob");
    char now[8];
    fpga_str(now);
    if (have_j) {
        printf("cf: update: the install of FPGA %s did not finish: finishing it now%s\n", j.ver, fpga_recovery() ? " (the recovery design runs)" : "");
        _FjResume = 1;
        return;
    }
    if (have_n && !strcmp(now, n.ver) && !fpga_recovery() &&
        CfDev_FpgaSlot() == (n.fpga_end ? cf_fota_slot_of(n.fpga_end - CF_FOTA_SECTOR) : CF_FOTA_SLOT_GB)) {
        printf("cf: update: FPGA %s runs\n", n.ver);
        fota_save(NULL, "fnext");
        if (!_OtaPending) { snprintf(_FotaNoteVer, sizeof _FotaNoteVer, "%s", n.rel); _FotaNote = 1; }
        return;
    }
    if (have_n) _FotaWaits = 1;
}
static void fj_pct(void *u, uint32_t done, uint32_t total) { (void)u; _FjPct = total ? (int)((uint64_t)done * 100u / total) : 0; }
static int fj_src(void *u, uint32_t o, uint8_t *b, uint32_t n) { return esp_flash_read(NULL, b, (uint32_t)(uintptr_t)u + o, n) == ESP_OK ? 0 : -1; }
/* the FPGA install (INSTALL, the download of an app + FPGA update, or a start that finds one unfinished): the staged
   copy into the full design's place (cf_fota_install: resumable, nothing changes if the staged copy is not whole) */
static int fota_install_now(void)
{
    fota_rec_t f;
    const int resume = !fota_load(&f, "fjob");           /* an install that began before: finish it */
    if (!resume && fota_load(&f, "fready")) return -1;
    (void)CfDev_FpgaSlot();
    if (!_FpgaSlotRead) return -1;                        /* never infer the booted slot after writing */
    /* Pre-slot jobs always wrote at the top of flash (GB); preserve their original destination. */
    if (resume && !f.fpga_end) f.fpga_end = cf_fota_slot_end(CF_FOTA_SLOT_GB);
    if (!resume) {
        cf_fota_layout_t L;
        if (cf_fota_scan(CfRc_FotaHost(), &L) || L.kind == CF_FL_LEGACY) return -1;
        const int slot = L.full_base ? cf_fota_slot_of(L.full_base) : CF_FOTA_SLOT_GB;
        f.fpga_end = cf_fota_slot_end(slot);
    }
    if (!f.fpga_end || cf_fota_slot_end(cf_fota_slot_of(f.fpga_end - CF_FOTA_SECTOR)) != f.fpga_end) return -1;
    if (fota_save(&f, "fjob")) return -1;                  /* persist the destination before invalidating headers */
    _FjBusy = 1; _FjPct = 0;
    if (CfRc_ResetIdle(8000)) printf("cf: update: the FPGA's op engine is not idle: trying anyway\n");
    const uint32_t t0 = now_ms();
    uint8_t *b4 = malloc(4096);                            /* (heap: DRAM is full) */
    const int e = b4 ? cf_fota_install(CfRc_FotaHost(), fj_src, (void *)(uintptr_t)f.slot, f.size, f.crc, cf_fota_slot_of(f.fpga_end - CF_FOTA_SECTOR), fj_pct, NULL, b4) : CF_FOTA_E_FLASH;
    free(b4);
    cf_fota_layout_t L;
    if (!cf_fota_scan(CfRc_FotaHost(), &L)) { _FlL = L; _FlOk = 1; }
    CfRc_GameAreaRescan();
    _FjBusy = 0; _FjRes = e;
    {
        /* the install log (NVS ota/fjlog: what the recovery design did, for the console of the next full design) */
        struct { uint32_t runs, ok, rec_runs, last_ms; int32_t last_e; char ver[8]; } lg;
        nvs_handle_t h; size_t n = sizeof lg;
        memset(&lg, 0, sizeof lg);
        if (nvs_open("ota", NVS_READWRITE, &h) == ESP_OK) {
            if (nvs_get_blob(h, "fjlog", &lg, &n) != ESP_OK || n != sizeof lg) memset(&lg, 0, sizeof lg);
            lg.runs++; if (!e) lg.ok++; if (fpga_recovery()) lg.rec_runs++;
            lg.last_ms = now_ms() - t0; lg.last_e = e; snprintf(lg.ver, sizeof lg.ver, "%s", f.ver);
            (void)nvs_set_blob(h, "fjlog", &lg, sizeof lg); (void)nvs_commit(h); nvs_close(h);
        }
    }
    if (e == CF_FOTA_E_SRC || e == CF_FOTA_E_SIZE || e == CF_FOTA_E_FULL || e == CF_FOTA_E_LAYOUT || e == CF_FOTA_E_GAMES) {
        /* nothing was written (the staged copy is not whole, or the flash cannot take it): a USB / a new download */
        printf("cf: update: the FPGA install cannot run (%d): forgotten\n", e);
        fota_save(NULL, "fjob"); fota_save(NULL, "fready");
        return -1;
    }
    if (e) {
        if (_FjTries < 5) { _FjRetryAt = now_ms() + 30000u; if (!_FjRetryAt) _FjRetryAt = 1; }
        printf("cf: update: the FPGA install stopped (%d): again in 30 s (and at every start)\n", e);
        return -1;
    }
    fota_save(NULL, "fjob"); fota_save(NULL, "fready");
    fota_save(&f, "fnext");                                /* it runs from the next power-on */
    _FotaWaits = 1;
    printf("cf: update: FPGA %s boots at the next power-on (%lu bytes in %lu ms; header at %03lx, %d places left)\n", f.ver,
           (unsigned long)f.size, (unsigned long)(now_ms() - t0), (unsigned long)_FlL.live_at, _FlL.free);
    return 0;
}
/* Shared by the console and the games page's worker; the booted slot stays cached. */
static int fpga_switch(int slot)
{
    fota_rec_t job;
    if (!fpga_can_fota() || !jobs_idle() || CfRc_Playing() || _FjBusy || _FjResume || _FcReq == 1 ||
        (_Od && _Od->st == 0) || !fota_load(&job, "fjob")) return CF_FOTA_E_FLASH;
    _FjBusy = 1;
    (void)CfDev_FpgaSlot();
    cf_fota_layout_t L;
    int e = cf_fota_scan(CfRc_FotaHost(), &L);
    if (e || !_FpgaSlotRead) e = CF_CORE_NOT_READY;
    if (!e) {
        uint32_t base;
        e = cf_fota_slot_base(CfRc_FotaHost(), slot, &base);
        if (e == CF_FOTA_E_FLASH) e = CF_CORE_NOT_READY;
    }
    if (!e) {
        uint8_t *b4 = malloc(4096);
        e = b4 ? cf_fota_switch(CfRc_FotaHost(), &L, slot, b4) : CF_FOTA_E_FLASH;
        free(b4);
        _FlL = L; _FlOk = 1;
        if (!e) _NextFpgaSlot = slot;
    }
    _FjBusy = 0;
    return e;
}
/* the HTTP task's FPGA boot-flash work (CfDev_HttpWork): 1 = call again soon */
static int fota_work(void)
{
    static int booted;
    if (!fpga_can_fota() || !wifi_fpga_ok() || CfRc_Playing() || _FjBusy) return 0;
    if (_FlScan && !(_Od && _Od->st == 0) && (!_FlScanAt || (int32_t)(now_ms() - _FlScanAt) >= 0)) {
        /* a scan that does not read (the FPGA's op engine not ready yet: at a start right after a JTAG reload / a USB
           Rescue, unit 1 2026-10-22) is retried every 300 ms in the first 10 s, then every 3 s: before, the unit stayed "not read" until the
           next start, its fw/check said "fl":"legacy" and the page said UPDATE OVER USB on an rf flash */
        cf_fota_layout_t L;
        const int e = cf_fota_scan(CfRc_FotaHost(), &L);
        if (!e) { fpga_slot_keep(&L); _FlL = L; _FlOk = 1; }
        /* Allow the extra startup attempts without shortening the later retry window. */
        if (e && ++_FlScanTries < 74) {
            _FlScanAt = now_ms() + (now_ms() < 10000u ? 300u : 3000u); if (!_FlScanAt) _FlScanAt = 1;
            if (_FlScanTries == 1) printf("cf: update: FPGA boot flash not read (%d): again every 300 ms for 10 s, then every 3 s\n", e);
        } else {
            _FlScan = 0; _FlScanAt = 0; _FlScanTries = 0;
            printf("cf: update: FPGA boot flash: %s, live header at %03lx, %d of %d full headers used\n",
                   !_FlOk ? "not read" : _FlL.kind == CF_FL_RF ? "layout rf" : _FlL.kind == CF_FL_RF_FULL ? "layout rf, full" :
                   _FlL.kind == CF_FL_RF_REC ? "layout rf, the recovery design boots" : "not the update layout",
                   (unsigned long)_FlL.live_at, _FlL.used, CF_FOTA_HDRS);
        }
    }
    /* The HTTP task otherwise sleeps for 1 s, longer than the startup retry interval. */
    if (!_FpgaSlotRead) return _FlScan && CfDev_FpgaSlotPending();
    /* FCRC uses the UART op/status round trip, unlike the header's FREAD window.
     * A readable header alone does not prove that slot scans are ready. Retry reads only. */
    static uint32_t slots_at;
    if (_FpgaSlot >= 0 && jobs_idle() && !(_Od && _Od->st == 0) && _OtherSlotState == CF_CORE_NOT_READY &&
        (!slots_at || (int32_t)(now_ms() - slots_at) >= 0)) {
        uint32_t base;
        _FjBusy = 1;
        int e = cf_fota_slot_base(CfRc_FotaHost(), 1 - _FpgaSlot, &base);
        _FjBusy = 0;
        _OtherSlotState = e == CF_FOTA_E_FLASH ? CF_CORE_NOT_READY : e;
        slots_at = now_ms() + 3000u;
    }
    if (!booted) { booted = 1; fota_boot_check(); }
    if (_FjRetryAt && (int32_t)(now_ms() - _FjRetryAt) >= 0) { _FjRetryAt = 0; _FjTries++; _FjResume = 1; }
    if (_FjResume) { _FjResume = 0; (void)fota_install_now(); }
    if (_FcReq == 1) { _FcRes = fota_install_now(); _FcReq = 2; }
    return _FcReq == 1;
}
static void ota_tick(void)
{
    if (!_OtaP) return;
    {
        /* fpgacomp1: the recovery design runs: SYSTEM > UPDATE is the page (it says what is going on) */
        static int opened;
        if (!opened && fpga_recovery() && (int32_t)(now_ms() - _OtaBootMs) >= 4000) { opened = 1; NW_OpenPage("UPDATE"); }
    }
    if (_FotaNote) { cf_ota_notice(_OtaP, _FotaNote, _FotaNoteVer); _FotaNote = 0; }
    if (_OtaPending && (int32_t)(now_ms() - _OtaBootMs) >= (int32_t)CF_OTA_CONFIRM_MS && wifi_fpga_ok()) {
#ifdef CONFIG_NW_OTA_TEST_NO_CONFIRM
        static int said;
        if (!said) { said = 1; printf("cf: update: TEST build: never confirmed; restarting in 20 s as a crashing build would\n"); }
        if ((int32_t)(now_ms() - _OtaBootMs) >= (int32_t)(CF_OTA_CONFIRM_MS + 20000u)) esp_restart();
#else
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
            _OtaPending = 0;
            cf_stage_hold = 0;
            ota_rec_save(NULL, "from");
            printf("cf: update: %s confirmed (it stays)\n", esp_app_get_description()->version);
            cf_ota_notice(_OtaP, _FotaWaits ? 3 : 1, esp_app_get_description()->version);
        }
#endif
    }
    cf_ota_tick(_OtaP, now_ms());
}
static void ota_init(void)
{
    if (!(_OtaP = calloc(1, sizeof *_OtaP))) return;
    const cf_ota_host_t h = { NULL, ci_api_start, ci_api_result, ci_registered, ci_reg_forget, ci_wifi, ci_hold_ota, ci_held, oh_busy, oh_slots, oh_me,
                              oh_dl_start, oh_dl_result, oh_dl_cancel, oh_ready, oh_ready_forget, oh_install, oh_log };
    cf_ota_init(_OtaP, &h, now_ms());
    ota_boot();
}
void CfDev_FwView(NwFwView_t *out)
{
    memset(out, 0, sizeof *out);
    if (!_OtaP) { out->Kind = -1; return; }
    cf_ota_view_t v;
    cf_ota_view(_OtaP, &v);
    out->Kind = v.kind;
    snprintf(out->Title, sizeof out->Title, "%s", v.title);
    snprintf(out->Note, sizeof out->Note, "%s", v.note);
    snprintf(out->Version, sizeof out->Version, "%s", v.version);
    snprintf(out->Running, sizeof out->Running, "%.15s", esp_app_get_description()->version);
    out->Pct = v.pct;
    out->Custom = v.custom;
    snprintf(out->Name, sizeof out->Name, "%s", v.name);
    if (v.kind == OV_MENU) {
        ota_rec_t r; out->ReadyWaits = !ota_rec_load(&r, "ready");
        if (out->ReadyWaits) { snprintf(out->Version, sizeof out->Version, "%s", r.ver); out->Custom = cf_ota_is_custom(r.ver); }
    }
    /* fpgacomp1: an FPGA install that runs, or the recovery design with an install to finish / just finished */
    fota_rec_t f;
    const int rec = fpga_recovery();
    if (_FjBusy || (rec && v.kind == OV_MENU && (!fota_load(&f, "fjob") || _FjResume))) {
        out->Kind = kNwFv_Fpga;
        snprintf(out->Title, sizeof out->Title, "%s", rec ? "FINISHING  UPDATE" : "INSTALLING");
        snprintf(out->Note, sizeof out->Note, "%s", "KEEP  IT  ON|DO  NOT  TURN  IT  OFF");
        out->Pct = _FjBusy ? _FjPct : 0;
    } else if (rec && v.kind == OV_MENU && !fota_load(&f, "fnext")) {
        out->Kind = kNwFv_Fpga;
        snprintf(out->Title, sizeof out->Title, "%s", "UPDATE  DONE");
        snprintf(out->Note, sizeof out->Note, "%s", "TURN  IT  OFF  AND|ON  TO  FINISH");
        out->Pct = -1;
    }
}
void CfDev_FwButton(int a) { if (_OtaP && !(a == 1 && _OtaP->st == OT_IDLE && cf_install_busy(&_Inst))) cf_ota_button(_OtaP, a); }
void CfDev_FwCustom(void) { if (_OtaP && _OtaP->st == OT_IDLE && !cf_install_busy(&_Inst)) cf_ota_custom(_OtaP); }
#else  /* no Wi-Fi install in this build (the USB route only) */
int CfDev_HttpWork(void) { return stage_erase_step(); }
void CfDev_View(NwCartView_t *out) { memset(out, 0, sizeof *out); out->Kind = -1; }
void CfDev_Button(int a) { (void)a; }
void CfDev_GgStart(void) { }
int  CfDev_GgSide(const char *path, const char *body, char *out, int cap) { (void)path; (void)body; (void)out; (void)cap; return -1; }
int  CfDev_GgSideDone(int *code) { *code = -1; return -1; }
int  CfDev_GgReady(void) { return 0; }
int  CfDev_GgActive(void) { return 0; }
void CfDev_FwView(NwFwView_t *out) { memset(out, 0, sizeof *out); out->Kind = -1; }
void CfDev_FwButton(int a) { (void)a; }
void CfDev_FwCustom(void) { }
#endif /* CONFIG_NW_CARTFLASH_WIFI */

/* ---- the USB console protocol (cf_usb.c, docs/CART_BACKUP_USB_PROTOCOL.md) */
#ifdef CONFIG_HEAP_USE_HOOKS
/* cartspeed2 bench only (a test build with CONFIG_HEAP_USE_HOOKS): who allocates the blocks that stay in
   the top of the heap (where the boot leaves the one big free block) after an install */
#include "esp_debug_helpers.h"
#define HK_N 96
static struct { uint32_t sz, ptr, pc[8]; } _HkRec[HK_N];
static volatile uint32_t _HkN;
/* 1.0.3 bench: the heap's low point. Every live block of 256 bytes or more (pointer, size, 4 callers); at each new
   lowest free heap the whole table is copied: `cart peak` prints the copy (who held the heap at its lowest). */
#define PK_N 80
typedef struct { uint32_t ptr, sz, pc[4]; } pk_rec_t;
static pk_rec_t _PkLive[PK_N], _PkSnap[PK_N];
static volatile uint32_t _PkLow = 0xffffffffu, _PkSnapFree, _PkSnapN, _PkArm;
static portMUX_TYPE _PkMux = portMUX_INITIALIZER_UNLOCKED;
IRAM_ATTR static void pk_alloc(void *ptr, size_t size)
{
    const uint32_t a = (uint32_t)(uintptr_t)ptr;
    if (size >= 256) {
        esp_backtrace_frame_t f;
        esp_backtrace_get_start(&f.pc, &f.sp, &f.next_pc);
        uint32_t pcs[10] = { 0 };
        for (int k = 0; k < 10; k++) { pcs[k] = f.pc; if (!f.next_pc || !esp_backtrace_get_next_frame(&f)) break; }
        portENTER_CRITICAL_SAFE(&_PkMux);
        for (int k = 0; k < PK_N; k++) if (!_PkLive[k].ptr) { _PkLive[k].ptr = a; _PkLive[k].sz = (uint32_t)size; for (int z = 0; z < 4; z++) _PkLive[k].pc[z] = pcs[3 + z]; break; }
        portEXIT_CRITICAL_SAFE(&_PkMux);
    }
    if (!_PkArm) return;
    const uint32_t fr = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    if (fr < _PkLow) {
        portENTER_CRITICAL_SAFE(&_PkMux);
        _PkLow = fr; _PkSnapFree = fr;
        uint32_t n = 0;
        for (int k = 0; k < PK_N; k++) if (_PkLive[k].ptr) _PkSnap[n++] = _PkLive[k];
        _PkSnapN = n;
        portEXIT_CRITICAL_SAFE(&_PkMux);
    }
}
IRAM_ATTR static void pk_free(void *ptr)
{
    const uint32_t a = (uint32_t)(uintptr_t)ptr;
    portENTER_CRITICAL_SAFE(&_PkMux);
    for (int k = 0; k < PK_N; k++) if (_PkLive[k].ptr == a) { _PkLive[k].ptr = 0; break; }
    portEXIT_CRITICAL_SAFE(&_PkMux);
}
void pk_print(int rearm)
{
    printf("cart peak: lowest free heap since armed %lu, %lu blocks >= 256 B live then:\n", (unsigned long)_PkSnapFree, (unsigned long)_PkSnapN);
    uint32_t tot = 0;
    for (uint32_t i = 0; i < _PkSnapN; i++) {
        tot += _PkSnap[i].sz;
        printf("  peak %lu by %08lx %08lx %08lx %08lx\n", (unsigned long)_PkSnap[i].sz, (unsigned long)_PkSnap[i].pc[0], (unsigned long)_PkSnap[i].pc[1], (unsigned long)_PkSnap[i].pc[2], (unsigned long)_PkSnap[i].pc[3]);
    }
    printf("cart peak: %lu bytes in those blocks\n", (unsigned long)tot);
    if (rearm) { _PkLow = 0xffffffffu; _PkSnapN = 0; _PkArm = 1; }
}
IRAM_ATTR void esp_heap_trace_alloc_hook(void *ptr, size_t size, uint32_t caps)
{
    (void)caps;
    pk_alloc(ptr, size);
    const uint32_t a = (uint32_t)(uintptr_t)ptr;
    if (a < 0x3fff9800u || a >= 0x40000000u) return;
    if (!(size == 12 || size == 16 || size == 20 || size == 24 || (size >= 76 && size <= 84) || size == 128 || (size >= 580 && size <= 592))) return;
    int i = -1;
    for (int k = 0; k < HK_N; k++) if (!_HkRec[k].ptr) { i = k; break; }
    if (i < 0) i = (int)(_HkN++ % HK_N);
    _HkRec[i].sz = (uint32_t)size; _HkRec[i].ptr = a;
    esp_backtrace_frame_t f;
    esp_backtrace_get_start(&f.pc, &f.sp, &f.next_pc);
    uint32_t pcs[15] = { 0 };
    for (int k = 0; k < 15; k++) { pcs[k] = f.pc; if (!f.next_pc || !esp_backtrace_get_next_frame(&f)) break; }
    for (int k = 0; k < 8; k++) _HkRec[i].pc[k] = pcs[5 + k];
}
IRAM_ATTR void esp_heap_trace_free_hook(void *ptr)
{
    pk_free(ptr);
    const uint32_t a = (uint32_t)(uintptr_t)ptr;
    if (a < 0x3fff9800u || a >= 0x40000000u) return;
    for (int k = 0; k < HK_N; k++) if (_HkRec[k].ptr == a) { _HkRec[k].ptr = 0; break; }
}
#endif
#ifdef CONFIG_SHINY_RELAY_DEBUG_ALLOW_HTTP_API   /* TEST builds only: the map's static table is RAM the release keeps for the TLS gate */
/* cartspeed2 bench (`cart heapmap`): free / largest, every free block of 1 KiB or more, and every used
   block in the heap's top region (from 0x3fff9800: where the boot leaves one free block of ~25 KiB), with the
   allocator's callers when the bench build keeps them. Collected first (nothing printed or allocated inside the
   walk: it holds the heap's lock), static (the map itself must not land in the heap it maps). */
#define HM_N 64
static struct { uint32_t n, drop; struct { uint32_t a, sz; uint8_t used; } e[HM_N]; } _Hm;
static bool hm_walk(walker_heap_into_t h, walker_block_info_t b, void *u)
{
    (void)h; (void)u;
    const uint32_t a = (uint32_t)(uintptr_t)b.ptr;
    if (b.used ? (a < 0x3fff9800u) : b.size < 1024) return true;
    if (_Hm.n >= HM_N) { _Hm.drop++; return true; }
    _Hm.e[_Hm.n].a = a; _Hm.e[_Hm.n].sz = (uint32_t)b.size; _Hm.e[_Hm.n].used = b.used ? 1 : 0;
    _Hm.n++;
    return true;
}
static void heapmap_print(void)
{
    memset(&_Hm, 0, sizeof _Hm);
    const uint32_t fr = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_DEFAULT), lg = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    heap_caps_walk(MALLOC_CAP_DEFAULT, hm_walk, NULL);
    printf("cart heapmap: free %lu, largest block %lu (%lu not listed)\n", (unsigned long)fr, (unsigned long)lg, (unsigned long)_Hm.drop);
    for (uint32_t i = 0; i < _Hm.n; i++) {
        if (!_Hm.e[i].used) { printf("  FREE %lu at %08lx\n", (unsigned long)_Hm.e[i].sz, (unsigned long)_Hm.e[i].a); continue; }
        printf("  used %lu at %08lx", (unsigned long)_Hm.e[i].sz, (unsigned long)_Hm.e[i].a);
#ifdef CONFIG_HEAP_USE_HOOKS
        for (int k = 0; k < HK_N; k++) if (_HkRec[k].ptr == _Hm.e[i].a) {
            printf(" by");
            for (int z = 0; z < 8 && _HkRec[k].pc[z]; z++) printf(" %08lx", (unsigned long)_HkRec[k].pc[z]);
            break;
        }
#endif
        printf("\n");
    }
}
#else
static void heapmap_print(void) { printf("cart heapmap: TEST builds only\n"); }
#endif
static int con_ensure(void);
static void usb_line(void *u, const char *s)
{
    (void)u;
    /* in order with the frames when the console path is up; else plainly (a line is never lost
       to a path that was just taken down) */
    const size_t n = strlen(s);
    if (n + 2 <= 1100) {
        char *m = malloc(n + 2);
        if (m) {
            memcpy(m, s, n); m[n] = '\n';
            const int sent = con_send((const uint8_t *)m, (uint32_t)(n + 1));
            free(m);
            if (sent) return;
        }
    }
    printf("%s\n", s);
}
static volatile int _CartHeld;          /* the link hold of a USB cart job (cf_usb net_hold) */
static volatile uint32_t _CartCmdMs;    /* the last `cart usb` command (0: none since boot) */
/* No light sleep during a cart console session: from a `cart usb` command until the link hold
   is lifted (30 s after the last command with no job), and at least 60 s after any command
   (status / stage take no hold). Outside: false, the system sleeps as before. pwrmgr.c. */
bool CfDev_KeepAwake(void)
{
    if (_CartHeld || _Cf.armed || _Cf.done_pending || _Usb.job || _Usb.st_clearing) return true;
    if (CfRc_KeepAwake()) return true;   /* fwshrink1: a RAM cart load / library job / stop (cf_rc_dev.c says why) */
    if (Sd_Busy() == kSdOp_Job || Sd_Busy() == kSdOp_Backup || Sd_Busy() == kSdOp_ToCart) return true;   /* lane ui110dev: a GAMES job (packing, the staging area, the card) */
#ifdef CONFIG_NW_CARTFLASH_WIFI
    if (CfShare_Busy()) return true;     /* lane share1: a share (the radio, a cart read, the store, the card) */
    /* a Wi-Fi install between its steps (a poll, the header, a report) or not finished: light sleep
       stalled the resume poll on unit 2 for minutes, until the console woke the unit (wifiinst1) */
    if (cf_install_busy(&_Inst) || _Net.on) return true;
    if (_OtaPending) return true;        /* 1.1.9: a new version not confirmed yet: a nap would hold its confirm off, and a power cycle then rolls it back */
#endif
    return _CartCmdMs != 0 && (uint32_t)(now_ms() - _CartCmdMs) < 60000u;
}
static int usb_heap_ok(void *u) { (void)u; return esp_get_free_heap_size() >= CF_JOB_MIN_HEAP; }
static uint32_t usb_heap_free(void *u) { (void)u; return (uint32_t)esp_get_free_heap_size(); }
/* usb-cart-6: the link gives its heap back for a cart job, and takes it again afterwards.
   On (console task): no new TLS (CwlOnline_TlsHold), the link session ends (relay BYE, Wi-Fi
   stopped), the HTTPS call in flight ends and the kept clients are freed. Off (nw task, 30 s
   after the last cart command): the session the LINK CABLE mode wants starts again. RAM only. */
static void usb_net_hold(void *u, int on)
{
    (void)u;
    if (!on) {
        _CartHeld = 0;
        (void)CWL_CartHold(false, 0);
        printf("cf: link hold off: the link starts again\n");
        return;
    }
    const uint32_t f0 = (uint32_t)esp_get_free_heap_size();
    _CartHeld = 1;
    const bool s_ok = CWL_CartHold(true, 12000);
    const bool o_ok = CwlOnline_Quiet(9000);
    const bool n_ok = NW_DropClient(9000);
    printf("cf: link hold on: free heap %lu -> %lu%s%s%s\n", (unsigned long)f0, (unsigned long)esp_get_free_heap_size(),
           s_ok ? "" : " (the link session did not end in 12 s)", o_ok ? "" : " (an online call still running)",
           n_ok ? "" : " (a network window request still running)");
}
static void usb_set_baud(void *u, uint32_t baud)
{
    (void)u;
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(100));               /* the answer line leaves at the old speed */
    (void)uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(200));
    (void)uart_set_baudrate(UART_NUM_0, baud);
}

/* No TLS while a cart job or the probe is armed: relay tickets and gateway calls wait */
#ifdef CONFIG_NW_CARTFLASH_WIFI
#define CF_NET_ON _Net.on
#else
#define CF_NET_ON 0
#endif
bool CwlOnline_TlsHold(void) { return _Cf.armed || _Cf.probe_armed || CF_NET_ON || _CartHeld; }   /* ... or the install's connection is open: one TLS session, ever */

/* `cart usb ...`: its own small frame (noinline), nothing big on the console task's stack: the
   bench commands below keep a 1 KB frame of their own and are never under this one. */
static int __attribute__((noinline)) cmd_cart_usb(int argc, char **argv)
{
    /* the owner's own backup / restore over this cable (protocol CBU1, cf_usb.c). On the USB
       cable the unit is on USB power: the battery rule is met. */
    _PowerUsb = 1;
    _CartCmdMs = now_ms() | 1;           /* keeps the console awake (CfDev_KeepAwake) */
    if (!strcmp(argv[2], "read") || !strcmp(argv[2], "info") || !strcmp(argv[2], "write") || !strcmp(argv[2], "install")) {
        uint8_t e, b; unsigned mj, mn;
        if (!fpga_cmds(&e, &b, &mj, &mn)) {
            /* 18.23 (its link commands 7 / 8 are the slave lock) or an FPGA not read yet: nothing is sent to it */
            printf("CBU1 fail fpga=%u.%u (no cart job on this FPGA)\nCBU1 ready\n", mj, mn);
            return 0;
        }
        cf_usb_hold(&_Usb);               /* before the console path takes its 10 KiB */
        (void)con_ensure();
    }
    const int r = cf_usb_cmd(&_Usb, argc - 2, argv + 2);
    if (_Usb.bin) {
        /* Binary write: this IS the console's reader task, so nothing else reads UART0 now.
           It stays here and reads the frames in blocks, with no echo (the REPL takes ~1.3
           characters per ms; the frames come at 11.5), until the job ends or no frame came
           for 30 s. A little above the other tasks meanwhile, so the 384-byte receive
           buffer is emptied in time. */
        uint8_t *rb = malloc(512);
        if (!rb) { _Usb.bin = 0; printf("CBU1 text\n"); return r; }
        const UBaseType_t prio = uxTaskPriorityGet(NULL);
        vTaskPrioritySet(NULL, prio + 4);
        while (cf_usb_bin_active(&_Usb)) {
            const int n = uart_read_bytes(UART_NUM_0, rb, 512, pdMS_TO_TICKS(10));
            if (n > 0) cf_usb_rx(&_Usb, rb, (uint32_t)n);
        }
        vTaskPrioritySet(NULL, prio);
        free(rb);
    }
    return r;
}

static int __attribute__((noinline)) cmd_cart_bench(int argc, char **argv);
static int cmd_cart(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "usb")) return cmd_cart_usb(argc, argv);
    return cmd_cart_bench(argc, argv);
}

static int __attribute__((noinline)) cmd_cart_bench(int argc, char **argv)
{
    const char *sub = argc >= 2 ? argv[1] : "status";
    cf_job_t j = { .job = CF_JOB_NONE, .allow_unverified = 0, .ram_clear = 1, .max_retries = 2 };
    if (!strcmp(sub, "where")) { extern void CfRc_Where(void); CfRc_Where(); return 0; }   /* gamecomp1 bench */
    if (!strcmp(sub, "status")) {
        uint8_t job, ph;
        const int pend = cf_esp_pending(&_Cf, &job, &ph);
        printf("cart: armed %d phase %d | journal job %u state %u%s | backup %s (%u bytes, crc32 %08lx, %u slots; ram crc32 %08lx present %08lx) | last \"%s\" result %u | journal result %u | cmds %lu erases %lu programmed %lu\n",
               _Cf.armed, _Cf.ph, job, ph, pend ? " (INTERRUPTED: cart resume / cart restore)" : "",
               _Cf.j.bak_ok ? "stored" : "none", (unsigned)_Cf.j.bak_rom_size, (unsigned long)_Cf.j.bak_rom_crc32,
               (unsigned)_NextSlot, (unsigned long)_Cf.j.bak_ram_crc32, (unsigned long)_Cf.j.ram_present, _Last.text, _Last.result,
               _Cf.j.result, (unsigned long)_Cf.n_cmds, (unsigned long)_Cf.n_erase, (unsigned long)_Cf.n_program_bytes);
        printf("cart: heap free %lu, min since boot %lu, largest block %lu | at the last job's arm (job %u): free %lu, min %lu; lowest while it ran %lu | console path %s | overlay %d (0 off / none, 1 ON, -1 no report)\n",
               (unsigned long)esp_get_free_heap_size(), (unsigned long)esp_get_minimum_free_heap_size(),
               (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT), _ArmJob, (unsigned long)_ArmHeap, (unsigned long)_ArmMin,
               (unsigned long)_JobLow, _ConSb ? "up" : "down", Patch_CartOverlay(false));
        {
            /* the least stack each task has ever had left, in bytes (usb-cart-1 overflowed the console's) */
            TaskHandle_t nw = xTaskGetHandle("nw"), ht = xTaskGetHandle("nw_http"), cc = xTaskGetHandle("cf_con");
            printf("cart: stack left at worst: console %u, nw %d, nw_http %d, cf_con %d (bytes; -1 = no such task now)\n",
                   (unsigned)uxTaskGetStackHighWaterMark(NULL), nw ? (int)uxTaskGetStackHighWaterMark(nw) : -1,
                   ht ? (int)uxTaskGetStackHighWaterMark(ht) : -1, cc ? (int)uxTaskGetStackHighWaterMark(cc) : -1);
        }
        printf("cart: writer cmds %lu, retries %lu, answers repeated %lu, bytes read %lu\n", (unsigned long)_Cf.n_cmds, (unsigned long)_Cf.n_retries, (unsigned long)_Cf.n_dup, (unsigned long)_Cf.n_read_bytes);
        return 0;
    }
    if (!strcmp(sub, "power") && argc >= 3 && (!strcmp(argv[2], "low") || !strcmp(argv[2], "real"))) {
        /* bench (1.0.5 gate): the battery reads as red (`low`) or as it is (`real`). RAM only, until reboot */
        _PowerLowTest = !strcmp(argv[2], "low");
        if (_PowerLowTest) _PowerUsb = 0;
        printf("cart: power check %s (bench; until reboot)\n", _PowerLowTest ? "forced LOW: the battery reads as red" : "back to the real battery");
        return 0;
    }
    if (!strcmp(sub, "power") && argc >= 3 && !strcmp(argv[2], "usb")) {
        _PowerUsb = 1;
        printf("cart: power check overridden: the unit is on USB power (bench; until reboot)\n");
        ESP_LOGW(TAG, "power check overridden from the console (USB power)");
        return 0;
    }
#ifdef CONFIG_NW_CARTFLASH_WIFI
    if (!strcmp(sub, "sdraw") && argc >= 4) {
        /* lane sdcard1 bench: COUNT raw sectors of the microSD card from LBA into the staging area (read back over USB
           with esptool: the Mac-side check of the card's volume). Nothing else may use the staging area meanwhile. */
        const uint32_t lba = (uint32_t)strtoul(argv[2], NULL, 0), cnt = (uint32_t)strtoul(argv[3], NULL, 0);
        if (!cnt || cnt * 512u > CF_STAGE_ROM_MAX || !jobs_idle()) { printf("cart: sdraw e=1 (count, or the unit is busy)\n"); return 0; }
        uint8_t *b = heap_caps_malloc(4096, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!b) { printf("cart: sdraw e=1 no memory\n"); return 0; }
        stage_erase(NULL);
        int e = wait_erase(60000);
        const uint32_t t0 = now_ms();
        for (uint32_t i = 0; !e && i < cnt; i += 8) {
            const uint32_t k = cnt - i < 8 ? cnt - i : 8;
            e = Sd_RawRead(lba + i, b, k) || stage_write_nonff(CF_STAGE_ROM_OFF + i * 512u, b, k * 512u);
            if ((i & 63u) == 0) vTaskDelay(1);         /* (the watchdog: a long copy restarted the unit) */
        }
        heap_caps_free(b);
        printf("cart: sdraw e=%d %lu sectors from %lu at flash %06lx (%lu ms)\n", e, (unsigned long)cnt, (unsigned long)lba, (unsigned long)CF_STAGE_ROM_OFF, (unsigned long)(now_ms() - t0));
        return 0;
    }
#endif
    if (!strcmp(sub, "stage") && argc >= 4) {
        /* Bench stand-in for the "Send to my Chromatic" download: the image was already
           written to the image region (end of "cart" - CF_IMG_CAP) over USB; take it only
           if its size and crc32 match what the owner typed. */
        const uint32_t sz = (uint32_t)strtoul(argv[2], NULL, 0), want = (uint32_t)strtoul(argv[3], NULL, 16);
        if (!_Part || !sz || sz > CF_IMG_CAP) { printf("cart: stage refused (no partition or size)\n"); return 1; }
        /* a bench command (needs the cart partition): its buffer is made at its first use and
           kept; never static (heap for good on every unit) and never on the console's 4 KB stack */
        enum { NB = 512 };
        static uint8_t *b;
        if (!b) b = (uint8_t *)malloc(NB);
        if (!b) { printf("cart: no memory\n"); return 1; }
        uint32_t crc = 0;
        for (uint32_t o = 0; o < sz; o += NB) {
            const uint32_t k = sz - o < NB ? sz - o : NB;
            if (esp_partition_read(_Part, _Part->size - CF_IMG_CAP + o, b, k) != ESP_OK) { printf("cart: stage read failed\n"); return 1; }
            crc = cf_crc32(crc, b, k);
        }
        if (crc != want) { printf("cart: stage refused: crc32 %08lx != %08lx\n", (unsigned long)crc, (unsigned long)want); return 1; }
        _ImgSize = sz;
        _Cf.h.img_size = sz;
        printf("cart: image staged: %lu bytes, crc32 %08lx\n", (unsigned long)sz, (unsigned long)crc);
        return 0;
    }
    if (!strcmp(sub, "xs")) {        /* cartspeed1 bench: cart xs [on|off|timing N] */
        if (argc >= 3 && !strcmp(argv[2], "off")) _XsOff = 1;
        else if (argc >= 3 && !strcmp(argv[2], "on")) _XsOff = 0;
        else if (argc >= 4 && !strcmp(argv[2], "timing")) _XsTiming = (uint8_t)(atoi(argv[3]) & 3);
#ifdef CONFIG_NW_CARTFLASH_WIFI
        else if (argc >= 4 && !strcmp(argv[2], "prog")) { _XsProgOff = !strcmp(argv[3], "off"); printf("cart: xs prog %s (the Wi-Fi cart write %s)\n", _XsProgOff ? "off" : "on", _XsProgOff ? "after the whole download" : "behind the download"); }
#endif
        static const struct cf_xs none;
        const struct cf_xs *x = _Cf.xs ? _Cf.xs : &none;
        printf("cart: xs %s (fpga %s) timing %u | last job: on %u st %u status %lu posts %lu refused %lu restarts %lu verified %lu missed %lu cons %lu valid %lu err %u\n",
               _XsOff ? "off" : "on", xs_avail(NULL) || _XsOff ? "has it" : "has not", (unsigned)_XsTiming, (unsigned)x->on, (unsigned)x->st,
               (unsigned long)x->n_status, (unsigned long)x->n_posts, (unsigned long)x->n_naks, (unsigned long)x->n_resync, (unsigned long)x->verified, (unsigned long)x->n_missed,
               (unsigned long)x->cons, (unsigned long)x->valid, (unsigned)x->err);
        return 0;
    }
#ifdef CONFIG_NW_CARTFLASH_WIFI
    if (!strcmp(sub, "code")) {      /* bench: what SYSTEM > DOWNLOAD shows (this unit's own USB console; never logged by itself) */
        cf_view_t v;
        cf_install_view(&_Inst, &v);
        printf("cart: page %d code %s mins %u\n", v.kind, v.code[0] ? v.code : "-", v.mins);
        return 0;
    }
    if (!strcmp(sub, "rxprobe")) {   /* bench (wifimem1): receive mode's link hold + N device calls (cwl relay rxprobe) */
        if (argc >= 3 && !strcmp(argv[2], "off")) {
            char *a[] = {"relay", "rxprobe", "0"};
            (void)CwlOnline_Console(3, a);
            if (_ProbeHold) { _ProbeHold = 0; ci_hold(NULL, 0); }
            return 0;
        }
        if (argc < 3 || cf_install_busy(&_Inst)) { printf("cart: rxprobe N [MS] [fresh] | off (not while receive mode or an install runs)\n"); return 1; }
        if (!_ProbeHold) { _ProbeHold = 1; ci_hold(NULL, 1); }
        for (int t = 0; t < 120 && !CWL_RecvHeld(); t++) vTaskDelay(pdMS_TO_TICKS(50));
        heap_note("rxprobe: link stopped");
        char *a[] = {"relay", "rxprobe", argv[2], argc >= 4 ? argv[3] : "4000", argc >= 5 ? argv[4] : "kept"};
        return CwlOnline_Console(5, a) ? 1 : 0;
    }
#ifdef CONFIG_HEAP_USE_HOOKS
    if (!strcmp(sub, "peak")) { extern void pk_print(int); pk_print(argc >= 3 && !strcmp(argv[2], "arm")); return 0; }
#endif
    if (!strcmp(sub, "stacks")) {    /* 1.0.3: the least stack every known task has ever had left (bytes), and the heap */
        static const char *const names[] = { "fpga_tx_task", "fpga_rx_task", "lvgl Timer", "pwr_mgr_task", "cwl", "cwl_online", "cwlnet",
                                             "nw", "nw_http", "cf_con", "Tmr Svc", "tiT", "wifi", "sys_evt", "esp_timer", "main",
                                             "console_repl", "ipc0", "ipc1", "IDLE0", "IDLE1" };
        printf("cart: stacks (least left, bytes):");
        for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
            TaskHandle_t t = xTaskGetHandle(names[i]);
            if (t) printf(" %s=%u", names[i], (unsigned)uxTaskGetStackHighWaterMark(t));
        }
        printf(" | this=%u | heap free %lu, min %lu, largest %lu\n", (unsigned)uxTaskGetStackHighWaterMark(NULL), (unsigned long)esp_get_free_heap_size(),
               (unsigned long)esp_get_minimum_free_heap_size(), (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
        return 0;
    }
    if (!strcmp(sub, "heapmap")) {   /* cartspeed2 bench: which used blocks split the free heap (addresses + sizes, no contents) */
        if (argc >= 3) _HeapMapOn = !strcmp(argv[2], "on");
        heapmap_print();
        return 0;
    }
    if (!strcmp(sub, "receive")) {   /* bench: RECEIVE GAME, as A on SYSTEM > DOWNLOAD's menu */
        cf_install_button(&_Inst, 1);
        NW_CfOpenMenu();               /* and SYSTEM > DOWNLOAD on the screen, as where the player pressed it */
        printf("cart: receive (state %s)\n", cf_install_state(&_Inst));
        return 0;
    }
    if (!strcmp(sub, "inst")) {
        cf_view_t v;
        cf_install_view(&_Inst, &v);
        printf("cart: install %s%s | codes %u expired %u polls %u offers %u installed %u declined %u failed %u resumes %u | last offer \"%s\" %s %lu B | power %s%s | page %d | registered %d | reports left %d\n",
               cf_install_state(&_Inst), _Inst.unfinished ? " (UNFINISHED)" : "", _Inst.n_codes, _Inst.n_expired, _Inst.n_polls, _Inst.n_offers, _Inst.n_installs,
               _Inst.n_declined, _Inst.n_failed, _Inst.n_resumes, _Inst.offer.title, _Inst.offer.id, (unsigned long)_Inst.offer.size,
               CfDev_PowerOk() ? "ok" : "LOW", _PowerUsb ? " (override)" : _PowerAnyway ? " (INSTALL ANYWAY)" : _PowerLowTest ? " (forced low)" : "", v.kind, CwlOnline_Registered() ? 1 : 0, _Inst.rep_left);
        printf("cart: receive mode %s | heap before the hold %lu, after it %lu, lowest while receiving %lu\n", _Inst.rx ? "ON" : "off",
               (unsigned long)_RxHeap0, (unsigned long)_RxHeapHeld, (unsigned long)_RxHeapLow);
        printf("cart: stream net: gets %u connects %u errors %u heap-waits %u min-heap %u last \"%s\" | free heap %lu\n", _Net.n_get, _Net.n_conn, _Net.n_err,
               _Net.n_heap_waits, _Net.min_heap, _Net.why, (unsigned long)esp_get_free_heap_size());
        {
            BatteryState_t b;
            Battery_GetState(&b);
            printf("cart: battery kind %d valid %d level %.2f V mid %.2f V charging %d no-battery %d | largest block %lu, min since boot %lu\n", (int)b.Kind, b.Valid, (double)b.Level_V,
                   (double)b.Mid_V, b.Charging, b.NoBattery, (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT), (unsigned long)esp_get_minimum_free_heap_size());
        }
        return 0;
    }
#endif /* CONFIG_NW_CARTFLASH_WIFI */
#ifdef CONFIG_NW_CARTFLASH_WIFI
    if (!strcmp(sub, "fw") && argc >= 3 && (!strcmp(argv[2], "slots") || !strcmp(argv[2], "switch"))) {
        const int switching = !strcmp(argv[2], "switch");
        const int slot = argc == 4 && !strcmp(argv[3], "gb") ? CF_FOTA_SLOT_GB :
                         argc == 4 && !strcmp(argv[3], "gg") ? CF_FOTA_SLOT_GG : -1;
        if (switching && slot < 0) { printf("cart: fw switch gb|gg\n"); return 0; }
        if (switching) {
            const int e = CfDev_FpgaSwitch(slot);
            printf("cart: fw switch e=%d slot %s%s\n", e, argv[3], !e ? " (boots at the next power-on)" : e == CF_CORE_NOT_READY ? " (not ready: try again)" : "");
        } else {
            fota_rec_t job;
            if (!fpga_can_fota() || !jobs_idle() || CfRc_Playing() || _FjBusy || _FjResume || _FcReq == 1 ||
                (_Od && _Od->st == 0) || !fota_load(&job, "fjob")) {
                printf("cart: fw %s: busy or FPGA unavailable\n", argv[2]); return 0;
            }
            (void)CfDev_FpgaSlot();
            cf_fota_layout_t L;
            int e = cf_fota_scan(CfRc_FotaHost(), &L);
            if (e || !_FpgaSlotRead) { printf("cart: fw slots: not ready: try again (scan %d)\n", e); return 0; }
            char bases[2][16];
            for (int i = 0; i < 2; i++) {
                uint32_t base;
                const int se = cf_fota_slot_base(CfRc_FotaHost(), i, &base);
                if (!se) snprintf(bases[i], sizeof bases[i], "%06lx", (unsigned long)base);
                else snprintf(bases[i], sizeof bases[i], "%s", se == CF_FOTA_E_EMPTY ? "empty" : "not ready");
            }
            const int live = L.kind == CF_FL_RF || L.kind == CF_FL_RF_FULL ? cf_fota_slot_of(L.full_base) : -1;
            printf("cart: fw slots %s header %03lx live %d gb %s gg %s level-1 %d booted %d\n",
                   L.kind == CF_FL_RF ? "rf" : L.kind == CF_FL_RF_FULL ? "rffull" : L.kind == CF_FL_RF_REC ? "rfrec" : "legacy",
                   (unsigned long)L.live_at, live, bases[0], bases[1], L.free, CfDev_FpgaSlot());
            printf("cart: fw scheme %s level-2 %d switches left %d\n",
                   L.kind == CF_FL_LEGACY ? "unknown" : !L.selector ? "direct" :
                   L.selector == CF_FOTA_SELECTOR_A ? "chain A" : "chain B",
                   L.l2_free, cf_fota_switches_left(&L));
        }
        return 0;
    }
    if (!strcmp(sub, "fw") && argc >= 5 && !strcmp(argv[2], "stage")) {
        /* bench (fpgacomp1): an FPGA image written by esptool at the start of the app slot that is not running becomes the
           waiting FPGA part of an update ("fready"), as a download leaves it: cart fw stage LEN VER [OFF]; then cart fw inst */
        const esp_partition_t *p = esp_ota_get_next_update_partition(NULL);
        const uint32_t len = (uint32_t)strtoul(argv[3], NULL, 0), off = argc >= 6 ? (uint32_t)strtoul(argv[5], NULL, 0) : 0;
        if (!p || len < CF_FOTA_IMG_MIN || len > CF_FOTA_FULL_MAX || off + len > p->size) { printf("cart: fw stage: no\n"); return 0; }
        uint8_t *b = malloc(4096), first[64], last[64];
        if (!b) return 0;
        mbedtls_sha256_context sc; mbedtls_sha256_init(&sc); (void)mbedtls_sha256_starts(&sc, 0);
        uint32_t crc = 0;
        for (uint32_t o = 0; o < len; o += 4096u) {
            const uint32_t n = len - o < 4096u ? len - o : 4096u;
            (void)esp_partition_read(p, off + o, b, n);
            crc = cf_fota_crc32(crc, b, n);
            (void)mbedtls_sha256_update(&sc, b, n);
        }
        free(b);
        (void)esp_partition_read(p, off, first, 64); (void)esp_partition_read(p, off + len - 64u, last, 64);
        fota_rec_t f; memset(&f, 0, sizeof f);
        snprintf(f.rel, sizeof f.rel, "bench"); snprintf(f.ver, sizeof f.ver, "%s", argv[4]);
        f.size = len; f.crc = crc; f.slot = p->address + off; f.parts = 2;
        (void)mbedtls_sha256_finish(&sc, f.sha); mbedtls_sha256_free(&sc);
        const int ok = !cf_fota_image_ok(first, last, len);
        if (ok) fota_save(&f, "fready");
        printf("cart: fw stage %s %lu bytes at %06lx crc %08lx image %s\n", f.ver, (unsigned long)len, (unsigned long)f.slot,
               (unsigned long)crc, ok ? "ok (fready saved)" : "NOT a GW5A-25 image of this project");
        return 0;
    }
    if (!strcmp(sub, "fw") && argc >= 4 && !strcmp(argv[2], "hold")) {   /* bench: receive mode as SYSTEM > UPDATE holds it */
        ci_hold(NULL, atoi(argv[3]) ? 1 : 0);
        printf("cart: fw hold %d\n", atoi(argv[3]) ? 1 : 0);
        return 0;
    }
    if (!strcmp(sub, "fw") && argc >= 6 && !strcmp(argv[2], "dl")) {
        /* bench (fpgacomp1): an FPGA-only download as an offer starts it (no fw/check): cart fw dl URL SIZE SHA256; it is
           READY when done (cart fw page), its time printed by part */
        static cf_ota_offer_t o;
        memset(&o, 0, sizeof o);
        snprintf(o.version, sizeof o.version, "bench");
        snprintf(o.fversion, sizeof o.fversion, "18.99");
        snprintf(o.furl, sizeof o.furl, "%s", argv[3]);
        o.fsize = (uint32_t)strtoul(argv[4], NULL, 0);
        snprintf(o.fsha256, sizeof o.fsha256, "%s", argv[5]);
        CfRc_Trim();
        const int e = oh_dl_start(NULL, &o);
        printf("cart: fw dl %s (%d)\n", e ? "refused" : "started", e);
        return 0;
    }
    if (!strcmp(sub, "fw") && argc >= 3 && !strcmp(argv[2], "inst")) {   /* bench: the FPGA install now, on this task */
        const uint32_t t0 = now_ms();
        const int e = fota_install_now();
        printf("cart: fw inst e=%d res %d (%lu ms)\n", e, _FjRes, (unsigned long)(now_ms() - t0));
        return 0;
    }
    if (!strcmp(sub, "fw")) {        /* bench (1.1.0): SYSTEM > UPDATE from the console: cart fw [a|b] (the page's A / B), then what it shows */
        if (argc >= 3 && (!strcmp(argv[2], "a") || !strcmp(argv[2], "b"))) CfDev_FwButton(!strcmp(argv[2], "a"));
        if (argc >= 3 && !strcmp(argv[2], "show")) NW_FwOpenMenu();   /* and SYSTEM > UPDATE on the screen */
        NwFwView_t v;
        CfDev_FwView(&v);
        const esp_partition_t *run = esp_ota_get_running_partition();
        printf("cart: fw page %d \"%s\" \"%s\" version %s pct %d ready %d | running %s %s | stg %06lx hold %d | fl %s scan %d/%d\n", v.Kind, v.Title, v.Note, v.Version, v.Pct,
               v.ReadyWaits, run ? run->label : "-", v.Running, (unsigned long)(cf_stage_hold ? 0 : cf_stage_rom_off), cf_stage_hold,
               !_FlOk ? "unread" : _FlL.kind == CF_FL_RF ? "rf" : _FlL.kind == CF_FL_RF_REC ? "rfrec" : _FlL.kind == CF_FL_RF_FULL ? "rffull" : "legacy",
               (int)_FlScan, _FlScanTries);
        return 0;
    }
    if (!strcmp(sub, "answer") && argc >= 3) {   /* bench: SYSTEM > DOWNLOAD's A ("yes") / B from the console */
        cf_install_button(&_Inst, !strcmp(argv[2], "yes"));
        printf("cart: pressed %s\n", !strcmp(argv[2], "yes") ? "A" : "B");
        return 0;
    }
#endif /* CONFIG_NW_CARTFLASH_WIFI */
    if (!strcmp(sub, "save") && argc >= 3) {
        /* cart save get | put | clear | test: jobs (below). cart save crc: the store's crc32.
           cart save show: the store as base64 lines (the owner's own USB console only).
           cart save load <offset> <base64>: bytes into the store (512-byte pieces, in order). */
        const char *w = argv[2];
        if (!strcmp(w, "crc")) { printf("cart: save store crc32 %08lx\n", (unsigned long)sav_crc()); return 0; }
        if (!strcmp(w, "show")) {
            uint8_t b[96]; char o[136];
            for (uint32_t off = 0; off < CF_SAV_SIZE; off += sizeof b) {
                static const char k[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                if (sav_read(NULL, off, b, sizeof b) && off + sizeof b <= CF_SAV_SIZE) return 1;
                uint32_t n = CF_SAV_SIZE - off < sizeof b ? CF_SAV_SIZE - off : sizeof b, q = 0;
                for (uint32_t i = 0; i < n; i += 3) {
                    const uint32_t v = (uint32_t)b[i] << 16 | (i + 1 < n ? (uint32_t)b[i + 1] << 8 : 0) | (i + 2 < n ? b[i + 2] : 0);
                    o[q++] = k[v >> 18 & 63]; o[q++] = k[v >> 12 & 63]; o[q++] = i + 1 < n ? k[v >> 6 & 63] : '='; o[q++] = i + 2 < n ? k[v & 63] : '=';
                }
                o[q] = 0;
                printf("cf: savestore %05lx ok %s\n", (unsigned long)off, o);
                if ((off / sizeof b) % 16 == 15) vTaskDelay(pdMS_TO_TICKS(10));
            }
            printf("cf: savestore done: crc32 %08lx\n", (unsigned long)sav_crc());
            return 0;
        }
        if (!strcmp(w, "load") && argc >= 5) {
            if (_Cf.armed) { printf("cart: refused: a job is armed\n"); return 1; }
            const uint32_t off = (uint32_t)strtoul(argv[3], NULL, 0);
            enum { NL = 768 };
            static uint8_t *b;                              /* as above: made at first use */
            if (!b) b = (uint8_t *)malloc(NL);
            if (!b) { printf("cart: no memory\n"); return 1; }
            uint32_t n = 0, acc = 0; int bits = 0;
            for (const char *p = argv[4]; *p && *p != '=' && n < NL; p++) {
                const int v = b64v(*p);
                if (v < 0) { printf("cart: save load: bad base64\n"); return 1; }
                acc = acc << 6 | (uint32_t)v; bits += 6;
                if (bits >= 8) { bits -= 8; b[n++] = (uint8_t)(acc >> bits); }
            }
            if (!n || sav_write(NULL, off, b, n)) { printf("cart: save load refused at %05lx\n", (unsigned long)off); return 1; }
            printf("cart: save load %05lx +%lu\n", (unsigned long)off, (unsigned long)n);
            return 0;
        }
        if (!strcmp(w, "get")) j.job = CF_JOB_SAVE_GET;
        else if (!strcmp(w, "put")) j.job = CF_JOB_SAVE_PUT;
        else if (!strcmp(w, "clear")) j.job = CF_JOB_SAVE_CLEAR;
        else if (!strcmp(w, "test")) j.job = CF_JOB_RAMTEST;
        else { printf("usage: cart save get | put | clear | test | crc | show | load <offset> <base64>\n"); return 1; }
        j.ram_clear = 0;
        const int r = esp_get_free_heap_size() < CF_JOB_MIN_HEAP ? CF_D_ABORTED : cf_esp_arm(&_Cf, &j);
        printf("cart: %s\n", r ? "refused" : "armed; the cart power-cycles and the writer starts");
        return r ? 1 : 0;
    }
#ifdef CONFIG_NW_CARTFLASH_WIFI
    if (!strcmp(sub, "fetch") && argc >= 5) {
        /* cart fetch <path below the gateway base> <size> <crc32 hex>: download + verify + stage */
        if (_Cf.armed || _Cf.probe_armed) { printf("cart: fetch refused: a cart job or the probe is armed\n"); return 1; }
        if (!_Part) { printf("cart: fetch refused: no cart partition\n"); return 1; }
        _ImgSize = 0; _Cf.h.img_size = 0;                       /* the region is about to change */
        const cf_fetch_host_t h = fe_host();
        const int r = cf_fetch(&h, argv[2], (uint32_t)strtoul(argv[3], NULL, 0), (uint32_t)strtoul(argv[4], NULL, 16), 6);
        if (r == CF_FE_OK) fetch_restage();
        else printf("cart: fetch failed, code %d (%s)\n", r, NW_CfLastWhy());
        return r ? 1 : 0;
    }
#endif /* CONFIG_NW_CARTFLASH_WIFI */
    if (!strcmp(sub, "probe")) {
        /* the read-only probe over USB; `cart probe flashid` = step 2 (OWNER APPROVAL) */
        if (con_ensure()) {
            printf("cart: probe refused: not enough free heap for the console stream (%lu bytes free)\n",
                   (unsigned long)esp_get_free_heap_size());
            return 1;
        }
        {
            uint8_t e, b; unsigned mj, mn;
            if (!fpga_cmds(&e, &b, &mj, &mn)) { printf("cart: refused: no cart job on FPGA %u.%u\n", mj, mn); return 1; }
        }
        const int r = cf_esp_arm_probe(&_Cf, argc >= 3 && !strcmp(argv[2], "flashid"));
        printf("cart: %s\n", r ? "refused (a cart job is armed)" : "probe armed; the cart power-cycles, the probe calls follow as CPX lines");
        return r ? 1 : 0;
    }
    if (!strcmp(sub, "px") && argc >= 4) {
        char *const a = malloc(1100);    /* console lines are at most 1024 (main.c) */
        if (!a) return 1;
        snprintf(a, 1100, "%s %s %s", argv[2], argv[3], argc >= 5 ? argv[4] : "");
        const int r = cf_esp_host_answer(&_Cf, a);
        free(a);
        return r ? 1 : 0;
    }
    if (!strcmp(sub, "backup")) j.job = CF_JOB_BACKUP;
    else if (!strcmp(sub, "install")) j.job = CF_JOB_INSTALL;
    else if (!strcmp(sub, "resume")) j.job = _Cf.j.job ? _Cf.j.job : CF_JOB_INSTALL;
    else if (!strcmp(sub, "restore")) j.job = CF_JOB_RESTORE;
    else if (!strcmp(sub, "selftest")) j.job = CF_JOB_SELFTEST;     /* blank-unit pattern + erase back */
    else if (!strcmp(sub, "roundtrip")) { j.job = CF_JOB_ROUNDTRIP; j.ram_clear = 0; }   /* the cart's own image */
    else if (!strcmp(sub, "ramdump")) { j.job = CF_JOB_RAMDUMP; j.ram_clear = 0; }      /* read only, engine cycles */
    else if (!strcmp(sub, "gbtest")) { j.job = CF_JOB_GBTEST; j.ram_clear = 0; }        /* read only: GB-timed vs engine */
    else {
        printf("usage: cart status | probe [flashid] | px <id> <st> <b64> | backup | install | resume | restore | selftest | roundtrip   [unverified]\n"
               "       cart fetch <gateway path> <size> <crc32> | stage <size> <crc32> | ramdump | gbtest | power usb\n"
               "       cart save get | put | clear | test | crc | show | load <offset> <base64>\n"
               "  (export / forget / fetch: TODO, see CART_FLASH_DESIGN.md)\n");
        return 1;
    }
    if (argc >= 3 && !strcmp(argv[2], "unverified")) j.allow_unverified = 1;   /* owner-approved tests only */
    {
        uint8_t e, b; unsigned mj, mn;
        if (!fpga_cmds(&e, &b, &mj, &mn)) { printf("cart: refused: no cart job on FPGA %u.%u\n", mj, mn); return 1; }
    }
    if (esp_get_free_heap_size() < CF_JOB_MIN_HEAP) {
        printf("cart: refused: %lu bytes of free heap, a job needs %u (turn the link off: cwl off)\n",
               (unsigned long)esp_get_free_heap_size(), CF_JOB_MIN_HEAP);
        return 1;
    }
    const int r = cf_esp_arm(&_Cf, &j);
    printf("cart: %s\n", r ? "refused" : "armed; the cart power-cycles and the writer starts");
    return r ? 1 : 0;
}

#include "mbedtls/sha1.h"
#include "mbedtls/bignum.h"
#include <sys/time.h>
#include <time.h>
#include "mbedtls/sha512.h"
#ifdef CONFIG_NW_CARTFLASH_WIFI
/* ---- lane sdcard1: the cart and the microSD card (SYSTEM > GAMES; run on the card's task, sd.c)
 *   backup   the cart in the slot read into the staging area (the Wi-Fi backup's own read job and length rule),
 *            then copied to /GAMES/<its title>.gb(c) (a file of the same game already there is kept as it is)
 *            and its save RAM (the header's size) to /SAVES/<the same name>.sav; nothing is written to the cart
 *   to_cart  a game on the card copied into the staging area, checked (crc32 + sha256), then written to the cart
 *            by the USB install's own job (the cart's save RAM left alone)
 * Both wait for the cart jobs on the nw task; the staged copy is erased when they end. */
static int cart_in_slot(void) { return CfRc_CartIn(); }
static int jobs_idle(void) { return !_Cf.armed && !_Cf.done_pending && !_StageErasing && !_Net.on && !_Bk && _Inst.st == CI_IDLE && !CfRc_Playing(); }
static int wait_jobs(void (*pct)(int), int p0, int p1, uint32_t ms)
{
    const uint32_t t0 = now_ms();
    while ((_Cf.armed || _Cf.done_pending) && now_ms() - t0 < ms) {
        if (pct && _Cf.st.total_bytes) pct(p0 + (int)((uint64_t)(p1 - p0) * _Cf.st.done_bytes / _Cf.st.total_bytes));
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return (_Cf.armed || _Cf.done_pending) ? -1 : 0;
}
static int wait_erase(uint32_t ms)
{
    const uint32_t t0 = now_ms();
    while (_StageErasing && now_ms() - t0 < ms) vTaskDelay(pdMS_TO_TICKS(50));
    return (_StageErasing || _StageEraseFailed) ? -1 : 0;
}
static int stage_to_card(int sav, uint32_t len, const char *path, uint32_t *crc_out)
{
    uint8_t *b = malloc(4096);
    sd_wr_t *w = malloc(sizeof *w);
    uint32_t crc = 0;
    int e = !b || !w || Sd_WriteBegin(w, path, sav ? 0 : len);
    for (uint32_t o = 0; !e && o < len; o += 4096u) {
        const uint32_t k = len - o < 4096u ? len - o : 4096u;
        e = stage_read(NULL, sav, o, b, k) || Sd_WriteAt(w, o, b, k);
        if (!e) crc = cf_crc32(crc, b, k);
    }
    if (!e) e = Sd_WriteCommit(w); else if (w && w->open) Sd_WriteAbort(w);
    free(b); free(w);
    if (crc_out) *crc_out = crc;
    return e;
}
/* lane ui110dev: the cartridge's title as the last cart job of this boot read or wrote it ("" not known: reading the header
   would stop the game the cartridge is playing), forgotten when the cartridge leaves the slot */
static char _CartTitle[17];
void CfDev_CartTitle(char *out, unsigned n)
{
    if (!n) return;
    if (_CartTitle[0] && !Sd_CartIn()) _CartTitle[0] = 0;
    snprintf(out, n, "%s", _CartTitle);
}
static void cart_title_from_stage(void)
{
    uint8_t h[0x150];
    sdc_hdr_t hd;
    static const uint8_t logo[8] = { 0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B };
    if (stage_read(NULL, 0, 0, h, sizeof h) || memcmp(h + 0x104, logo, 8)) { _CartTitle[0] = 0; return; }
    (void)sdc_hdr_parse(h, CF_STAGE_ROM_MAX, &hd);
    snprintf(_CartTitle, sizeof _CartTitle, "%s", hd.title[0] ? hd.title : "");
    for (char *q = _CartTitle; *q; q++) if (*q == '_') *q = ' ';
}
/* the cart in the slot read into the staging area (erased first): its ROM (the backup's length rule: _Bk->rom_len) and its
   save RAM. 0 = there; else res says why */
static int cart_read_stage(void (*pct)(int), int p0, int p1, const char *no, char *res, size_t n)
{
    if (_DlP) dl_cleanup();
    if (wait_erase(30000)) { snprintf(res, n, "%s|TRY  AGAIN", no); return -1; }
    stage_erase(NULL);
    if (wait_erase(60000)) { snprintf(res, n, "%s|TRY  AGAIN", no); return -1; }
    for (int what = 0; what < 2; what++) {
        cf_job_t j;
        memset(&j, 0, sizeof j);
        j.job = CF_JOB_USBREAD; j.rd_what = (uint8_t)what; j.rd_skip = what == 0; j.rd_off = 0; j.rd_len = what ? CF_RAM_SIZE : CF_STAGE_ROM_MAX; j.max_retries = 2;
        _Bk->sink = 1; _Bk->err = 0;
        _Cf.st.result = 0xEE;
        int a = cf_esp_arm(&_Cf, &j);
        for (int t = 0; a == CF_D_ABORTED && t < 50; t++) { vTaskDelay(pdMS_TO_TICKS(200)); a = cf_esp_arm(&_Cf, &j); }
        if (a) { _Bk->sink = 0; snprintf(res, n, "%s|THE  CART  WAS  NOT  READ", no); return -1; }
        if (wait_jobs(pct, what ? p0 + (p1 - p0) * 9 / 10 : p0, what ? p1 : p0 + (p1 - p0) * 9 / 10, 300000)) { _Bk->sink = 0; snprintf(res, n, "%s|THE  CART  WAS  NOT  READ", no); return -1; }
        _Bk->sink = 0;
        if (_Cf.st.result != CF_D_OK || _Bk->err) { printf("cf: the cart read failed (result %u)\n", _Cf.st.result); snprintf(res, n, "%s|THE  CART  WAS  NOT  READ", no); return -1; }
    }
    bk_len();
    cart_title_from_stage();
    return 0;
}
static int card_backup2(void (*pct)(int), int p0, int p1, char *res, size_t n);
static int card_backup(void (*pct)(int), char *res, size_t n) { return card_backup2(pct, 0, 100, res, n); }
static int card_backup2(void (*pct)(int), int p0, int p1, char *res, size_t n)
{
    (void)CfRc_SlotBack();
    if (!cart_in_slot()) { snprintf(res, n, "NOT  BACKED  UP|NO  CART  IN  THE  SLOT"); return -1; }
    if (!jobs_idle()) { snprintf(res, n, "NOT  BACKED  UP|CHROMATIC  IS  BUSY"); return -1; }
    if (!(_Bk = calloc(1, sizeof *_Bk))) { snprintf(res, n, "NOT  BACKED  UP|NOT  ENOUGH  MEMORY"); return -1; }
    _Bk->sock = -1; _Bk->ps_saved = -1; _Bk->st = BK_ROM; _Bk->t0 = now_ms();
    int rc = -1;
    printf("cf: card backup: this cart is read into the staging area, then copied to the card\n");
    if (cart_read_stage(pct, p0, p0 + (p1 - p0) * 8 / 10, "NOT  BACKED  UP", res, n)) goto out;
    {
        uint8_t h[0x150];
        sdc_hdr_t hd;
        static const uint8_t logo[8] = { 0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B };
        if (stage_read(NULL, 0, 0, h, sizeof h) || memcmp(h + 0x104, logo, 8)) { snprintf(res, n, "NOT  BACKED  UP|NO  GAME  ON  THE  CART"); rc = -2; goto out; }
        (void)sdc_hdr_parse(h, _Bk->rom_len, &hd);
        char name[64], file[64], path[SDC_PATH_MAX], sp[SDC_PATH_MAX + 8];
        snprintf(name, sizeof name, "%s", hd.title[0] ? hd.title : "CART");
        for (char *q = name; *q; q++) if (*q == '_') *q = ' ';
        if (sdc_clean_file(name, (hd.cgb & 0x80) ? ".gbc" : ".gb", file, sizeof file)) snprintf(file, sizeof file, "cart%s", (hd.cgb & 0x80) ? ".gbc" : ".gb");
        /* the same game already on the card under its name: kept (only its save is copied) */
        snprintf(path, sizeof path, SDC_GAMES_DIR "/%s", file);
        uint32_t have = 0, crc = 0;
        int same = 0;
        if (Sd_FileSize(path, &have) == 0 && have == _Bk->rom_len) {
            FIL *f = malloc(sizeof *f);
            uint8_t *b = malloc(4096), *b2 = malloc(4096);
            uint32_t sz = 0, got = 0;
            if (f && b && b2 && Sd_ReadOpen(f, path, &sz) == 0) {
                same = 1;
                for (uint32_t o = 0; same && o < sz; o += 4096u) {
                    if (Sd_Read(f, o, b, 4096, &got) || !got || stage_read(NULL, 0, o, b2, got) || memcmp(b, b2, got)) same = 0;
                }
                Sd_Close(f);
            }
            free(f); free(b); free(b2);
        }
        if (!same && Sd_FreeName(SDC_GAMES_DIR, file, path, sizeof path)) { snprintf(res, n, "NOT  BACKED  UP|CARD  WRITE  FAILED"); goto out; }
        if (pct) pct(p0 + (p1 - p0) * 85 / 100);
        if (!same && stage_to_card(0, _Bk->rom_len, path, &crc)) { snprintf(res, n, "NOT  BACKED  UP|CARD  WRITE  FAILED"); goto out; }
        if (hd.battery && hd.sav_size && hd.sav_size <= CF_RAM_SIZE && sdc_save_path(path, sp, sizeof sp) == 0) {
            if (pct) pct(p0 + (p1 - p0) * 95 / 100);
            if (stage_to_card(1, hd.sav_size, sp, NULL)) { snprintf(res, n, "GAME  BACKED  UP|ITS  SAVE  WAS  NOT"); goto out; }
        }
        printf("cf: card backup: %s (%lu bytes%s, crc32 %08lx)%s in %lu ms\n", path, (unsigned long)_Bk->rom_len, same ? ", already there" : "", (unsigned long)crc,
               hd.battery && hd.sav_size ? " and its save" : "", (unsigned long)(now_ms() - _Bk->t0));
        snprintf(res, n, "CART  BACKED  UP|%s", hd.battery && hd.sav_size ? "GAME  +  SAVE  ON  CARD" : "THE  GAME  IS  ON  CARD");
        rc = 0;
    }
out:
    bk_end();                                   /* (the staged copy erased) */
    return rc;
}

/* ---- lane share1: the cartridge shared with another Chromatic. Read into the staging area as the card backup reads it
 * (the ROM, then its 32 KiB save RAM; nothing is written to the cart), then sent from there; the staged copy is erased at
 * the end. 0 = read: *rom_len (from the header, else the last byte that is not FF), *sav_len (the header's save, 0 none),
 * the header's title / type / RAM code, the ROM's crc32. */
int CfDev_ShareCartBegin(void (*pct)(int), uint32_t *rom_len, uint32_t *sav_len, char title[17], uint8_t *cart_type, uint8_t *cart_ram, uint32_t *rom_crc, int *cgb)
{
    (void)CfRc_SlotBack();
    if (!cart_in_slot()) return -2;
    if (!jobs_idle()) return -3;
    if (!(_Bk = calloc(1, sizeof *_Bk))) return -4;
    _Bk->sock = -1; _Bk->ps_saved = -1; _Bk->st = BK_ROM; _Bk->t0 = now_ms();
    if (_DlP) dl_cleanup();
    if (wait_erase(30000)) goto fail;
    stage_erase(NULL);
    if (wait_erase(60000)) goto fail;
    /* the first 32 KiB, then the rest of the ROM the header says (a valid header: the card backup reads the whole 2 MiB chip,
       66 s for a 256 KiB game; this reads the 256 KiB), then the save RAM. A header that does not check: the whole chip */
    uint32_t rest = 0;
    for (int step = 0; step < 3; step++) {
        cf_job_t j;
        memset(&j, 0, sizeof j);
        const int what = step == 2;
        j.job = CF_JOB_USBREAD; j.rd_what = (uint8_t)what; j.rd_skip = !what; j.max_retries = 2;
        j.rd_off = step == 1 ? 0x8000u : 0; j.rd_len = step == 0 ? 0x8000u : step == 1 ? rest : CF_RAM_SIZE;
        if (step == 1) {
            uint8_t h[0x50];
            uint8_t hc = 0;
            uint32_t n = 0;
            if (!stage_read(NULL, 0, 0x100, h, sizeof h)) {
                for (int i = 0x34; i < 0x4D; i++) hc = (uint8_t)(hc - h[i] - 1u);
                n = (hc == h[0x4D] && h[0x48] <= 8) ? 0x8000u << h[0x48] : 0;
            }
            rest = (n && n <= CF_STAGE_ROM_MAX) ? n - 0x8000u : CF_STAGE_ROM_MAX - 0x8000u;
            j.rd_len = rest;
            if (!rest) continue;
        }
        _Bk->sink = 1; _Bk->err = 0;
        _Cf.st.result = 0xEE;
        int a = cf_esp_arm(&_Cf, &j);
        for (int t = 0; a == CF_D_ABORTED && t < 50; t++) { vTaskDelay(pdMS_TO_TICKS(200)); a = cf_esp_arm(&_Cf, &j); }
        if (a) { _Bk->sink = 0; goto fail; }
        if (wait_jobs(pct, step == 0 ? 0 : step == 1 ? 10 : 90, step == 0 ? 10 : step == 1 ? 90 : 100, 300000)) { _Bk->sink = 0; goto fail; }
        _Bk->sink = 0;
        if (_Cf.st.result != CF_D_OK || _Bk->err) { printf("share: the cart read failed (result %u)\n", _Cf.st.result); goto fail; }
    }
    bk_len();
    {
        uint8_t h[0x150];
        sdc_hdr_t hd;
        static const uint8_t logo[8] = { 0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B };
        if (stage_read(NULL, 0, 0, h, sizeof h) || memcmp(h + 0x104, logo, 8)) { printf("share: no game on the cart\n"); goto fail; }
        (void)sdc_hdr_parse(h, _Bk->rom_len, &hd);
        snprintf(title, 17, "%s", hd.title[0] ? hd.title : "CART");
        *cart_type = h[0x147]; *cart_ram = h[0x149];
        *cgb = (hd.cgb & 0x80) != 0;
        *rom_len = _Bk->rom_len;
        *sav_len = (hd.battery && hd.sav_size && hd.sav_size <= CF_RAM_SIZE) ? hd.sav_size : 0;
        uint8_t *b = malloc(4096);
        uint32_t c = 0;
        if (!b) goto fail;
        for (uint32_t o = 0; o < _Bk->rom_len; o += 4096u) {
            const uint32_t k = _Bk->rom_len - o < 4096u ? _Bk->rom_len - o : 4096u;
            if (stage_read(NULL, 0, o, b, k)) { free(b); goto fail; }
            c = cf_crc32(c, b, k);
        }
        free(b);
        *rom_crc = c;
        printf("share: the cart read: %s, %lu bytes (crc32 %08lx), save %lu, in %lu ms\n", title, (unsigned long)*rom_len, (unsigned long)c,
               (unsigned long)*sav_len, (unsigned long)(now_ms() - _Bk->t0));
    }
    return 0;
fail:
    bk_end();
    return -1;
}
int CfDev_ShareCartRead(int sav, uint32_t off, uint8_t *b, uint32_t n) { return _Bk ? stage_read(NULL, sav, off, b, n) : -1; }
void CfDev_ShareCartEnd(void) { if (_Bk) bk_end(); }

/* the game in the staging area (size, crc32, sha256 of what was staged) written to the cartridge by the USB install's own route
   (staged and checked again from the flash, then the stream job; the cart's save RAM left alone). The staging area is erased
   after. 0 = written */
static int staged_to_cart(uint32_t size, uint32_t crc, const uint8_t d[32], void (*pct)(int), int p0, int p1, char *res, size_t n)
{
    char size_s[12], crc_s[12], sha_s[65];
    int rc = -1;
    snprintf(size_s, sizeof size_s, "%lu", (unsigned long)size);
    snprintf(crc_s, sizeof crc_s, "%08lx", (unsigned long)crc);
    for (int i = 0; i < 32; i++) snprintf(sha_s + 2 * i, 3, "%02x", d[i]);
    char *av1[] = { "stage", "rom", size_s, crc_s, sha_s };
    char *av2[] = { "install", "fast", "nosav" };
    cf_usb_hold(&_Usb);
    (void)cf_usb_cmd(&_Usb, 5, av1);
    if (!_Usb.st_rom) { snprintf(res, n, "NOT  WRITTEN|TRY  AGAIN"); goto out; }
    _Cf.st.result = 0xEE;
    (void)cf_usb_cmd(&_Usb, 3, av2);
    if (!_Cf.armed) { snprintf(res, n, "NOT  WRITTEN|CART  NOT  READY"); goto out; }
    if (wait_jobs(pct, p0, p1, 600000)) { snprintf(res, n, "NOT  WRITTEN|TRY  AGAIN"); goto out; }
    if (_Cf.st.result != CF_D_OK) { printf("cf: to cart: the write ended %u\n", _Cf.st.result); snprintf(res, n, "NOT  WRITTEN|THE  WRITE  STOPPED"); goto out; }
    cart_title_from_stage();
    snprintf(res, n, "CART  WRITTEN|ITS  SAVE  WAS  KEPT");
    rc = 0;
out:
    _Usb.st_rom = 0; _Usb.st_sav = 0;
    stage_erase(NULL);
    return rc;
}
static int card_to_cart2(const char *path, void (*pct)(int), int p0, int p1, char *res, size_t n);
static int card_to_cart(const char *path, void (*pct)(int), char *res, size_t n) { return card_to_cart2(path, pct, 0, 100, res, n); }
static int card_to_cart2(const char *path, void (*pct)(int), int p0, int p1, char *res, size_t n)
{
    (void)CfRc_SlotBack();
    if (!cart_in_slot()) { snprintf(res, n, "NOT  WRITTEN|NO  CART  IN  THE  SLOT"); return -1; }
    if (!jobs_idle()) { snprintf(res, n, "NOT  WRITTEN|CHROMATIC  IS  BUSY"); return -1; }
    if (stage_room(NULL)) { snprintf(res, n, "NOT  WRITTEN|"); return -1; }
    FIL *f = malloc(sizeof *f);
    uint8_t *b = malloc(4096);
    uint32_t size = 0, crc = 0, got = 0;
    int rc = -1;
    mbedtls_sha256_context h;
    mbedtls_sha256_init(&h);
    (void)mbedtls_sha256_starts(&h, 0);
    if (!f || !b || Sd_Mount() != 0 || Sd_ReadOpen(f, path, &size) != 0) { snprintf(res, n, "NOT  WRITTEN|CANNOT  READ  IT"); goto out0; }
    if (size < 0x8000u || size > CF_INST_WRITABLE) { Sd_Close(f); snprintf(res, n, "NOT  WRITTEN|TOO  BIG  FOR  A  CART"); goto out0; }
    if (_DlP) dl_cleanup();
    if (wait_erase(30000)) { Sd_Close(f); snprintf(res, n, "NOT  WRITTEN|TRY  AGAIN"); goto out0; }
    stage_erase(NULL);
    if (wait_erase(60000)) { Sd_Close(f); snprintf(res, n, "NOT  WRITTEN|TRY  AGAIN"); goto out0; }
    for (uint32_t o = 0; o < size; o += got) {
        if (Sd_Read(f, o, b, 4096, &got) || !got || stage_write_nonff(CF_STAGE_ROM_OFF + o, b, got)) { Sd_Close(f); snprintf(res, n, "NOT  WRITTEN|TRY  AGAIN"); goto out1; }
        crc = cf_crc32(crc, b, got);
        (void)mbedtls_sha256_update(&h, b, got);
        if (pct) pct(p0 + (int)((uint64_t)o * (uint32_t)(p1 - p0) / 5u / size));
    }
    Sd_Close(f);
    {
        uint8_t d[32];
        (void)mbedtls_sha256_finish(&h, d);
        rc = staged_to_cart(size, crc, d, pct, p0 + (p1 - p0) / 5, p1, res, n);
        if (!rc) printf("cf: card to cart: %s (%lu bytes, crc32 %08lx) written to the cart\n", path, (unsigned long)size, (unsigned long)crc);
    }
    goto out0;
out1:
    _Usb.st_rom = 0; _Usb.st_sav = 0;
    stage_erase(NULL);
out0:
    mbedtls_sha256_free(&h);
    free(f); free(b);
    return rc;
}
/* ---- lane ui110dev: SYSTEM > GAMES's jobs between the places (sd.c kSdOp_Job: on the card's task, one at a time; the
 * library's parts on the RAM cart task, asked and waited for: cf_rc_dev.c CfRc_LibCopy / CfRc_LibKeepStaged). Each one:
 * a game playing with no cartridge stops first (its save kept), the link gives its heap back (cf_usb_hold, as a USB cart job:
 * back 30 s after), the staging area is the go-between and is erased at the end. Words on the page: DESIGN.md.
 *   backup     the cartridge -> /GAMES + /SAVES (card_backup)
 *   card2cart  a card game -> the cartridge; kSdJf_BackupFirst: the cartridge to the card first (a blank cartridge has none)
 *   lib2cart   a kept game unpacked into the staging area -> the cartridge (the same backup first)
 *   card2lib   a card game PACKED here (cf_pack.c, SGZ1) into the staging area -> kept (the Wi-Fi keep's own path: the
 *              container checked whole, copied, every extent read back), then its save from /SAVES
 *   cart2lib   the cartridge read into the staging area -> packed IN ORDER straight into the library's stream (cf_pack_seq:
 *              no room for a container beside the game in the staging area), kept as it is when that does not work; its save
 *   lib2card   a kept game unpacked onto the card as /GAMES/<name>.gb(c), its save as /SAVES/<name>.sav */
int CfDev_StageWrite(uint32_t off, const uint8_t *b, uint32_t n)
{
    if ((uint64_t)off + n > CF_STAGE_ROM_MAX) return -1;
    return stage_write_nonff(CF_STAGE_ROM_OFF + off, b, n);
}
static void (*_GjPct)(int);
static int _GjP0, _GjP1;
/* the job's link hold (cf_usb_hold) is let go 30 s after the last cart command: a GAMES job keeps it while it works */
static void gj_keep(void) { if (_Usb.held) _Usb.last_cmd_t = now_ms(); }
static void gj_sub(int p) { gj_keep(); if (_GjPct) _GjPct(_GjP0 + (_GjP1 - _GjP0) * (p < 0 ? 0 : p > 100 ? 100 : p) / 100); }
/* a library op asked of the RAM cart task: waited for (its percent onto the page); its result */
static int gj_lib_wait(uint32_t ms)
{
    const uint32_t t0 = now_ms();
    vTaskDelay(pdMS_TO_TICKS(50));
    while (CfRc_LibBusy() && now_ms() - t0 < ms) { gj_sub(CfRc_LibCopyPct()); vTaskDelay(pdMS_TO_TICKS(200)); }
    gj_keep();
    return CfRc_LibBusy() ? -100 : CfRc_LibResult();
}
static int _GjNetHeld;
static int gj_ready(const char *no, char *res, size_t n)
{
    if (CfRc_Playing()) {
        /* a game playing with no cartridge: stopped (its save copied) */
        printf("cf: games job: the game playing stops first\n");
        CfRc_Stop();
        for (int t = 0; t < 100 && CfRc_Playing(); t++) vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!jobs_idle() || CfRc_LibBusy()) { snprintf(res, n, "%s|CHROMATIC  IS  BUSY", no); return -1; }
    if (stage_room(NULL)) { snprintf(res, n, "%s|", no); return -1; }
    cf_usb_hold(&_Usb);                                   /* (the link's heap back: the unpack / pack needs it) */
    if (!NW_RecvHeld()) { NW_RecvHold(true); _GjNetHeld = 1; }   /* (the network window too: its idle TLS connection closed) */
    CfRc_Trim();
    return 0;
}
static int gj_erase(const char *no, char *res, size_t n)
{
    gj_keep();
    if (_DlP) dl_cleanup();
    if (wait_erase(30000)) { snprintf(res, n, "%s|TRY  AGAIN", no); return -1; }
    stage_erase(NULL);
    if (wait_erase(60000)) { snprintf(res, n, "%s|TRY  AGAIN", no); return -1; }
    return 0;
}
/* the cartridge backed up to the card before a write (kSdJf_BackupFirst): 0 = backed up (or nothing on it to back up) */
static int gj_backup_first(int *did, char *res, size_t n)
{
    *did = 0;
    Sd_JobSetStep("BACKING  UP  FIRST");
    char r[48] = "";
    const int e = card_backup2(_GjPct, 0, 40, r, sizeof r);
    if (e == -2) { printf("cf: games job: the cartridge has no game to back up\n"); return 0; }
    if (e) { snprintf(res, n, "NOT  WRITTEN|THE  BACKUP  FAILED"); printf("cf: games job: the backup first failed (%s): nothing written\n", r); return -1; }
    *did = 1;
    /* m110: the backup's staged copy is erased in the background (bk_end); the write after it must wait for that, or
       card_to_cart2's jobs_idle() says THE UNIT IS BUSY (unit 1, 2026-10-24: card -> cart refused right after its backup) */
    if (wait_erase(60000)) { snprintf(res, n, "NOT  WRITTEN|TRY  AGAIN"); printf("cf: games job: the backup's staged copy was not erased in time\n"); return -1; }
    return 0;
}
static void gj_pack_pct(void *u, int p) { (void)u; gj_sub(p); }
static int gj_card_to_lib(const char *path, const char *title, char *res, size_t n);
/* the library's stream for a container packed in order (cf_pack_seq): planned when its length is known */
typedef struct { cf_lib_tx_t tx; const char *title; int plan; uint8_t *cb; uint32_t cn; } gj_tx_t;
#define GJ_TX_CHUNK 2048u   /* the library's flash takes its writes in pieces this big (each is FPGA ops + a status: 256 B a piece was 0.1 s) */
static int gj_stage_rd(void *u, uint32_t off, uint8_t *b, uint32_t n) { (void)u; return stage_read_mm(NULL, 0, off, b, n); }
static int gj_tx_begin(void *u, uint32_t total)
{
    gj_tx_t *g = u;
    g->plan = CfRc_LibPlanStream(total, g->title, &g->tx);
    if (g->plan == 0) g->plan = -999;                       /* (planned) */
    if (g->plan == -999 && !g->cb && !(g->cb = malloc(GJ_TX_CHUNK))) return -1;
    g->cn = 0;
    return g->plan == -999 ? 0 : -1;
}
static int gj_tx_flush(gj_tx_t *g)
{
    if (!g->cn) return 0;
    const int e = CfRc_LibTxWrite(&g->tx, g->cb, g->cn);
    g->cn = 0;
    return e ? -1 : 0;
}
static int gj_tx_wr(void *u, const uint8_t *b, uint32_t n)
{
    gj_tx_t *g = u;
    gj_keep();
    while (n) {
        const uint32_t k = GJ_TX_CHUNK - g->cn < n ? GJ_TX_CHUNK - g->cn : n;
        memcpy(g->cb + g->cn, b, k);
        g->cn += k; b += k; n -= k;
        if (g->cn == GJ_TX_CHUNK && gj_tx_flush(g)) return -1;
    }
    return 0;
}
/* a card game kept ON CHROMATIC: packed here in order straight into the library's stream (cf_pack_seq; no staging area, so
   no 1.94 MB cap: a 4 MiB game whose container fits the library is kept), kept as it is when it does not get shorter; its save
   from /SAVES after */
typedef struct { gj_tx_t g; FIL *f; uint32_t size; } gj_card_t;
static int gj_cardf_rd(void *u, uint32_t off, uint8_t *b, uint32_t n)
{
    gj_card_t *c = u;
    uint32_t got = 0;
    return Sd_Read(c->f, off, b, n, &got) || got != n ? -1 : 0;
}
static int gj_cardf_begin(void *u, uint32_t total) { return gj_tx_begin(&((gj_card_t *)u)->g, total); }
static int gj_cardf_wr(void *u, const uint8_t *b, uint32_t n) { return gj_tx_wr(&((gj_card_t *)u)->g, b, n); }
static int gj_card_to_lib(const char *path, const char *title, char *res, size_t n)
{
    static const char no[] = "NOT  KEPT";
    if (gj_ready(no, res, n)) return -1;
    gj_card_t *c = calloc(1, sizeof *c);
    uint8_t *h = malloc(0x150);
    if (c) c->f = malloc(sizeof *c->f);
    uint32_t got = 0;
    sdc_hdr_t hd;
    if (!c || !c->f || !h || Sd_Mount() != 0 || Sd_ReadOpen(c->f, path, &c->size) != 0) {
        if (c) free(c->f);
        free(c); free(h);
        snprintf(res, n, "%s|CANNOT  READ  IT", no);
        return -1;
    }
    const int hr = c->size >= 0x150u && Sd_Read(c->f, 0, h, 0x150, &got) == 0 && got == 0x150 ? sdc_hdr_parse(h, c->size, &hd) : -1;
    free(h);
    if (hr) { Sd_Close(c->f); free(c->f); free(c); snprintf(res, n, "%s|IT  CANNOT  PLAY  HERE", no); return -1; }
    c->g.title = title && title[0] ? title : hd.title;
    Sd_JobSetStep("PACKING  IT");
    _GjP0 = 0; _GjP1 = 92;
    uint32_t total = 0;
    const int64_t t0 = esp_timer_get_time();
    const uint32_t fh = esp_get_free_heap_size(), big = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    int e = CF_PACK_E_MEM;
    for (int t = 0; t < 8 && e == CF_PACK_E_MEM; t++) {   /* (the heap in pieces right after the hold: a moment, and again) */
        if (t) vTaskDelay(pdMS_TO_TICKS(500));
        /* (over 2 MiB: 8 KiB blocks, the table half as long: a 4 MiB game's 4 KiB table did not fit the heap; the unit plays both) */
        e = cf_pack_seq(gj_cardf_rd, gj_cardf_begin, gj_cardf_wr, c, c->size, c->size > 0x200000u ? 13 : 12, 0, &total, gj_pack_pct);
    }
    printf("cf: games job: %s packed into the library: e=%d, %lu -> %lu bytes in %lld ms (blocks dyn %lu fix %lu raw %lu, chains %lu; plan %d; free heap before %lu, largest %lu)\n",
           path, e, (unsigned long)c->size, (unsigned long)total, (long long)((esp_timer_get_time() - t0) / 1000), (unsigned long)cf_pack_prof.dyn,
           (unsigned long)cf_pack_prof.fix, (unsigned long)cf_pack_prof.raw, (unsigned long)cf_pack_prof.chains, c->g.plan, (unsigned long)fh, (unsigned long)big);
    if (e == CF_PACK_E_NOGAIN || e == CF_PACK_E_MEM) {
        /* it does not get shorter (or no memory to pack): kept as it is, streamed from the card */
        memset(&c->g.tx, 0, sizeof c->g.tx);
        c->g.plan = 0;
        e = gj_cardf_begin(c, c->size) ? CF_PACK_E_WRITE : 0;
        uint8_t *b = e ? NULL : malloc(2048);
        if (!e && !b) e = CF_PACK_E_MEM;
        for (uint32_t o = 0; !e && o < c->size; o += 2048u) {
            const uint32_t k = c->size - o < 2048u ? c->size - o : 2048u;
            e = gj_cardf_rd(c, o, b, k) || gj_cardf_wr(c, b, k) ? CF_PACK_E_WRITE : 0;
            gj_sub((int)((uint64_t)o * 100u / c->size));
        }
        free(b);
        total = c->size;
    }
    Sd_Close(c->f);
    free(c->f); c->f = NULL;
    if (!e && gj_tx_flush(&c->g)) e = CF_PACK_E_WRITE;
    free(c->g.cb); c->g.cb = NULL;
    int le = e ? (c->g.plan && c->g.plan != -999 ? c->g.plan : -100) : 0;
    if (!e) {
        Sd_JobSetStep("KEEPING  IT");
        _GjP0 = 92; _GjP1 = 96;
        le = CfRc_LibStreamEnd(&c->g.tx) ? -100 : gj_lib_wait(600000);
    }
    const uint32_t uid = CfRc_LibUid();
    free(c);
    if (le) { snprintf(res, n, "%s|%s", no, le == CF_LIB_E_FULL ? "NOT  ENOUGH  ROOM|DELETE  ONE  FIRST" : le == CF_LIB_E_MANY ? "16  GAMES  KEPT|DELETE  ONE  FIRST" : le == CF_LIB_E_ARG ? "IT  CANNOT  PLAY  HERE" : "TRY  AGAIN"); return -1; }
    char sp[SDC_PATH_MAX + 8];
    if (hd.battery && hd.sav_size && sdc_save_path(path, sp, sizeof sp) == 0 && Sd_Exists(sp)) {
        Sd_JobSetStep("ITS  SAVE");
        _GjP0 = 96; _GjP1 = 100;
        if (CfRc_LibCopy(2, uid, sp, 0) == 0 && gj_lib_wait(60000) != 0) { snprintf(res, n, "GAME  KEPT|ITS  SAVE  WAS  NOT"); return -2; }
    }
    snprintf(res, n, "KEPT  ON  CHROMATIC|IT  USES  %lu  KB", (unsigned long)(total / 1024u));
    return 0;
}
static int gj_cart_to_lib(char *res, size_t n)
{
    static const char no[] = "NOT  KEPT";
    (void)CfRc_SlotBack();
    if (!cart_in_slot()) { snprintf(res, n, "%s|NO  CART  IN  THE  SLOT", no); return -1; }
    if (gj_ready(no, res, n)) return -1;
    if (!(_Bk = calloc(1, sizeof *_Bk))) { snprintf(res, n, "%s|NOT  ENOUGH  MEMORY", no); return -1; }
    _Bk->sock = -1; _Bk->ps_saved = -1; _Bk->st = BK_ROM; _Bk->t0 = now_ms();
    int rc = -1;
    Sd_JobSetStep("READING  THE  CART");
    if (cart_read_stage(_GjPct, 0, 60, no, res, n)) goto out;
    {
        static const uint8_t logo[8] = { 0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B };
        uint8_t h[0x150];
        sdc_hdr_t hd;
        if (stage_read(NULL, 0, 0, h, sizeof h) || memcmp(h + 0x104, logo, 8)) { snprintf(res, n, "%s|NO  GAME  ON  THE  CART", no); goto out; }
        if (sdc_hdr_parse(h, _Bk->rom_len, &hd)) { snprintf(res, n, "%s|IT  CANNOT  PLAY  HERE", no); goto out; }
        char t[24];
        snprintf(t, sizeof t, "%s", hd.title[0] ? hd.title : "CART");
        for (char *q = t; *q; q++) if (*q == '_') *q = ' ';
        /* packed straight into the library's stream (the staging area holds the cartridge's game: no room for a container
           beside it), in order: cf_pack_seq; kept as it is when it does not get shorter (or the pack has no memory) */
        Sd_JobSetStep("PACKING  IT");
        _GjP0 = 60; _GjP1 = 92;
        gj_tx_t *g = calloc(1, sizeof *g);
        int pe = g ? 0 : CF_PACK_E_MEM;
        uint32_t total = 0;
        if (g) {
            g->title = t;
            mw_drop();
            const int64_t t0 = esp_timer_get_time();
            pe = CF_PACK_E_MEM;
            for (int tt = 0; tt < 8 && pe == CF_PACK_E_MEM; tt++) {
                if (tt) vTaskDelay(pdMS_TO_TICKS(500));
                pe = cf_pack_seq(gj_stage_rd, gj_tx_begin, gj_tx_wr, g, _Bk->rom_len, 12, 0, &total, gj_pack_pct);
            }
            printf("cf: games job: the cartridge's %s packed into the library: e=%d, %lu -> %lu bytes in %lld ms (plan %d)\n", t, pe, (unsigned long)_Bk->rom_len,
                   (unsigned long)total, (long long)((esp_timer_get_time() - t0) / 1000), g->plan);
        }
        if (!pe && g && gj_tx_flush(g)) pe = CF_PACK_E_WRITE;
        if (g) { free(g->cb); g->cb = NULL; }
        int le;
        if (!pe) {
            Sd_JobSetStep("KEEPING  IT");
            _GjP0 = 92; _GjP1 = 95;
            le = CfRc_LibStreamEnd(&g->tx) ? -100 : gj_lib_wait(600000);
        } else if (g && g->plan && g->plan != -999) le = g->plan;   /* (no room: the plan said so) */
        else {
            Sd_JobSetStep("KEEPING  IT");
            _GjP0 = 60; _GjP1 = 95;
            le = CfRc_LibKeepStaged(CF_STAGE_ROM_OFF, _Bk->rom_len, 0, t, 0) ? -100 : gj_lib_wait(600000);
        }
        free(g);
        if (le) { snprintf(res, n, "%s|%s", no, le == CF_LIB_E_FULL ? "NOT  ENOUGH  ROOM|DELETE  ONE  FIRST" : le == CF_LIB_E_MANY ? "16  GAMES  KEPT|DELETE  ONE  FIRST" : "TRY  AGAIN"); goto out; }
        const uint32_t uid = CfRc_LibUid();
        if (hd.battery && hd.sav_size && hd.sav_size <= CF_RAM_SIZE) {
            Sd_JobSetStep("ITS  SAVE");
            _GjP0 = 95; _GjP1 = 100;
            if (CfRc_LibCopy(3, uid, NULL, CF_STAGE_SAV_OFF) == 0 && gj_lib_wait(60000) != 0) { snprintf(res, n, "GAME  KEPT|ITS  SAVE  WAS  NOT"); rc = -2; goto out; }
        }
        printf("cf: games job: the cartridge's %s (%lu bytes%s) is kept on the Chromatic (uid %lu)\n", t, (unsigned long)_Bk->rom_len, hd.battery ? " and its save" : "", (unsigned long)uid);
        snprintf(res, n, "KEPT  ON  CHROMATIC|%s", hd.battery && hd.sav_size ? "GAME  +  SAVE" : "");
        rc = 0;
    }
out:
    bk_end();                                   /* (the staged copy erased) */
    return rc;
}
static int gj_lib_to_card(uint32_t uid, char *res, size_t n)
{
    static const char no[] = "NOT  COPIED";
    if (Sd_Mount() != 0 || !Sd_Writable()) { snprintf(res, n, "%s|CARD  NOT  READY", no); return -1; }
    if (gj_ready(no, res, n)) return -1;
    Sd_JobSetStep("COPYING");
    _GjP0 = 0; _GjP1 = 100;
    if (CfRc_LibCopy(0, uid, NULL, 0)) { snprintf(res, n, "%s|CHROMATIC  IS  BUSY", no); return -1; }
    const int e = gj_lib_wait(900000);
    if (e) { snprintf(res, n, "%s", CfRc_LibCopyWhy() ? CfRc_LibCopyWhy() : "NOT  COPIED|TRY  AGAIN"); return e == -2 ? -2 : -1; }
    (void)SdLib_Refresh(0);
    snprintf(res, n, "COPIED  TO  THE  CARD|");
    return 0;
}

/* ---- lane savecloud1: SAVE TO MY GAMES (SYSTEM > GAMES > a game kept ON CHROMATIC). cf_sup.c decides (host-tested); this is
 * the connection, the flash read and the job. The job runs on the card's task (kSdJob_SaveToCloud, as the other GAMES jobs: its
 * percent and words reach the page, B hides it), the link is stopped first as receive mode stops it (ci_hold: the handshake needs
 * ~21 KB) and comes back on EVERY path out (one exit), and the upload itself is stepped on the nw HTTP task (CfDev_HttpWork:
 * the task that has the TLS stack; the card's and the RAM cart's are 4 KB). The save is read by its extents
 * (CfRc_LibShareRead, as SHARE does: not through the open library, which is closed meanwhile for its ~5.5 KB) and is never
 * written: a failure cannot change the stored save. */
#define UP_URL_MAX   256
#define UP_TLS_FREE  21000u        /* the link task's own gate (cwl_online.c kTlsMinHeap) */
#define UP_TLS_BLOCK 8192u
static struct {
    volatile int on, done, stop;
    cf_sup_t *s;
    cf_lib_ent_t e;
    cf_lib_save_t sv;
    esp_http_client_handle_t cli;
    uint32_t next_open, heap_waits, opens;
} _Up;
/* what goes up is the save and then, for a clock game, its clock: the same 48 bytes the card copy has */
static uint8_t _UpFt[SDC_RTC_FOOTER];
static uint32_t _UpFtN;
static int up_read(void *u, uint32_t off, uint8_t *b, uint32_t n)
{
    (void)u;
    const uint32_t ss = _Up.sv.size;
    if (off < ss) {
        const uint32_t k = ss - off < n ? ss - off : n;
        if (CfRc_LibShareRead(&_Up.e, &_Up.sv, 1, off, b, k)) return -1;
        off += k; b += k; n -= k;
    }
    if (!n) return 0;
    if (off - ss + n > _UpFtN) return -1;
    memcpy(b, _UpFt + (off - ss), n);
    return 0;
}
static void up_footer(void)
{
    _UpFtN = 0;
    if (!(_Up.e.flags & CF_LIB_F_RTC)) return;
    /* when this clock was true, from the save's own record: the same save has the same bytes after a restart too */
    sdc_rtc_put(_UpFt, _Up.sv.rtc, sdc_tsec_unpack(_Up.sv.tsec, CwlTime_Anchor()));
    _UpFtN = SDC_RTC_FOOTER;
}
static void up_close(void *u)
{
    (void)u;
    if (_Up.cli) { esp_http_client_close(_Up.cli); esp_http_client_cleanup(_Up.cli); _Up.cli = NULL; }
}
static int up_open(void *u, const char *target, uint32_t body_len)
{
    (void)u;
    if ((int32_t)(now_ms() - _Up.next_open) < 0) return 1;
    if (!CWL_IsOnline()) { CWL_SetNetworkWanted(true); CWL_NetworkUse(false, true); _Up.next_open = now_ms() + 500u; return 1; }
    const char *api = CwlOnline_Api();
    if (!api || !CwlSec_ApiUrlOk(api)) return -1;        /* (the pairing token goes only to an https API) */
    CfRc_Trim();                                          /* the check pass's 1 KiB read window: the send pass takes it again */
    const uint32_t fr = esp_get_free_heap_size(), big = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    if (fr < UP_TLS_FREE || big < UP_TLS_BLOCK) {
        if ((_Up.heap_waits++ & 15u) == 0) printf("cf: save to my games: waiting for memory (free %lu, largest %lu)\n", (unsigned long)fr, (unsigned long)big);
        _Up.next_open = now_ms() + 250u;
        return 1;
    }
    char *url = malloc(UP_URL_MAX);
    char *auth = malloc(800);
    if (!url || !auth) { free(url); free(auth); _Up.next_open = now_ms() + 250u; return 1; }
    snprintf(url, UP_URL_MAX, "%s%s", api, target);
    memcpy(auth, "Bearer ", 7);
    const int have = CwlOnline_CopyToken(auth + 7, 800 - 7) > 0;
    esp_http_client_config_t cfg = { .url = url, .timeout_ms = 10000, .buffer_size = 1024, .buffer_size_tx = 1536, .keep_alive_enable = false, .method = HTTP_METHOD_POST };
    cfg.crt_bundle_attach = CwlTls_Attach;
    _Up.cli = have ? esp_http_client_init(&cfg) : NULL;
    free(url);
    int r = -1;
    if (_Up.cli && esp_http_client_set_header(_Up.cli, "Authorization", auth) == ESP_OK &&
        esp_http_client_set_header(_Up.cli, "Content-Type", "application/octet-stream") == ESP_OK &&
        esp_http_client_set_header(_Up.cli, "User-Agent", "ShinyChromatic-save/1") == ESP_OK) {
        memset(auth, 0, 800);                               /* (the client keeps its own copy: these 800 bytes go before the handshake) */
        free(auth);
        auth = NULL;
        _Up.opens++;
        const int wdt = CwlWdt_LongBegin();                /* the handshake: seconds without a yield */
        const esp_err_t e = esp_http_client_open(_Up.cli, (int)body_len);
        CwlWdt_LongEnd(wdt);
        r = e == ESP_OK ? 0 : -1;
        if (r) printf("cf: save to my games: could not connect (%s), free heap %lu\n", esp_err_to_name(e), (unsigned long)esp_get_free_heap_size());
    }
    if (auth) { memset(auth, 0, 800); free(auth); }         /* (the token is never kept or printed) */
    if (r) { up_close(NULL); _Up.next_open = now_ms() + 1500u; }
    return r;
}
static int up_write(void *u, const uint8_t *b, uint32_t n) { (void)u; return _Up.cli && esp_http_client_write(_Up.cli, (const char *)b, (int)n) == (int)n ? 0 : -1; }
static int up_answer(void *u, int *http, char *out, uint32_t cap)
{
    (void)u;
    *http = 0;
    if (!_Up.cli) return -1;
    const int64_t h = esp_http_client_fetch_headers(_Up.cli);
    *http = esp_http_client_get_status_code(_Up.cli);
    if (h < 0 || *http <= 0) return -1;
    uint32_t n = 0;
    while (n + 1 < cap) {
        const int r = esp_http_client_read(_Up.cli, out + n, (int)(cap - 1 - n));
        if (r <= 0) break;
        n += (uint32_t)r;
    }
    out[n] = 0;
    return 0;
}
static uint32_t up_now(void *u) { (void)u; return now_ms(); }
static int up_stop(void *u) { (void)u; return _Up.stop; }
static const cf_sup_hooks_t _UpHooks = { NULL, up_read, up_open, up_write, up_answer, up_close, up_now, up_stop };
/* the nw HTTP task: one step of the upload; 1 = call again soon */
static int up_work(void)
{
    if (!_Up.on) return 0;
    if (!_Up.s) { _Up.on = 0; return 0; }
    const int more = cf_sup_step(_Up.s);
    if (!more) { up_close(NULL); _Up.on = 0; _Up.done = 1; }
    return more;
}
static int up_wifi_saved(void)
{
    char ssid[36];
    CWL_GetSsidText(ssid, sizeof ssid);
    return ssid[0] != 0;
}
static int gj_save_to_cloud(uint32_t uid, char *res, size_t n)
{
    int out = 0, held = 0;
    cf_sup_t *s = NULL;
    const uint32_t fh = esp_get_free_heap_size(), t0 = now_ms();
    out = cf_sup_gate(NW_Paired(), up_wifi_saved(), CfRc_Playing(), 1);
    if (!out && (!jobs_idle() || CfRc_LibBusy() || _Up.on)) out = CF_SUP_E_BUSY;
    /* the game's entry and its newest save record, as stored (a copy: nothing here writes the library) */
    if (!out && CfRc_LibShareOpen(uid, &_Up.e, &_Up.sv)) out = CF_SUP_E_BUSY;
    if (!out && (!_Up.e.save_size || !_Up.sv.seq || _Up.sv.size != _Up.e.save_size)) out = CF_SUP_E_NOSAVE;
    if (!out && !(s = malloc(sizeof *s))) out = CF_SUP_E_MEM;
    if (!out) {
        Sd_JobSetStep("SENDING  THE  SAVE");
        (void)CfRc_LibNetClose();                          /* its ~5.5 KB are the handshake's */
        if (!_CiHoldOn) { ci_hold(NULL, 1); held = 1; }        /* the link stops (receive mode's own hold), session or not: the hold is what lets the online task's memory go */
        for (int t = 0; t < 120 && !CWL_RecvHeld(); t++) vTaskDelay(pdMS_TO_TICKS(100));
        vTaskDelay(pdMS_TO_TICKS(1500));                       /* (its kept connection goes on its next turn) */
        NW_CfCloseGateway();
        up_footer();
        cf_sup_begin(s, &_UpHooks, _Up.e.rom_crc32, _Up.e.rom_size, _Up.sv.size + _UpFtN, _UpFtN ? cf_crc32(_Up.sv.crc, _UpFt, _UpFtN) : _Up.sv.crc, _Up.e.title);
        _Up.done = 0; _Up.stop = 0; _Up.next_open = 0; _Up.heap_waits = 0; _Up.opens = 0;
        _Up.s = s;
        _Up.on = 1;
        while (!_Up.done) {
            gj_sub(s->pct);
            gj_keep();
            if (!_Up.stop && now_ms() - t0 > 120000u) _Up.stop = 1;       /* (the step ends it at its next turn) */
            if (_Up.stop && now_ms() - t0 > 150000u) break;               /* (the HTTP task never came back: see below) */
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (_Up.done) out = s->res;
        else out = CF_SUP_E_STOP;
        printf("cf: save to my games: %lu bytes, outcome %d (http %d) in %lu ms; opens %lu, heap free %lu at the start, %lu now, lowest %lu\n",
               (unsigned long)(_Up.sv.size + _UpFtN), out, s->http, (unsigned long)(now_ms() - t0), (unsigned long)_Up.opens, (unsigned long)fh,
               (unsigned long)esp_get_free_heap_size(), (unsigned long)esp_get_minimum_free_heap_size());
        if (_Up.done) { _Up.s = NULL; free(s); }                          /* (if the task did not come back it still owns it: left, not freed) */
        else { _Up.stop = 1; printf("cf: save to my games: the HTTP task did not end the upload in time (left)\n"); }
        /* every way out: the link comes back, the page's library opens again */
        if (held) ci_hold(NULL, 0);
        if (CfRc_LibHeld()) CfRc_LibHold(1);
    }
    snprintf(res, n, "%s", cf_sup_words(out));
    if (out) printf("cf: save to my games: not saved (%d)\n", out);
    return out == CF_SUP_OK || out == CF_SUP_ALREADY ? 0 : -1;
}
static int gj_to_cart(int from_lib, const char *path, uint32_t uid, int flags, char *res, size_t n)
{
    static const char no[] = "NOT  WRITTEN";
    (void)CfRc_SlotBack();
    if (!cart_in_slot()) { snprintf(res, n, "%s|NO  CART  IN  THE  SLOT", no); return -1; }
    if (gj_ready(no, res, n)) return -1;
    int did = 0;
    if ((flags & kSdJf_BackupFirst) && gj_backup_first(&did, res, n)) return -1;
    const int p0 = (flags & kSdJf_BackupFirst) ? 40 : 0;
    Sd_JobSetStep("WRITING");
    int e;
    char r[48] = "";
    if (!from_lib) e = card_to_cart2(path, _GjPct, p0, 100, r, sizeof r);
    else {
        if (gj_erase(no, res, n)) return -1;
        _GjP0 = p0; _GjP1 = p0 + (100 - p0) / 4;
        if (CfRc_LibCopy(1, uid, NULL, 0)) { snprintf(res, n, "%s|CHROMATIC  IS  BUSY", no); stage_erase(NULL); return -1; }
        e = gj_lib_wait(600000);
        if (e) { snprintf(res, n, "%s", CfRc_LibCopyWhy() ? CfRc_LibCopyWhy() : "NOT  WRITTEN|TRY  AGAIN"); stage_erase(NULL); return -1; }
        uint32_t size = 0, crc = 0;
        uint8_t d[32];
        CfRc_LibCopyStaged(&size, &crc, d);
        mw_drop();
        e = staged_to_cart(size, crc, d, _GjPct, _GjP1, 100, r, sizeof r);
    }
    if (e) { snprintf(res, n, "%s", r); return -1; }
    snprintf(res, n, did ? "CARTRIDGE  WRITTEN|OLD  GAME  ON  CARD" : "CARTRIDGE  WRITTEN|ITS  SAVE  WAS  KEPT");
    return 0;
}
static int card_job(int kind, const char *path, uint32_t uid, int flags, const char *title, void (*pct)(int), char *res, size_t n)
{
    _GjPct = pct; _GjP0 = 0; _GjP1 = 100;
    const int64_t t0 = esp_timer_get_time();
    const uint32_t fh = esp_get_free_heap_size();
    int e;
    switch (kind) {
    case kSdJob_Backup:     e = card_backup(pct, res, n); break;
    case kSdJob_CardToCart: e = path ? gj_to_cart(0, path, 0, flags, res, n) : -1; break;
    case kSdJob_LibToCart:  e = gj_to_cart(1, NULL, uid, flags, res, n); break;
    case kSdJob_CardToLib:  e = path ? gj_card_to_lib(path, title, res, n) : -1; break;
    case kSdJob_CartToLib:  e = gj_cart_to_lib(res, n); break;
    case kSdJob_LibToCard:  e = gj_lib_to_card(uid, res, n); break;
    case kSdJob_SaveToCloud: e = gj_save_to_cloud(uid, res, n); break;
    default:                e = -1; snprintf(res, n, "NOT  DONE|"); break;
    }
    if (e && !res[0]) snprintf(res, n, "NOT  DONE|TRY  AGAIN");
    if (kind == kSdJob_Backup || kind == kSdJob_CardToCart || kind == kSdJob_LibToCart || kind == kSdJob_LibToCard) (void)SdLib_Refresh(0);   /* (new files on the card: in the list) */
    if (kind == kSdJob_Backup || kind == kSdJob_CardToCart || kind == kSdJob_LibToCart || kind == kSdJob_CartToLib) {
        /* the cartridge restarted (its game is back) and that closed the menu, but the ESP32 still takes the OSD as shown (it
           learns it only from the FPGA's button packets): told, then GAMES opened again (a MENU tap), with the result */
        extern void OSD_SetVisiblityState(const bool IsVisible);
        vTaskDelay(pdMS_TO_TICKS(2500));
        OSD_SetVisiblityState(false);
        if (NW_GgOwnsJob()) NW_GgOpenMenu(); else NW_PlayOpenMenu();   /* lane mygames1: GET GAMES's backup goes on there */
    }
    if (_GjNetHeld) { _GjNetHeld = 0; NW_RecvHold(false); }
    printf("cf: games job %d (%s) e=%d \"%s\" in %lld ms (free heap %lu at the start, %lu now, lowest %lu)\n", kind, path ? path : "", e, res,
           (long long)((esp_timer_get_time() - t0) / 1000), (unsigned long)fh, (unsigned long)esp_get_free_heap_size(), (unsigned long)esp_get_minimum_free_heap_size());
    _GjPct = NULL;
    return e;
}
static const sd_cart_hooks_t _SdCartHooks = { card_backup, card_to_cart, cart_in_slot, card_job };
#endif /* CONFIG_NW_CARTFLASH_WIFI */

void CfDev_Init(void)
{
    {   /* 1.1.0: the staged ROM goes in the app slot that is NOT running (cf_usb.h CF_STAGE_ROM_A / _B) */
        const esp_partition_t *run = esp_ota_get_running_partition();
        cf_stage_rom_off = (run && run->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1) ? CF_STAGE_ROM_B : CF_STAGE_ROM_A;
        printf("cart: running from %s at %06lx: the staged ROM goes to %06lx, the save to %06lx\n", run ? run->label : "?",
               (unsigned long)(run ? run->address : 0), (unsigned long)cf_stage_rom_off, (unsigned long)CF_STAGE_SAV_OFF);
    }
    /* cartspeed2: objects the system makes once, on first use, and keeps for good are made here, while the
       heap is still in one piece. Made later (the first TLS handshake, the first staging hash) they landed in
       the middle of the big free block that every later TLS handshake needs (unit 1, `cart heapmap` with
       allocation hooks: the SHA-1 / SHA-256 / SHA-512 engine locks, 84 bytes each, at the top of the heap
       after the first install; the largest free block went from 24.5 KiB to 13-16 KiB) */
    {
        uint8_t d[64];
        (void)mbedtls_sha1((const unsigned char *)"cs", 2, d);
        (void)mbedtls_sha256((const unsigned char *)"cs", 2, d, 0);
        (void)mbedtls_sha512((const unsigned char *)"cs", 2, d, 0);
        /* the time functions' boot-time locks (every TLS handshake asks the time) */
        struct timeval tv;
        (void)gettimeofday(&tv, NULL);
        (void)time(NULL);
        /* the bignum hardware's lock (every handshake's key exchange): one multiplication big enough for the hardware */
        mbedtls_mpi a, b, x;
        mbedtls_mpi_init(&a); mbedtls_mpi_init(&b); mbedtls_mpi_init(&x);
        if (!mbedtls_mpi_lset(&a, 3) && !mbedtls_mpi_shift_l(&a, 1000) && !mbedtls_mpi_lset(&b, 5) && !mbedtls_mpi_shift_l(&b, 1000))
            (void)mbedtls_mpi_mul_mpi(&x, &a, &b);
        mbedtls_mpi_free(&a); mbedtls_mpi_free(&b); mbedtls_mpi_free(&x);
        _MwMx = xSemaphoreCreateMutex();      /* the staging windows' lock: made now, low in the heap, not mid-install */
    }
    _Part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "cart");
    if (!_Part) ESP_LOGW(TAG, "no \"cart\" partition (partitions_cartflash.csv): cart jobs refuse to back up");
    const cf_host_t h = {
        .user = NULL, .img_size = 0, .img_read = img_read,
        .bak_write = bak_write, .bak_read = bak_read, .ram_write = ram_write, .ram_read = ram_read,
        .sav_write = sav_write, .sav_read = sav_read, .chunk = chunk_hook,
        .bak_capacity = slots_max() * CF_BLK,
        .jr_load = jr_load, .jr_save = jr_save,
        .boot_select = boot_select, .cart_cycle = cart_cycle, .power_ok = power_ok,
        .progress = progress, .log = logm, .host_line = host_line, .host_frame = host_frame, .engine_allow = engine_allow,
        .overlay_hold = overlay_hold, .overlay_state = overlay_state,
        .xs_avail = xs_avail, .xs_send = xs_send, .xs_ring = xs_ring,
        .img_avail = img_avail, .img_gate = img_gate, .fpga_ver = fpga_ver, .img_blk_crc = img_blk_crc,
    };
    cf_esp_init(&_Cf, &h);
#ifdef CONFIG_NW_CARTFLASH_WIFI
    fetch_restage();                     /* a verified download from before the reboot */
#endif
    {
#ifdef CONFIG_NW_CARTFLASH_WIFI
        const cf_inst_host_t ih = { NULL, ci_api_start, ci_api_result, ci_base, ci_registered, ci_reg_forget, ci_fpga, ci_wifi, ci_hold_rx, ci_held,
                                    ci_peek_start, ci_peek_result, ci_power, ci_open, ci_arm, ci_job, ci_pending, ci_cancel, ci_log, ci_call_busy, ci_power_anyway, bk_stop_req, ci_start_body };
#endif /* CONFIG_NW_CARTFLASH_WIFI */
        cf_usb_init(&_Usb, &_Cf, usb_line, NULL);
        _Usb.heap_ok = usb_heap_ok; _Usb.heap_free = usb_heap_free; _Usb.heap_need = CF_JOB_MIN_HEAP;
        _Usb.net_hold = usb_net_hold;
        _Usb.stage_read = stage_read_mm; _Usb.stage_room = stage_room;
        _Usb.stage_erase = stage_erase; _Usb.stage_busy = stage_busy;
        _Usb.set_baud = usb_set_baud;
        {   /* cartspeed2: a clear owed from before the restart: the staged ROM still starts as the installed one */
            uint32_t want = 0, x = 0;
            if (clr_get(&want) == 0) {
                if (!stage_head(&x) && x == want) {
                    _ClrAt = now_ms() + 15000u; if (!_ClrAt) _ClrAt = 1;
                    _ClrCmds = _Usb.st_cmds;
                    printf("cart: the staged copy of the last install was being erased at the restart: it is erased in 15 s\n");
                } else clr_mark(0, 0);
            }
        }
#ifdef CONFIG_NW_CARTFLASH_GBT
        _Usb.gb_timed = 1;
#endif
#ifdef CONFIG_NW_CARTFLASH_WIFI
        cf_install_init(&_Inst, &ih, now_ms());
        ota_init();                      /* 1.1.0 */
#endif /* CONFIG_NW_CARTFLASH_WIFI */
    }
    const esp_console_cmd_t cmd = {
        .command = "cart",
        .help = "Cart flash (DRAFT): cart status | probe [flashid] | backup | install | resume | restore",
        .hint = NULL,
        .func = &cmd_cart,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
    CfRc_Register();                     /* 1.1.0: rc (RAM cart + FPGA boot flash bench) */
    CfRc_Init();                         /* 1.1.0: the kept game, the save store (moved after an update) */
#ifdef CONFIG_NW_CARTFLASH_WIFI
    Sd_SetUnpack(sd_unpack);             /* lane gamecomp1: `sd imp zpart` (and `sd st` says z1) */
    SdLib_SetCore(fpga_slot_cached);
    Sd_SetCartHooks(&_SdCartHooks);      /* lane sdcard1: SYSTEM > GAMES's cart backup / write from the card */
    CfShare_Register();                  /* lane share1: the console bench `share` */
#endif
    uint8_t job, ph;
    if (cf_esp_pending(&_Cf, &job, &ph))
        ESP_LOGW(TAG, "an interrupted cart write is in the journal (job %u): the OSD must offer resume / restore", job);
}
#endif

#ifdef CONFIG_NW_CARTFLASH_WIFI
/* fpgacomp1 bench: the FPGA install log (rc fjlog) */
void CfDev_FjLogPrint(int clear)
{
    struct { uint32_t runs, ok, rec_runs, last_ms; int32_t last_e; char ver[8]; } lg;
    nvs_handle_t h; size_t n = sizeof lg;
    memset(&lg, 0, sizeof lg);
    if (nvs_open("ota", NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_get_blob(h, "fjlog", &lg, &n) != ESP_OK || n != sizeof lg) memset(&lg, 0, sizeof lg);
    fota_rec_t j;
    const int have_j = !fota_load(&j, "fjob");
    printf("rc: fjlog runs %lu ok %lu from-recovery %lu last e=%ld %lu ms ver %s | fjob %s | recovery now %d\n", (unsigned long)lg.runs,
           (unsigned long)lg.ok, (unsigned long)lg.rec_runs, (long)lg.last_e, (unsigned long)lg.last_ms, lg.ver, have_j ? j.ver : "none", fpga_recovery());
    if (clear) { (void)nvs_erase_key(h, "fjlog"); (void)nvs_commit(h); }
    nvs_close(h);
}
#else
void CfDev_FjLogPrint(int clear) { (void)clear; }
#endif
