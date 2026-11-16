/* cf_rc_dev.c: Shiny Relay 1.1.0 (FPGA 18.30) device glue for the FPGA's own boot flash and the
 * RAM cart (fpga ramcart.v): the cart_xs ops 16-21 (boot flash) and 32-34 (RAM cart) over UART
 * address 0x12, their results in the status packets on address 0x0C (tapped here from
 * CfDev_XsStatus), PSRAM writes and window reads over the OSD's QSPI bus (main.c).
 *
 * Bench console (`rc`): the first proofs on a unit; the OSD flows build on the same calls.
 *   rc st                         the FPGA's RAM cart / slot status
 *   rc fid                        the boot flash's JEDEC id
 *   rc fcrc ADDR LEN              crc32 + blank check of boot flash bytes
 *   rc probe                      PSRAM byte order and size (write, read back through the window)
 *   rc load FLASHADDR LEN         a game from this ESP32's flash into PSRAM (ROM_BASE), verified
 *   rc ram [FLASHADDR] LEN        the save RAM: from flash, or FF
 *   rc on MBC ROMBANKS RAMBANKS [rtc]   play it (the core restarts on the RAM cart)
 *   rc off                        back to the cartridge slot
 *   rc save LEN                   crc32 of the save RAM as the game left it (PSRAM)
 *   rc rtc [S M H D]              the MBC3 clock: show, or set
 *   rc fscan                      the boot flash's header area (cf_fota.h, layout rf): kind, live header, next, free
 *   rc finst SRC LEN CRC          the FPGA update (cf_fota_install) from this ESP32's flash at SRC: resumable, checked
 *   rc fread ADDR LEN             boot flash bytes through the read window (FREAD): crc32 + first bytes + speed
 *   rc ga erase|prog|read|crc ... the game area ops (bench, speeds)
 */
#include "sdkconfig.h"
#ifdef NW_CARTFLASH

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "rom/ets_sys.h"
#include "esp_console.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "crc8_sae_j1850.h"

#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_private/spi_flash_os.h"
#include "nvs.h"
#include "cf_fota.h"
#include "cf_proto.h"
#include "cf_rc_dev.h"
#include "cf_rc_sleep.h"
int CfDev_FpgaRecovery(void);   /* cf_dev.c: the recovery design runs (no RAM cart in it) */
#include "nw_dev.h"
#include "esp_timer.h"
#include "sd.h"
#include "cf_sgz.h"
#include "cf_lib.h"
#include "mbedtls/sha256.h"
#define CF_INST_WRITABLE_RC 2031616u   /* the ModRetro cart: 2 MiB less its protected top block (cf_install.h CF_INST_WRITABLE) */
#include "cwl.h"
#include "cwl_online.h"
#include "cwl_time.h"
int CfDev_FlashReadMm(uint32_t addr, uint8_t *b, uint32_t n);   /* cf_dev.c: the flash through mapped windows */
void CfDev_FlashMmDrop(void);
uint32_t cf_crc32(uint32_t crc, const uint8_t *b, uint32_t n);   /* cf_esp.c: the table one */

static const char *TAG = "rc";

__attribute__((weak)) int OSD_QspiPsramWrite(uint32_t Addr, const uint8_t *pData) { (void)Addr; (void)pData; return -1; }
__attribute__((weak)) int OSD_QspiWinRead(uint16_t Off, uint8_t *pData, uint16_t Len) { (void)Off; (void)pData; (void)Len; return -1; }
__attribute__((weak)) int OSD_QspiCartWrite(uint16_t Off, const uint8_t *pData, uint16_t Len);
/* m110: a RAM game starting restarts the Game Boy core, and that closes the menu on the screen; the ESP32 learns the OSD's
   state only from the FPGA's button packets (main/fpga_rx.c), which do not come for that, so after SYSTEM > GAMES PLAY it
   still took the menu as shown: NW_OpenPage (a share's, a game's room menu) did not open it and pokes went to the hidden
   menu (unit 2, 2026-10-24). Told here, as cf_dev.c's card jobs do after a cart restart; a wrong "closed" corrects itself
   with the next button packet, a wrong "shown" did not. */
__attribute__((weak)) void OSD_SetVisiblityState(const bool IsVisible) { (void)IsVisible; }

/* the last status packet (32 bytes) and a counter, filled from the nw task */
static uint8_t _St[32];
static volatile uint32_t _StN;
static uint8_t _Seq = 0xC0;             /* 0xC0..0xFF: never the cart jobs' sequence numbers */
static uint8_t *_Buf;                   /* 1 KiB, DMA-capable */
/* gamecomp1: where a load is (a hang seen 1 in ~10 loads, no panic): the last mark, its time (rc where) */
volatile uint32_t g_rc_mark, g_rc_mark_ms;
#define RC_MARK() do { g_rc_mark = __LINE__; g_rc_mark_ms = (uint32_t)(esp_timer_get_time() / 1000); } while (0)

void CfRc_Status(const uint8_t *p, uint8_t n)
{
    if (n < 32 || p[0] != 1) return;
    memcpy(_St, p, 32);
    _StN++;
}

static void xs_send(const uint8_t *p, uint8_t n)
{
    uint8_t buf[3 + 16 + 1] = { 0x8F, CF_XS_ADDR_TO_FPGA, n };
    if (n > 16) return;
    memcpy(buf + 3, p, n);
    const size_t k = crc8_sae_j1850_encode(buf, 3 + (size_t)n, buf);
    (void)uart_write_bytes(UART_NUM_1, buf, k);
}

static uint32_t u24(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16; }
static uint32_t u32(const uint8_t *p) { return u24(p) | (uint32_t)p[3] << 24; }

/* one op, waited for: 0 = done (the status then holds its results), else the FPGA's error code
 * or -1 (no answer in ms) */
int CfRc_Op(uint8_t op, uint32_t f, uint32_t n, uint8_t arg, uint32_t ms)
{
    RC_MARK();
    const uint8_t seq = _Seq;
    _Seq = (uint8_t)(_Seq == 0xFF ? 0xC0 : _Seq + 1);
    const uint8_t p[10] = { CF_XS_P_OP, seq, op, (uint8_t)f, (uint8_t)(f >> 8), (uint8_t)(f >> 16),
                            (uint8_t)n, (uint8_t)(n >> 8), (uint8_t)(n >> 16), arg };
    xs_send(p, sizeof p);
    const TickType_t t0 = xTaskGetTickCount();
    uint32_t seen = _StN;
    for (;;) {
        if (_StN != seen) {
            seen = _StN;
            if (_St[3] != 0) return _St[3];
            if (_St[2] == seq && !(_St[1] & CF_XS_F_RUNNING)) return 0;
        }
        if ((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS > ms) {
            const uint8_t s = CF_XS_P_STATUS;
            xs_send(&s, 1);                                  /* one last look */
            vTaskDelay(pdMS_TO_TICKS(20));
            if (_St[2] == seq && !(_St[1] & CF_XS_F_RUNNING) && _St[3] == 0) return 0;
            return _St[3] ? _St[3] : -1;
        }
        vTaskDelay(1);
    }
}

int CfRc_Reset(void)
{
    const uint8_t r = CF_XS_P_RESET;
    xs_send(&r, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    const uint8_t s = CF_XS_P_STATUS;
    xs_send(&s, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    return _St[3];
}

/* fpgacomp1: RESET until the FPGA's op engine is idle with no error (an op the ESP32 left running when it restarted,
   e.g. a program waiting for ring bytes, ends by its own timeout first, ~4 s): 0 = ready */
int CfRc_ResetIdle(uint32_t ms)
{
    const int64_t t0 = esp_timer_get_time();
    printf("[%u ms] rc: reset idle: start (timeout %lu ms)\n", (unsigned)esp_log_timestamp(), (unsigned long)ms);
    for (uint32_t t = 0; t < ms; t += 100) {
        const int e = CfRc_Reset();
        if (e == 0 && !(_St[1] & CF_XS_F_RUNNING)) {
            printf("[%lld ms] rc: reset idle: ready after %lld ms (FPGA status %02x %02x, result %d)\n", (long long)(esp_timer_get_time() / 1000), (long long)(esp_timer_get_time() - t0) / 1000, _St[1], _St[3], e);
            return 0;
        }
        vTaskDelay(pdMS_TO_TICKS(80));
    }
    printf("[%lld ms] rc: reset idle: timeout after %lld ms (FPGA status %02x %02x, result -1)\n", (long long)(esp_timer_get_time() / 1000), (long long)(esp_timer_get_time() - t0) / 1000, _St[1], _St[3]);
    return -1;
}
uint32_t CfRc_Crc(void) { return u32(_St + 12); }
int CfRc_AllFF(void) { return (_St[19] & 0x80) != 0; }

static uint32_t crc32_step(uint32_t c, const uint8_t *b, uint32_t n)
{
    c = ~c;
    for (uint32_t i = 0; i < n; i++) {
        c ^= b[i];
        for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
    }
    return ~c;
}

static int buf_ok(void)
{
    if (!_Buf) _Buf = heap_caps_malloc(1024, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    return _Buf != NULL;
}
/* gamecomp1 (unit 1, r1831e, 2026-10-22): a window read that is read twice and kept only when both reads agree (crc32 of each):
   in a library play a checked RREAD's window came back "00 00" + the bytes 2 on now and then (every other try), and both the
   fast read and the checked re-read of a block came back wrong. Up to 4 tries; the counts tell whether reads differ (the
   QSPI transfer) or agree and are wrong (the window's fill) */
static uint32_t _WinReads, _WinDiff, _WinFixed;
static int win_read_sure(uint16_t len)
{
    for (int t = 0; t < 4; t++) {
        RC_MARK();
        if (OSD_QspiWinRead(0, _Buf, len)) return -2;
        RC_MARK();
        const uint32_t c1 = crc32_step(0, _Buf, len);
        if (OSD_QspiWinRead(0, _Buf, len)) return -2;
        _WinReads++;
        if (crc32_step(0, _Buf, len) == c1) { if (t) _WinFixed++; return 0; }
        _WinDiff++;
    }
    return -6;
}
static int playing(void);
/* receive mode (the TLS / http call needs the heap): the window buffer goes back unless the RAM cart plays */
void CfRc_Trim(void) { if (_Buf && !playing()) { heap_caps_free(_Buf); _Buf = NULL; } }
/* receive mode: a game playing from RAM stops (its save copied first), so the TLS call gets the heap its stack and
   buffer hold; 0 = nothing plays now (the caller tries its call again meanwhile) */
int CfRc_Yield(void)
{
    if (!playing()) { CfRc_Trim(); return 0; }
    printf("rc: receive mode: the game playing from RAM stops (its save is kept)\n");
    CfRc_Stop();
    return 1;
}

/* n bytes of PSRAM at addr (even) into dst, through the window: RREAD then the QSPI read */
int CfRc_PsramRead(uint32_t addr, uint8_t *dst, uint32_t n)
{
    if (!buf_ok()) return -1;
    while (n) {
        uint32_t k = 1024u - (addr & 1023u);                /* one 1 KiB PSRAM row per window */
        if (k > n) k = n;
        const uint32_t ke = (k + 1u) & ~1u;
        int e = CfRc_Op(CF_RC_O_RREAD, addr & ~0u, ke, 0, 200);
        if (e) return e;
        { const int we = win_read_sure((uint16_t)ke); if (we) return we; }
        memcpy(dst, _Buf, k);
        dst += k; addr += k; n -= k;
    }
    return 0;
}

/* len bytes from this ESP32's flash at src into PSRAM at dst (1 KiB aligned), read back and
 * compared by crc32 per KiB; 0 = all there */
int CfRc_LoadFromFlash(uint32_t src, uint32_t dst, uint32_t len, uint32_t *crc_out)
{
    static uint8_t chk[1024];
    if (!buf_ok() || (dst & 1023u) || (len & 1023u)) return -1;
    uint32_t crc = 0;
    for (uint32_t o = 0; o < len; o += 1024) {
        if (esp_flash_read(NULL, _Buf, src + o, 1024) != ESP_OK) return -3;
        crc = crc32_step(crc, _Buf, 1024);
        const uint32_t want = crc32_step(0, _Buf, 1024);
        int tries = 0;
        for (;;) {
            if (OSD_QspiPsramWrite(dst + o, _Buf)) return -4;
            vTaskDelay(1);                                  /* the 1 KiB burst lands */
            int e = CfRc_PsramRead(dst + o, chk, 1024);
            if (e) return e;
            if (crc32_step(0, chk, 1024) == want) break;
            if (++tries == 3) {
                static uint8_t src_b[1024];
                (void)esp_flash_read(NULL, src_b, src + o, 1024);
                int d = 0;
                while (d < 1024 && src_b[d] == chk[d]) d++;
                printf("rc: PSRAM %06lx: read back differs from byte %d (want %02x %02x %02x %02x, got %02x %02x %02x %02x)\n",
                       (unsigned long)(dst + o), d, src_b[d & ~3], src_b[(d & ~3) + 1], src_b[(d & ~3) + 2], src_b[(d & ~3) + 3],
                       chk[d & ~3], chk[(d & ~3) + 1], chk[(d & ~3) + 2], chk[(d & ~3) + 3]);
                return -5;
            }
            if (esp_flash_read(NULL, _Buf, src + o, 1024) != ESP_OK) return -3;
        }
    }
    if (crc_out) *crc_out = crc;
    return 0;
}

/* RCFG: the mapper and the flags (CF_RC_A_*) */
static int _SdKeepOn;                /* a stopped RAM game keeps the slot off through the next card load */
static volatile uint8_t _CfgFlags;   /* 1.1.2: the last RCFG flags the core took */
int CfRc_Cfg(uint8_t mbc, uint16_t rom_banks, uint8_t ram_banks, int rtc, uint8_t flags)
{
    const uint32_t rom_mask = rom_banks ? (uint32_t)(rom_banks - 1u) & 0xFFu : 0u;
    const uint32_t ram_mask = ram_banks ? (uint32_t)(ram_banks - 1u) & 0xFu : 0u;
    const uint32_t f = (mbc & 7u) | (ram_banks ? 8u : 0u) | (rtc ? 16u : 0u) | rom_mask << 8 | ram_mask << 16;
    const int e = CfRc_Op(CF_RC_O_RCFG, f, 0, flags, 200);
    if (!e) _CfgFlags = flags;             /* 1.1.2: what the core was last told (held or not) */
    return e;
}
/* 1.1.2: the core held with no game playing from RAM (a library write left it so): handed back to the cartridge, as
   CfRc_RamRelease does; 1 = it was (the cartridge's game restarted, which closes the menu) */
static volatile int _Released;
static int release_if_held(const char *why)
{
    if (CfRc_Playing() || !(_CfgFlags & CF_RC_A_HOLD)) return 0;
    (void)CfRc_Cfg(0, 0, 0, 0, CF_RC_A_HOLD);
    vTaskDelay(pdMS_TO_TICKS(300));
    (void)CfRc_Cfg(0, 0, 0, 0, 0);
    printf("rc: %s: the core was held: back to the cartridge\n", why);
    _Released = 1;
    return 1;
}
int CfRc_TakeReleased(void) { const int r = _Released; _Released = 0; return r; }

/* ---- the FPGA's boot flash: FPROG / FHDR take their bytes from the express ring (cart_xs.v), as a cart install's
 * PROG does: each KiB is written into the ring over QSPI (whole 32-bit words, FF padded) and POSTed with its crc32;
 * the FPGA checks it before it takes it (`valid` moves on), a refused post is sent again. The ring holds 4 KiB:
 * one op of up to 4 KiB is posted whole, then run, so a ring slot is never written while it waits. */
static uint32_t _Pos;                  /* stream position since RingBegin */
int CfRc_RingBegin(void) { _Pos = 0; return CfRc_Reset(); }

static uint32_t st_valid(void) { return u24(_St + 8); }
/* one ring write + its POST (no wait: the FPGA checks posts in order and moves `valid` on) */
static int ring_post(const uint8_t *b, uint32_t n, uint32_t at)
{
    const uint32_t q = (n + 3u) & ~3u;
    memcpy(_Buf, b, n);
    for (uint32_t i = n; i < q; i++) _Buf[i] = 0xFF;
    const uint32_t crc = crc32_step(0, _Buf, n), e = at + n;
    if (OSD_QspiCartWrite((uint16_t)(at % CF_XS_RING), _Buf, (uint16_t)q)) return -2;
    const uint8_t p[11] = { CF_XS_P_POST, (uint8_t)e, (uint8_t)(e >> 8), (uint8_t)(e >> 16), (uint8_t)(e >> 24),
                            (uint8_t)n, (uint8_t)(n >> 8), (uint8_t)crc, (uint8_t)(crc >> 8), (uint8_t)(crc >> 16), (uint8_t)(crc >> 24) };
    xs_send(p, sizeof p);
    return 0;
}
/* n (<= 4 KiB) bytes into the ring from _Pos, all posted, then one STATUS asked: 0 once the FPGA holds them all as
   valid. A refused post (its crc32 did not check) leaves `valid` short: the rest is sent again from there. */
static int ring_fill(const uint8_t *b, uint32_t n)
{
    if (!buf_ok() || n == 0 || n > CF_XS_RING) return -1;
    const uint32_t start = _Pos, end = _Pos + n;
    uint32_t from = start;
    for (int tries = 0; tries < 4; tries++) {
        for (uint32_t at = from; at < end; at += CF_XS_CHUNK) {
            const uint32_t k = end - at < CF_XS_CHUNK ? end - at : CF_XS_CHUNK;
            const int e = ring_post(b + (at - start), k, at);
            if (e) return e;
        }
        const TickType_t t0 = xTaskGetTickCount();
        uint32_t seen = _StN;
        const uint8_t sreq = CF_XS_P_STATUS;
        xs_send(&sreq, 1);
        while ((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS < 200u) {
            if (_StN != seen) {
                seen = _StN;
                if (st_valid() >= (end & 0xFFFFFFu)) { _Pos = end; return 0; }
                xs_send(&sreq, 1);                         /* not all checked yet: look again */
            }
            vTaskDelay(1);
        }
        const uint32_t v = st_valid();
        from = (v > start && v < end) ? v : start;         /* a refused post: again from where `valid` stopped */
    }
    return -3;
}
static int ring_op(uint8_t op, uint32_t a, const uint8_t *b, uint32_t n, uint32_t ms)
{
    const int e = ring_fill(b, n);
    return e ? e : CfRc_Op(op, a, n, 0, ms);
}
int CfRc_FlashErase64(uint32_t a) { return CfRc_Op(CF_RC_O_FERASE, a, 0, 0xD8, 4000); }
int CfRc_FlashProg(uint32_t a, const uint8_t *b, uint32_t n) { return ring_op(CF_RC_O_FPROG, a, b, n, 2000); }
int CfRc_FlashHdr(uint32_t a, const uint8_t *b, uint32_t n) { return ring_op(CF_RC_O_FHDR, a, b, n, 500); }
int CfRc_FlashCrc(uint32_t a, uint32_t n, uint32_t *crc, int *all_ff)
{
    const int e = CfRc_Op(CF_RC_O_FCRC, a, n, 0, 2000u + n / 64u);   /* x1 at hclk/2: ~1 MiB in a few s */
    if (e) return e;
    *crc = CfRc_Crc();
    *all_ff = CfRc_AllFF();
    return 0;
}

/* FREAD (FPGA 18.31): n bytes of the boot flash at a into dst, through the flash read window (1 KiB per op), each
   window checked against the crc32 the FPGA computed while it read the flash: 0 = all there */
static int _FrDbg;
int CfRc_FlashRead(uint32_t a, uint8_t *dst, uint32_t n)
{
    if (!buf_ok()) return -1;
    while (n) {
        const uint32_t k = n < 1024u ? n : 1024u;
        const uint32_t ke = (k + 1u) & ~1u;
        int tries = 0;
        for (;;) {
            const uint8_t seq0 = _St[2];
            int e = CfRc_Op(CF_RC_O_FREAD, a, ke, 0, 300);
            if (_FrDbg) printf("rc: fread dbg op %06lx %lu e=%d seq %02x->%02x err %02x f %02x%02x%02x crc %08lx\n", (unsigned long)a, (unsigned long)ke, e,
                               seq0, _St[2], _St[3], _St[18], _St[17], _St[16], (unsigned long)CfRc_Crc());
            if (e) return e;
            const uint32_t want = CfRc_Crc();
            if (OSD_QspiWinRead(0, _Buf, (uint16_t)ke)) return -2;
            if (_FrDbg) printf("rc: fread dbg win %02x%02x%02x%02x crc %08lx want %08lx\n", _Buf[0], _Buf[1], _Buf[2], _Buf[3], (unsigned long)crc32_step(0, _Buf, ke), (unsigned long)want);
            if (crc32_step(0, _Buf, ke) == want) break;
            if (++tries == 3) return -5;                      /* the window read did not come across whole */
        }
        memcpy(dst, _Buf, k);
        dst += k; a += k; n -= k;
    }
    return 0;
}

#include "cf_rc_win.inc"
/* the checked way (the status round trip) at any address. Each window is filled twice (two RREADs) and kept when both
   agree: with the GB core held (RCFG HOLD, as every load does) an RREAD now and then fills its window one word late (a
   word of nothing first, the bytes 2 on): unit 1 2026-10-22, ~1 in 40 windows of 256 bytes, r1831b as r1831e, never with
   the core running. Two fills that agree are right (each is wrong on its own, not the same way twice). Up to 6 fills */
static uint32_t _RrFills, _RrRedo;
static int psram_read_any(uint32_t a, uint8_t *dst, uint32_t n)
{
    if (!buf_ok()) return -1;
    while (n) {
        const uint32_t a0 = a & ~1u, sk = a - a0;
        const uint32_t rl = 1024u - (a0 & 1023u) - sk;     /* (inside one 1 KiB PSRAM row, as ramcart.v's window read asks) */
        const uint32_t k = n < rl ? n : rl, ke = (sk + k + 1u) & ~1u;
        uint32_t c_prev = 0;
        int agreed = 0;
        for (int t = 0; t < 6 && !agreed; t++) {
            int e = CfRc_Op(CF_RC_O_RREAD, a0, ke, 0, 200);
            if (e) return e;
            { const int we = win_read_sure((uint16_t)ke); if (we) return we; }
            _RrFills++;
            const uint32_t c = crc32_step(0, _Buf, ke);
            if (t && c == c_prev) agreed = 1; else { if (t) _RrRedo++; c_prev = c; }
        }
        if (!agreed) return -7;
        memcpy(dst, _Buf + sk, k);
        dst += k; a += k; n -= k;
    }
    return 0;
}
static int win_timed(uint8_t op, uint32_t a, uint8_t *dst, uint32_t n, uint32_t us) { return win_timed_lim(op, a, dst, n, us, a + n); }

/* cf_fota's flash, through the FPGA. The ring positions start again before the first program after a header op
   (a header op begins its own ring) */
static int _RingFresh;
static int fh_fcrc(void *u, uint32_t a, uint32_t n, uint32_t *crc, int *ff) { (void)u; return CfRc_FlashCrc(a, n, crc, ff); }
static int fh_hdr(void *u, uint32_t a, const uint8_t *b, uint32_t n)
{
    (void)u;
    _RingFresh = 0;
    const int e = CfRc_RingBegin();
    return e ? e : CfRc_FlashHdr(a, b, n);
}
static int fh_erase(void *u, uint32_t a, uint32_t len) { (void)u; return CfRc_Op(CF_RC_O_FERASE, a, 0, len == 0x10000u ? 0xD8 : 0x20, len == 0x10000u ? 4000 : 1000); }
static int fh_read(void *u, uint32_t a, uint8_t *b, uint32_t n) { (void)u; return CfRc_FlashRead(a, b, n); }
static int fh_prog(void *u, uint32_t a, const uint8_t *b, uint32_t n)
{
    (void)u;
    if (!_RingFresh) { const int e = CfRc_RingBegin(); if (e) return e; _RingFresh = 1; }
    return CfRc_FlashProg(a, b, n);
}
const cf_fota_host_t *CfRc_FotaHost(void)
{
    static const cf_fota_host_t h = { NULL, fh_fcrc, fh_hdr, fh_erase, fh_prog, fh_read };
    _RingFresh = 0;
    return &h;
}

/* ---- the GAME AREA of the boot flash (cf_fota.h: 0x05C000 up to 0x25E000):
   every op refuses anything outside it, reserving both design slots even in recovery.
   The header scan validates the layout; its live target never changes the fixed end. */
static uint32_t _GaEnd;
void CfRc_GameAreaRescan(void) { _GaEnd = 0; }
int CfRc_GameArea(uint32_t *base, uint32_t *size)
{
    if (!_GaEnd) {
        cf_fota_layout_t L;
        uint32_t b = 0, n = 0;
        if (cf_fota_scan(CfRc_FotaHost(), &L) || cf_fota_game_region(&L, &b, &n)) return -1;
        _GaEnd = b + n;
    }
    *base = CF_FOTA_GAME_BASE; *size = _GaEnd - CF_FOTA_GAME_BASE;
    return 0;
}
static int ga_in(uint32_t a, uint32_t n)
{
    uint32_t b = 0, sz = 0;
    if (CfRc_GameArea(&b, &sz)) return 0;
    return a >= b && n <= sz && a + n <= b + sz;
}
int CfRc_GameAreaErase(uint32_t a, uint32_t n)
{
    if ((a & 0xFFFu) || (n & 0xFFFu) || !ga_in(a, n)) return -10;
    while (n) {
        int e;
        if (!(a & 0xFFFFu) && n >= 0x10000u) { e = CfRc_Op(CF_RC_O_FERASE, a, 0, 0xD8, 4000); a += 0x10000u; n -= 0x10000u; }
        else { e = CfRc_Op(CF_RC_O_FERASE, a, 0, 0x20, 1000); a += 0x1000u; n -= 0x1000u; }
        if (e) return e;
    }
    return 0;
}
/* savefast1: a save's pieces go through one ring (a RESET costs 40 ms: per 2 KiB piece that was the 22 KB/s of a save) */
static int _GaRingOpen;
int CfRc_GameAreaProg(uint32_t a, const uint8_t *b, uint32_t n)
{
    if (!ga_in(a, n)) return -10;
    int e = (_GaRingOpen && n <= CF_XS_RING - (_Pos % CF_XS_RING)) ? 0 : CfRc_RingBegin();
    for (uint32_t o = 0; o < n && !e; o += 4096u) e = CfRc_FlashProg(a + o, b + o, n - o < 4096u ? n - o : 4096u);
    return e;
}
int CfRc_GameAreaRead(uint32_t a, uint8_t *dst, uint32_t n) { return ga_in(a, n) ? CfRc_FlashRead(a, dst, n) : -10; }
int CfRc_GameAreaCrc(uint32_t a, uint32_t n, uint32_t *crc, int *all_ff) { return ga_in(a, n) ? CfRc_FlashCrc(a, n, crc, all_ff) : -10; }
int CfRc_FlashCopy(uint32_t a, uint32_t pa, uint32_t n, uint32_t *crc)
{
    if ((n & 1u) || (pa & 1u) || n == 0) return -10;
    int e = CfRc_Op(CF_RC_O_FCDST, pa, 0, 0, 300);
    if (!e) e = CfRc_Op(CF_RC_O_FCOPY, a, n, 0, 3000u + n / 500u);   /* ~1 MB/s: n / 1000 ms, twice that allowed */
    if (!e && crc) *crc = CfRc_Crc();
    return e;
}
int CfRc_GameAreaCopy(uint32_t a, uint32_t pa, uint32_t n, uint32_t *crc) { return ga_in(a, n) ? CfRc_FlashCopy(a, pa, n, crc) : -10; }

/* ================================================================ the RAM cart manager
 *
 * The game kept in this Chromatic is the one a "play" send staged in the slot that is not running
 * (cf_dev.c dl_for; kept there instead of erased): NVS "rc"/"game" says where it is and what it is.
 * Any other download, a cart install's staging or an update into that slot drops the record first
 * (CfRc_GameGone).
 *
 * Saves: a store of 64 KiB next to the RUNNING slot, the one place neither staging nor an update
 * ever writes (running ota_0: 0x1F0000, before otadata; running ota_1: 0x3F0000, the flash's top).
 * After an update moves to the other slot, the first boot copies the store over (CfRc_Init).
 * Sector 0 is the index (magic, entries: game crc32, first data sector, sectors, size, data crc32,
 * the MBC3 clock); sectors 1-15 hold the saves. The game's save RAM is copied there 1.5 s after
 * its last write (the FPGA's dirty flag), only the 4 KiB sectors that changed, and when the
 * RAM cart stops. The clock counts only while the Chromatic is on (no wall clock on this unit).
 */
#define RC_STORE_A   0x1F0000u          /* running ota_0 */
#define RC_STORE_B   0x3F0000u          /* running ota_1 */
#define RC_STORE_SZ  0x10000u
#define RC_MAGIC     0x31534352u        /* "RCS1" */
#define RC_ENTRIES   15
#define RC_QUIET_MS  200u              /* savefast1: was 1500: a game that writes only when it saves is copied at once */
#define RC_MAX_MS    10000u           /* a game that writes its save RAM all the time (Shiny Kart's counters): a copy at least this often */

typedef struct {
    uint32_t crc;                       /* the game's crc32 (the whole ROM) */
    uint16_t sect, nsect;               /* data sectors [sect, sect + nsect) of the store */
    uint32_t size;                      /* save bytes */
    uint32_t dcrc;                      /* crc32 of the save as last written */
    uint8_t  rtc[5];                    /* {S, M, H, DL, DH} */
    uint8_t  used;
    uint32_t lru;
} rc_ent_t;
typedef struct {
    uint32_t magic, seq;
    rc_ent_t e[RC_ENTRIES];
    uint32_t tm[RC_ENTRIES];            /* 1.1.7: when each entry's clock was true (unix s). After the entries so the index of 1.1.6
                                           (shorter, erased flash after it: 0xFFFFFFFF) still loads and 1.1.6 still reads a newer one */
} rc_index_t;
typedef struct {
    uint32_t magic;
    uint32_t base, size, crc;           /* where the game is staged, its length and crc32 */
    char     title[17];
} rc_game_t;

static rc_index_t _Ix;                  /* (static: the console / task stacks are small) */
static rc_game_t  _Game;
static int        _GameOk;
/* 1.1.0: a game streamed straight into PSRAM over Wi-Fi (bigger than the staging area): played from there, not kept */
static rc_game_t  _RamGame;
static volatile int _InRam;             /* the next play is of _RamGame, already in PSRAM */
static uint8_t   *_Carry;               /* the KiB being filled (DMA-capable) */
static uint32_t   _CarryAt;             /* its PSRAM offset (from _CarryBase) */
static uint32_t   _CarryBase = CF_RC_ROM_BASE, _CarryMax = CF_RC_ROM_MAX;   /* lane share1: the region being written (the ROM area, PSRAM scratch, the save RAM) */
static int       _CarryDirty;           /* lane share1: bytes in the carry KiB not written yet */
static int       _CarryAll = 1;         /* lane share1: every KiB read back (0: the first 16 and every 64th: a container, checked again at its unpack) */
static int       _SaveGiven;            /* lane share1: the next play's save is in the save RAM already (it came with the game): kept, put in the store */
static struct {
    volatile int st;                    /* kNwPv_* */
    volatile int req;                   /* 1 play, 2 stop, 3 result seen */
    volatile int pct;
    char  r1[20], r2[20];
    uint8_t mbc, ram_banks, rtc, battery;
    uint16_t rom_banks;
    uint32_t sav_size, crc;
    int   ent;                          /* the store entry of the game playing (-1 none) */
    int   pend;                         /* a save write seen, not yet in the store */
    int   notkept;                      /* lane gamecomp1: the game playing was not kept (1 the library is full, 2 it could not be) */
    uint32_t pend_at, pend_first, saves, last_save;
    TaskHandle_t task;
} _P = { .ent = -1 };

static uint32_t store_base(void)
{
    const esp_partition_t *r = esp_ota_get_running_partition();
    return (r && r->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1) ? RC_STORE_B : RC_STORE_A;
}
static uint32_t store_other(void) { return store_base() == RC_STORE_A ? RC_STORE_B : RC_STORE_A; }

/* the store's own view of the flash chip: only its 64 KiB may be erased / written (as cf_dev.c stage_chip) */
static esp_err_t store_region_ok(void *arg, size_t start, size_t size)
{
    (void)arg;
    const uint32_t b = store_base();
    return (start >= b && start + size <= b + RC_STORE_SZ) ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}
static esp_flash_t *store_chip(void)
{
    static esp_flash_os_functions_t os;
    static esp_flash_t chip;
    static int made;
    if (!made) {
        if (!esp_flash_default_chip || !esp_flash_default_chip->os_func) return NULL;
        os = *esp_flash_default_chip->os_func;
        os.region_protected = store_region_ok;
        chip = *esp_flash_default_chip;
        chip.os_func = &os;
        made = 1;
    }
    return &chip;
}
static int store_erase(uint32_t a) { esp_flash_t *c = store_chip(); return c && esp_flash_erase_region(c, a, 0x1000) == ESP_OK ? 0 : -1; }
static int store_write(uint32_t a, const void *b, uint32_t n) { esp_flash_t *c = store_chip(); return c && esp_flash_write(c, b, a, n) == ESP_OK ? 0 : -1; }

static int index_load(void)
{
    if (esp_flash_read(NULL, &_Ix, store_base(), sizeof _Ix) != ESP_OK || _Ix.magic != RC_MAGIC) {
        memset(&_Ix, 0, sizeof _Ix);
        _Ix.magic = RC_MAGIC;
        return -1;
    }
    return 0;
}
static int index_save(void)
{
    _Ix.magic = RC_MAGIC; _Ix.seq++;
    const uint32_t b = store_base();
    if (store_erase(b)) return -1;
    return store_write(b, &_Ix, sizeof _Ix);
}

/* the entry for a game (crc), made if asked: -1 none */
static int ent_for(uint32_t crc, uint32_t size, int make)
{
    for (int i = 0; i < RC_ENTRIES; i++) if (_Ix.e[i].used && _Ix.e[i].crc == crc && _Ix.e[i].size == size) return i;
    if (!make || size == 0 || size > 15u * 4096u) return -1;
    const uint16_t need = (uint16_t)((size + 4095u) / 4096u);
    for (;;) {
        /* the first free run of sectors 1..15 */
        uint8_t taken[16] = { 1 };
        for (int i = 0; i < RC_ENTRIES; i++) if (_Ix.e[i].used) for (int k = 0; k < _Ix.e[i].nsect; k++) taken[_Ix.e[i].sect + k] = 1;
        for (int s0 = 1; s0 + need <= 16; s0++) {
            int ok = 1;
            for (int k = 0; k < need; k++) if (taken[s0 + k]) { ok = 0; break; }
            if (!ok) continue;
            for (int i = 0; i < RC_ENTRIES; i++) if (!_Ix.e[i].used) {
                memset(&_Ix.e[i], 0, sizeof _Ix.e[i]);
                _Ix.tm[i] = 0;
                _Ix.e[i].used = 1; _Ix.e[i].crc = crc; _Ix.e[i].size = size; _Ix.e[i].sect = (uint16_t)s0; _Ix.e[i].nsect = need;
                return i;
            }
        }
        /* full: the save not played for longest goes */
        int old = -1;
        for (int i = 0; i < RC_ENTRIES; i++) if (_Ix.e[i].used && (old < 0 || _Ix.e[i].lru < _Ix.e[old].lru)) old = i;
        if (old < 0) return -1;
        printf("rc: save store full: the save of game %08lx goes\n", (unsigned long)_Ix.e[old].crc);
        _Ix.e[old].used = 0;
    }
}

/* ---- 1.1.7: a game's clock from its load to its saves (sdc_clk_*, sd_core.c; the rules and their tests are there) */
static sdc_clk_t _Clk;
static int64_t clk_up(void) { return esp_timer_get_time() / 1000000; }
static uint64_t ix_tm(int ent) { return ent >= 0 && _Ix.tm[ent] != 0xFFFFFFFFu ? _Ix.tm[ent] : 0; }
/* a game's clock as loaded: rt from its save, t_saved when it was true (0 = not known); the clock caught up when both times are known */
static void clk_loaded(uint8_t rt[5], uint64_t t_saved)
{
    const uint64_t now = Sd_UnixTime();
    const int adv = sdc_clk_load(&_Clk, rt, t_saved, now, clk_up());
    printf("rc: clock: saved at %llu, now %llu: %s\n", (unsigned long long)t_saved, (unsigned long long)now,
           adv ? "caught up" : now ? "nothing to catch up" : t_saved ? "no real time yet: the clock is kept as it was and the gap is caught up later" : "no real time");
}
/* the live clock rt as a save keeps it (caught up if the load could not), and the real time it is true at (0 = not known) */
static uint64_t clk_keep(uint8_t rt[5])
{
    const uint64_t t = sdc_clk_save(&_Clk, rt, Sd_UnixTime(), clk_up());
    printf("rc: clock: kept, true at %llu\n", (unsigned long long)t);
    return t;
}

/* 1.1.0: after an update to the other slot the store is still next to the old one: copy it over */
static void store_migrate(void)
{
    static uint8_t b[1024];
    uint32_t mg = 0;
    if (esp_flash_read(NULL, &mg, store_base(), 4) == ESP_OK && mg == RC_MAGIC) return;
    if (esp_flash_read(NULL, &mg, store_other(), 4) != ESP_OK || mg != RC_MAGIC) return;
    printf("rc: the save store moves next to the running slot (%06lx -> %06lx)\n", (unsigned long)store_other(), (unsigned long)store_base());
    for (uint32_t s = 1; s < 16; s++) {                 /* data first, the index last (a cut: copied again) */
        if (store_erase(store_base() + s * 4096u)) return;
        for (uint32_t o = 0; o < 4096u; o += 1024u) {
            if (esp_flash_read(NULL, b, store_other() + s * 4096u + o, 1024) != ESP_OK) return;
            if (store_write(store_base() + s * 4096u + o, b, 1024)) return;
        }
    }
    if (store_erase(store_base())) return;
    for (uint32_t o = 0; o < 4096u; o += 1024u) {
        if (esp_flash_read(NULL, b, store_other() + o, 1024) != ESP_OK) return;
        if (store_write(store_base() + o, b, 1024)) return;
    }
}

/* the kept game (NVS) */
static void game_load(void)
{
    nvs_handle_t h;
    size_t n = sizeof _Game;
    _GameOk = 0;
    if (nvs_open("rc", NVS_READONLY, &h) != ESP_OK) return;
    if (nvs_get_blob(h, "game", &_Game, &n) == ESP_OK && n == sizeof _Game && _Game.magic == RC_MAGIC) _GameOk = 1;
    nvs_close(h);
}
static void game_store(int keep)
{
    nvs_handle_t h;
    if (nvs_open("rc", NVS_READWRITE, &h) != ESP_OK) return;
    if (keep) { _Game.magic = RC_MAGIC; (void)nvs_set_blob(h, "game", &_Game, sizeof _Game); }
    else (void)nvs_erase_key(h, "game");
    (void)nvs_commit(h);
    nvs_close(h);
    _GameOk = keep;
}
void CfRc_GameGone(void)
{
    if (!_GameOk) return;
    if (_P.st == kNwPv_Playing || _P.st == kNwPv_Loading) return;    /* (PSRAM has it: it plays on until stopped) */
    printf("rc: the kept game %s is being replaced (the slot is written)\n", _Game.title);
    game_store(0);
}
void CfRc_Keep(uint32_t base, uint32_t size, uint32_t crc, const char *title)
{
    memset(&_Game, 0, sizeof _Game);
    _Game.base = base; _Game.size = size; _Game.crc = crc;
    snprintf(_Game.title, sizeof _Game.title, "%s", title ? title : "");
    game_store(1);
}
int CfRc_Kept(void) { return _GameOk; }

/* the cart header: the mapper and the sizes; 0 = playable */
static int header_parse(const uint8_t *h, uint32_t size, const char **why)
{
    const uint8_t t = h[0x147], rs = h[0x148], ra = h[0x149];
    _P.battery = 0; _P.rtc = 0;
    switch (t) {
    case 0x00: _P.mbc = 0; break;
    case 0x08: _P.mbc = 0; break;
    case 0x09: _P.mbc = 0; _P.battery = 1; break;
    case 0x01: case 0x02: _P.mbc = 1; break;
    case 0x03: _P.mbc = 1; _P.battery = 1; break;
    case 0x05: _P.mbc = 2; break;
    case 0x06: _P.mbc = 2; _P.battery = 1; break;
    case 0x0F: case 0x10: _P.mbc = 3; _P.battery = 1; _P.rtc = 1; break;
    case 0x11: case 0x12: _P.mbc = 3; break;
    case 0x13: _P.mbc = 3; _P.battery = 1; break;
    case 0x19: case 0x1A: case 0x1C: case 0x1D: _P.mbc = 5; break;
    case 0x1B: case 0x1E: _P.mbc = 5; _P.battery = 1; break;
    default: *why = "MAPPER"; return -1;
    }
    /* m110t: the card's rule (sd_core.c): a file shorter than its header says plays (rom_pad fills the rest with FF) */
    if (sdc_rom_fit(rs, size, CF_RC_ROM_MAX, &_P.rom_banks, why)) return -1;
    static const uint8_t banks[6] = { 0, 1, 1, 4, 16, 8 };
    _P.ram_banks = ra < 6 ? banks[ra] : 0;
    _P.sav_size = (uint32_t)_P.ram_banks * 0x2000u;
    if (ra == 1) _P.sav_size = 0x800;
    if (_P.mbc == 2) { _P.ram_banks = 1; _P.sav_size = 0x200; }
    return 0;
}
static int cfg_now(uint8_t flags) { return CfRc_Cfg(_P.mbc, _P.rom_banks, _P.ram_banks, _P.rtc, flags); }

static int save_copy_sd(void);
static uint8_t _Src;                     /* the game playing / asked for: 0 kept (ESP32 flash), 1 in RAM (Wi-Fi), 2 the card, 3 the library, 4 staged, not kept */
static rc_game_t _TmpGame;                /* lane gamecomp1: _Src 4 */
static int play_lib(void);
static int save_copy_lib(void);
/* the save RAM (PSRAM) into the store: only the sectors that differ. 0 = in the store */
static int save_copy_store(void);
static int save_copy(void) { return _Src == 2 ? save_copy_sd() : _Src == 3 ? save_copy_lib() : save_copy_store(); }
static int save_copy_store(void)
{
    static uint8_t a[1024], b[1024];
    if (_P.ent < 0 || !_P.sav_size) return 0;
    rc_ent_t *e = &_Ix.e[_P.ent];
    const uint32_t base = store_base() + (uint32_t)e->sect * 4096u;
    uint32_t crc = 0;
    int wrote = 0;
    for (uint32_t s = 0; s < e->nsect; s++) {
        int same = 1;
        for (uint32_t o = 0; o < 4096u && same; o += 1024u) {
            const uint32_t at = s * 4096u + o;
            if (at >= _P.sav_size) break;
            if (CfRc_PsramRead(CF_RC_RAM_BASE + at, a, 1024)) return -1;
            if (esp_flash_read(NULL, b, base + at, 1024) != ESP_OK) return -1;
            if (memcmp(a, b, 1024)) same = 0;
        }
        if (same) continue;
        if (store_erase(base + s * 4096u)) return -1;
        for (uint32_t o = 0; o < 4096u; o += 1024u) {
            const uint32_t at = s * 4096u + o;
            if (at >= _P.sav_size) break;
            if (CfRc_PsramRead(CF_RC_RAM_BASE + at, a, 1024)) return -1;
            if (store_write(base + at, a, 1024)) return -1;
        }
        wrote++;
    }
    for (uint32_t o = 0; o < _P.sav_size; o += 1024u) {
        const uint32_t k = _P.sav_size - o < 1024u ? _P.sav_size - o : 1024u;
        if (esp_flash_read(NULL, a, base + o, k) != ESP_OK) return -1;
        crc = crc32_step(crc, a, k);
    }
    int ix = 0;
    if (_P.rtc && CfRc_Op(CF_RC_O_RRTC, 0, 0, 0, 200) == 0) {
        const uint32_t r = CfRc_Crc();
        const uint8_t rt[5] = { (uint8_t)(r & 0x3F), (uint8_t)((r >> 8) & 0x3F), (uint8_t)((r >> 16) & 0x1F), (uint8_t)(r >> 24), _St[16] };
        uint8_t kp[5];
        memcpy(kp, rt, 5);
        const uint64_t tk = clk_keep(kp);                 /* (1.1.7: caught up if the load could not; and the time it is true at) */
        if (memcmp(kp, e->rtc, 5)) { memcpy(e->rtc, kp, 5); ix = 1; }
        const int ei = (int)(e - _Ix.e);
        if (ei >= 0 && ei < RC_ENTRIES && tk && (uint32_t)tk != _Ix.tm[ei]) { _Ix.tm[ei] = (uint32_t)tk; ix = 1; }
    }
    if (crc != e->dcrc) { e->dcrc = crc; ix = 1; }
    if (ix && index_save()) return -1;
    _P.saves++;
    _P.last_save = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (wrote) printf("rc: save: %d sector(s) written (crc %08lx)\n", wrote, (unsigned long)crc);
    return 0;
}

static void result(int ok_kind, const char *r1, const char *r2)
{
    snprintf(_P.r1, sizeof _P.r1, "%s", r1);
    snprintf(_P.r2, sizeof _P.r2, "%s", r2);
    _P.st = ok_kind;
    printf("rc: play: %s %s %s\n", ok_kind == kNwPv_NoFpga ? "this FPGA has no RAM cart" : "not playing:", r1, r2);
}

/* ================================================================ lane gamecomp1: an SGZ1 container unpacked into PSRAM
 * The source is any reader (this ESP32's flash: the kept game; later the FPGA flash's library); each block lands in
 * PSRAM 1 KiB per QSPI write, read back as the card's path does (the first 16 KiB and every 64th KiB; a KiB that reads
 * back wrong is written again and every KiB after it is read back). The format's own checks (every block's crc32, the
 * game's crc32 + sha256) come on top. */
/* the page's view of the library (lane gamecomp1; filled by lib_snap) */
static portMUX_TYPE _LibMux = portMUX_INITIALIZER_UNLOCKED;
static struct { int n; uint32_t free, total; cf_lib_view_ent_t e[CF_LIB_MAX_GAMES]; } *_Snap;
typedef struct {
    int (*rd)(void *u, uint32_t off, uint8_t *b, uint32_t n);
    int (*rdf)(void *u, uint32_t off, uint8_t *b, uint32_t n);   /* a fast reader for the blocks (NULL: rd) */
    void *ru;
    uint8_t *wb, *chk;
    int all;
    uint32_t checked, misses, ms_wr, op_errs;
    int last_err;
    int64_t t_wr;
} zp_t;
static int sd_ram_write(uint32_t a, const uint8_t *b, uint8_t *chk, int verify, uint32_t *checked, uint32_t *misses);
static int cfg_now(uint8_t flags);
static int zp_rd(void *u, uint32_t off, uint8_t *b, uint32_t n) { zp_t *z = u; return z->rd(z->ru, off, b, n); }
static int zp_rdf(void *u, uint32_t off, uint8_t *b, uint32_t n) { zp_t *z = u; return z->rdf(z->ru, off, b, n); }
static int zp_wr(void *u, uint32_t off, const uint8_t *b, uint32_t n)
{
    zp_t *z = u;
    const int64_t t = esp_timer_get_time();
    for (uint32_t q = 0; q < n; q += 1024u) {
        const uint32_t k = n - q < 1024u ? n - q : 1024u;
        /* a whole KiB goes from the unpack's own piece (heap: DMA-capable, aligned); a part KiB is padded in wb */
        const uint8_t *src = b + q;
        if (k < 1024u || ((uintptr_t)src & 3u)) {
            if (!z->wb && !(z->wb = heap_caps_malloc(1024, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL))) return -3;
            memcpy(z->wb, b + q, k); if (k < 1024u) memset(z->wb + k, 0xFF, 1024u - k); src = z->wb;
        }
        const uint32_t kib = (off + q) >> 10, m0 = z->misses;
        const int v = z->all || kib < 16u || (kib & 63u) == 0;
        if (v) ahead_done();                             /* (the read-back's RREAD uses the window too) */
        int e = 0;
        for (int t = 0; t < 6; t++) {
            /* an op that did not come back (right after receive mode the link starts again on the same UART, unit 2
               2026-10-22: the first read-back failed): a moment, the FPGA's sticky error cleared, the hold again */
            e = sd_ram_write(CF_RC_ROM_BASE + off + q, src, z->chk, v, &z->checked, &z->misses);
            if (!e) break;
            z->op_errs++; z->last_err = e;
            vTaskDelay(pdMS_TO_TICKS(100 * (t + 1)));
            if (e > 0 || e == -1) { (void)CfRc_Reset(); (void)cfg_now(CF_RC_A_HOLD); }
        }
        if (e) return e;
        if (z->misses != m0) z->all = 1;
    }
    z->ms_wr += (uint32_t)((esp_timer_get_time() - t) / 1000);
    _P.pct = (int)((uint64_t)(off + n) * 100u / (_P.rom_banks ? (uint32_t)_P.rom_banks * 0x4000u : 1u));
    return 0;
}
/* 0 = the game is in PSRAM (h filled); else a cf_sgz error */
static int z_to_psram2(int (*rdf)(void *u, uint32_t off, uint8_t *b, uint32_t n), int (*rd)(void *u, uint32_t off, uint8_t *b, uint32_t n),
                       void *ru, uint32_t total, cf_sgz_hdr_t *h);
static int z_to_psram(int (*rd)(void *u, uint32_t off, uint8_t *b, uint32_t n), void *ru, uint32_t total, cf_sgz_hdr_t *h) { return z_to_psram2(NULL, rd, ru, total, h); }
static int z_to_psram2(int (*rdf)(void *u, uint32_t off, uint8_t *b, uint32_t n), int (*rd)(void *u, uint32_t off, uint8_t *b, uint32_t n),
                       void *ru, uint32_t total, cf_sgz_hdr_t *h)
{
    zp_t z = { .rd = rd, .rdf = rdf, .ru = ru };
    z.wb = NULL;                                        /* (only for a part KiB at the end: whole ones go from the unpack's piece) */
    z.chk = NULL;                                       /* (gamecomp1: read-backs compared in the window: a KiB of heap back for the unpack) */
    if (_Snap) {                                        /* (the page's copy of the list: made again when it looks) */
        portENTER_CRITICAL(&_LibMux);
        void *sn = _Snap; _Snap = NULL;
        portEXIT_CRITICAL(&_LibMux);
        free(sn);
    }
    int e = CF_SGZ_E_MEM;
    const uint32_t fh = esp_get_free_heap_size(), big = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    const int64_t t0 = esp_timer_get_time();
    for (int t = 0; t < 12; t++) {     /* (right after receive mode the link takes its heap back: a few tries) */
        {
            uint8_t q[8] = { 0 };
            const int re = rd(ru, 0, q, sizeof q);
            printf("rc: SGZ1 try %d: header %02x %02x %02x %02x (read e=%d)\n", t + 1, q[0], q[1], q[2], q[3], re);
        }
        e = rdf ? cf_sgz_unpack2(zp_rdf, zp_rd, zp_wr, &z, total, h, NULL) : cf_sgz_unpack(zp_rd, zp_wr, &z, total, h, NULL);
        CfDev_FlashMmDrop();
        if (e) printf("rc: SGZ1 try %d: %s (free heap %lu, largest %lu; failed allocation %08lx)\n", t + 1, cf_sgz_why(e), (unsigned long)esp_get_free_heap_size(),
                      (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), (unsigned long)cf_sgz_prof.mem_at);
        if (e == CF_SGZ_E_MEM) printf("rc: SGZ1 heap: 8-bit %lu (largest %lu), internal %lu, 32-bit only %lu\n", (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                                      (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                                      (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_32BIT) - heap_caps_get_free_size(MALLOC_CAP_8BIT)));
        if (e != CF_SGZ_E_MEM) break;
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    const uint32_t ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    if (e == CF_SGZ_E_MAGIC || e == CF_SGZ_E_HEADER) {
        uint8_t q[16] = { 0 };
        const int re = rd(ru, 0, q, sizeof q);
        printf("rc: SGZ1: the first bytes as read (e=%d): %02x %02x %02x %02x %02x %02x %02x %02x, total %lu\n", re, q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7], (unsigned long)total);
    }
    printf("rc: SGZ1 pieces of %u bytes\n", 1u << cf_sgz_prof.seg_shift);
    printf("rc: window reads %lu, reads that differed %lu, kept after a retry %lu; checked fills %lu, filled again %lu\n", (unsigned long)_WinReads,
           (unsigned long)_WinDiff, (unsigned long)_WinFixed, (unsigned long)_RrFills, (unsigned long)_RrRedo);
    printf("rc: SGZ1 time: reading %lld ms, inflating %lld ms, crc32 + sha256 %lld ms (sha256 %lld), into PSRAM %lld ms; blocks read again %lu\n", (long long)(cf_sgz_prof.read_us / 1000),
           (long long)(cf_sgz_prof.inflate_us / 1000), (long long)(cf_sgz_prof.check_us / 1000), (long long)(cf_sgz_prof.sha_us / 1000), (long long)(cf_sgz_prof.write_us / 1000), (unsigned long)cf_sgz_prof.retries);
    printf("rc: SGZ1 %lu -> %lu bytes into PSRAM: %s in %lu ms (PSRAM writes + read-backs %lu ms; %lu KiB read back, %lu wrong; %lu op errors, last %d; free heap before %lu, largest %lu)\n",
           (unsigned long)total, (unsigned long)(e ? 0 : h->rom_size), cf_sgz_why(e), (unsigned long)ms, (unsigned long)z.ms_wr, (unsigned long)z.checked,
           (unsigned long)z.misses, (unsigned long)z.op_errs, z.last_err, (unsigned long)fh, (unsigned long)big);
    heap_caps_free(z.wb); heap_caps_free(z.chk);
    return e;
}
static int zf_rd(void *u, uint32_t off, uint8_t *b, uint32_t n) { return CfDev_FlashReadMm((uint32_t)(uintptr_t)u + off, b, n); }
int CfRc_ScratchRead(uint32_t off, uint8_t *b, uint32_t n);
static int sh_rd_scratch(void *u, uint32_t off, uint8_t *b, uint32_t n) { (void)u; return CfRc_ScratchRead(off, b, n) ? -1 : 0; }   /* lane share1 */
int CfRc_ScratchReadFast(uint32_t off, uint8_t *b, uint32_t n, uint32_t len);
static int sh_rd_scratch_fast(void *u, uint32_t off, uint8_t *b, uint32_t n) { (void)u; return CfRc_ScratchReadFast(off, b, n, _RamGame.size) ? -1 : 0; }

static int play_sd(void);
static int play_now_inner(void);
static bool load_quiet_begin(const char *what);
static void load_quiet_end(bool held);
/* m110s: a game in PSRAM that did not start (a share's PLAY NOW, a Wi-Fi Just play it): the core was held since the bytes
   came (CfRc_RamBegin), and nothing let it go, so the cartridge's game stayed stopped and the screen had no picture until
   the FPGA was reloaded (unit 2, m110f). Let go now, as CfRc_RamRelease does */
static void core_back(const char *why)
{
    printf("[%u ms] rc: core back: start (reason %s)\n", (unsigned)esp_log_timestamp(), why);
    if (_Src == 2 && (_SdKeepOn || !CfRc_CartIn())) {
        _SdKeepOn = 1;
        const uint8_t flags = CF_RC_A_ON | CF_RC_A_HOLD | CF_RC_A_MRESET | CF_RC_A_INVAL;
        const int e = cfg_now(flags);
        printf("[%u ms] rc: core back: keep slot off (flags 0x%02x, result %d)\n", (unsigned)esp_log_timestamp(), flags, e);
        return;
    }
    const int hold = CfRc_Cfg(0, 0, 0, 0, CF_RC_A_HOLD);
    vTaskDelay(pdMS_TO_TICKS(300));
    const int release = CfRc_Cfg(0, 0, 0, 0, 0);
    printf("[%u ms] rc: core back: end (hold %d, release %d)\n", (unsigned)esp_log_timestamp(), hold, release);
    printf("rc: play: the game did not start: back to the cartridge\n");
}
static int play_now(void)
{
    (void)CwlTime_WaitForLoad(3000);              /* 1.1.7: a clock game starts with the real time when Wi-Fi is joining (up to 3 s), else without it */
    if (_Src == 2) return play_sd();
    if (_Src == 3) return play_lib();
    const bool held = load_quiet_begin("play");
    const int r = play_now_inner();
    if (r && (_Src == 1 || _Src == 5) && _P.st == kNwPv_Result) core_back("kept game failed to start");
    load_quiet_end(held);
    return r;
}
/* m110s: the header of a game in PSRAM's ROM area, read with the core let go and read again while its check byte is wrong
   (cf_rc_hdr.inc, also built into the Mac test host/cf_hdr_test.c) */
static int hd_read(uint8_t *h, uint32_t n) { return CfRc_PsramRead(CF_RC_ROM_BASE, h, n); }
static void hd_let_go(void) { (void)CfRc_Cfg(0, 0, 0, 0, 0); }
static void hd_delay_ms(int ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }
static void hd_say(const char *m) { printf("rc: play: %s\n", m); }
#include "cf_rc_hdr.inc"
static int ram_hdr_read(uint8_t *hdr, uint32_t n) { return hd_ram_header(hdr, n); }
/* m110t: a trimmed game: the ROM area past its file filled with FF up to its header's size (cf_rc_pad.inc, also built into
   the Mac test host/cf_pad_test.c). Called with the core held, right before it starts */
static int pd_write(uint32_t off, const uint8_t *kib, int verify) { uint32_t c = 0, m = 0; return sd_ram_write(CF_RC_ROM_BASE + off, kib, NULL, verify, &c, &m); }
static int pd_read(uint32_t off, uint8_t *kib)
{
    if (_Src == 2 && _SdKeepOn) return CfRc_PsramRead(CF_RC_ROM_BASE + off, kib, 1024);   /* keep ON|HOLD during card staging */
    if (CfRc_Cfg(0, 0, 0, 0, 0)) return -1;              /* (let go: a window read with the core held can come a word late) */
    const int e = CfRc_PsramRead(CF_RC_ROM_BASE + off, kib, 1024);
    return cfg_now(CF_RC_A_HOLD) ? -1 : e;
}
#include "cf_rc_pad.inc"
static int rom_pad(uint32_t have, const char *title)
{
    if (have >= (uint32_t)_P.rom_banks * 0x4000u) return 0;
    uint8_t *b = NULL;
    for (int t = 0; t < 10 && !(b = heap_caps_malloc(1024, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)); t++) vTaskDelay(pdMS_TO_TICKS(100));
    if (!b) { printf("rc: %s: no KiB to fill the rest of its banks\n", title); return -1; }
    uint32_t kib = 0;
    const int64_t t0 = esp_timer_get_time();
    const int e = pd_rom_pad(have, _P.rom_banks, b, &kib);
    heap_caps_free(b);
    printf("rc: %s: %lu bytes for a %lu-byte header: the rest read as FF (%lu KiB filled in %lld ms): e=%d\n", title, (unsigned long)have,
           (unsigned long)_P.rom_banks * 0x4000u, (unsigned long)kib, (long long)((esp_timer_get_time() - t0) / 1000), e);
    return e;
}
static int play_now_inner(void)
{
    static uint8_t hdr[0x150];
    const char *why = "";
    const int inram = _InRam;
    const int scr = _Src == 5;                               /* lane share1: an SGZ1 container in PSRAM scratch (a share's PLAY NOW) */
    const int given = _SaveGiven;
    _SaveGiven = 0;
    rc_game_t *const kg = _Src == 4 ? &_TmpGame : &_Game;   /* lane gamecomp1: 4 = a game staged by a send, played but not kept */
    const rc_game_t *g = (inram || scr) ? &_RamGame : kg;
    if (!inram && !scr && _Src != 4 && !_GameOk) { result(kNwPv_Result, "NO  GAME  KEPT", ""); return -1; }
    {
        unsigned maj = 0, min = 0;
        extern int CfDev_FpgaVersion(unsigned *maj, unsigned *min);
        if (!CfDev_FpgaVersion(&maj, &min) || maj != 18 || min < CF_RC_FPGA_MINOR || CfDev_FpgaRecovery()) { result(kNwPv_NoFpga, "", ""); return -1; }
    }
    if (scr ? CfRc_ScratchRead(0, hdr, CF_SGZ_HDR) != 0 : inram ? ram_hdr_read(hdr, sizeof hdr) != 0 : esp_flash_read(NULL, hdr, kg->base, sizeof hdr) != ESP_OK) { result(kNwPv_Result, "CANNOT  READ", "THE  GAME"); return -1; }
    /* lane gamecomp1: a kept game stored as an SGZ1 container: its header has the game's cart bytes and size */
    cf_sgz_hdr_t zh;
    const int zk = scr || (!inram && cf_sgz_is(hdr, sizeof hdr));
    uint32_t gsize = g->size;
    if (zk) {
        if (cf_sgz_parse(hdr, scr ? _RamGame.size : kg->size, &zh)) { result(kNwPv_Result, scr ? "THE  GAME  IS" : "THE  GAME  WAS", scr ? "DAMAGED" : "REPLACED"); if (_Src == 0) game_store(0); return -1; }
        memset(hdr, 0, sizeof hdr);
        memcpy(hdr + 0x134, zh.cart, sizeof zh.cart);
        gsize = zh.rom_size;
    }
    if (header_parse(hdr, gsize, &why)) { result(kNwPv_Result, "CANNOT  PLAY  IT", why); return -1; }
    _P.st = kNwPv_Loading; _P.pct = 0;
    if (CfRc_Reset() != 0) { result(kNwPv_Result, "IT  DID  NOT  START", "TRY  AGAIN"); return -1; }
    if (cfg_now(CF_RC_A_HOLD)) { result(kNwPv_Result, "IT  DID  NOT  START", "TRY  AGAIN"); return -1; }
    /* the game, a KiB at a time, each read back (in PSRAM already: it was checked as it came, by KiB and as a whole) */
    const uint32_t len = (g->size + 1023u) & ~1023u;
    uint32_t crc = inram ? g->crc : 0;
    if (zk) {
        /* lane share1: scratch is read with the core let go (RAM cart off, not held): with the core held a window read now and then
           comes a word late, the same way twice (gamecomp1 10-22: cfg 0 0 0 2 -> ~5.6 KB per MiB wrong, cfg flags 0 -> none) */
        if (scr && CfRc_Cfg(0, 0, 0, 0, 0)) { result(kNwPv_Result, "IT  DID  NOT  START", "TRY  AGAIN"); return -1; }
        /* (the blocks by timed window reads, a block that does not check read again the checked way, as a library load) */
        const int ze = scr ? z_to_psram2(sh_rd_scratch_fast, sh_rd_scratch, NULL, _RamGame.size, &zh) : z_to_psram(zf_rd, (void *)(uintptr_t)kg->base, kg->size, &zh);
        if (scr) ahead_done();
        if (scr && cfg_now(CF_RC_A_HOLD)) { result(kNwPv_Result, "IT  DID  NOT  START", "TRY  AGAIN"); return -1; }
        if (ze) {
            cfg_now(0);
            if (ze == CF_SGZ_E_WRITE || ze == CF_SGZ_E_MEM || ze == CF_SGZ_E_READ) { result(kNwPv_Result, "LOADING  FAILED", "TRY  AGAIN"); return -1; }
            result(kNwPv_Result, "THE  GAME  WAS", "REPLACED"); if (_Src == 0) game_store(0); return -1;   /* (the staged bytes are not the game any more) */
        }
        crc = zh.rom_crc32;
    }
    for (uint32_t o = 0; o < len && !inram && !zk; o += 0x10000u) {
        const uint32_t k = len - o < 0x10000u ? len - o : 0x10000u;
        uint32_t c = 0;
        if (CfRc_LoadFromFlash(kg->base + o, CF_RC_ROM_BASE + o, k, &c)) { cfg_now(0); result(kNwPv_Result, "LOADING  FAILED", "TRY  AGAIN"); return -1; }
        _P.pct = (int)((uint64_t)(o + k) * 100u / len);
    }
    /* the whole game's crc32 from the flash copy (what the send said it is) */
    if (!inram && !zk) {
        static uint8_t b[1024];
        for (uint32_t o = 0; o < kg->size; o += 1024u) {
            const uint32_t k = kg->size - o < 1024u ? kg->size - o : 1024u;
            if (esp_flash_read(NULL, b, kg->base + o, k) != ESP_OK) break;
            crc = crc32_step(crc, b, k);
        }
    }
    if (!inram && !zk && kg->crc && crc != kg->crc) { cfg_now(0); result(kNwPv_Result, "THE  GAME  WAS", "REPLACED");   /* (a USB install wrote the slot) */ if (_Src == 0) game_store(0); return -1; }
    _P.crc = crc;
    memset(&_Clk, 0, sizeof _Clk);
    /* its save: from the store, else blank */
    {
        static uint8_t b[1024];
        (void)index_load();
        _P.ent = _P.battery && _P.sav_size ? ent_for(crc, _P.sav_size, 1) : -1;
        const int have = _P.ent >= 0 && _Ix.e[_P.ent].dcrc != 0 && !given;
        for (uint32_t o = 0; !given && o < ((_P.sav_size + 1023u) & ~1023u); o += 1024u) {
            if (have) { if (esp_flash_read(NULL, b, store_base() + (uint32_t)_Ix.e[_P.ent].sect * 4096u + o, 1024) != ESP_OK) memset(b, 0xFF, sizeof b); }
            else memset(b, 0xFF, sizeof b);
            if (OSD_QspiPsramWrite(CF_RC_RAM_BASE + o, b)) { cfg_now(0); result(kNwPv_Result, "LOADING  FAILED", "TRY  AGAIN"); return -1; }
        }
        if (_P.ent >= 0) {
            static uint32_t lru;
            _Ix.e[_P.ent].lru = ++lru + _Ix.seq;
            if (!have) (void)index_save();
            if (_P.rtc) {                                  /* a new save: the clock from 0; else as it was left, caught up to now (1.1.7) */
                uint8_t r[5] = { 0 };
                if (have) memcpy(r, _Ix.e[_P.ent].rtc, 5);
                clk_loaded(r, have ? ix_tm(_P.ent) : 0);
                (void)CfRc_Op(CF_RC_O_RRTC, (uint32_t)r[0] | (uint32_t)r[1] << 8 | (uint32_t)r[2] << 16, (uint32_t)r[3] | (uint32_t)r[4] << 8, 1, 200);
            }
        }
        if (given && _P.ent >= 0) {
            /* lane share1: the save came with the game (in the save RAM already): into this unit's store now, so it stays
               even if the game is stopped before it saves */
            const int se = save_copy_store();
            printf("rc: %s: the save that came with it put in the store: e=%d\n", g->title, se);
        }
        printf("rc: %s: save %lu bytes %s\n", g->title, (unsigned long)_P.sav_size, given ? "that came with the game" : have ? "from the store" : "blank");
    }
    if (rom_pad(gsize, g->title)) { cfg_now(0); result(kNwPv_Result, "LOADING  FAILED", "TRY  AGAIN"); return -1; }
    if (cfg_now(CF_RC_A_ON | CF_RC_A_HOLD | CF_RC_A_INVAL | CF_RC_A_CLEAN)) { cfg_now(0); result(kNwPv_Result, "IT  DID  NOT  START", "TRY  AGAIN"); return -1; }
    vTaskDelay(pdMS_TO_TICKS(50));
    if (cfg_now(CF_RC_A_ON)) { result(kNwPv_Result, "IT  DID  NOT  START", "TRY  AGAIN"); return -1; }
    _P.pend = 0; _P.saves = 0;
    _P.st = kNwPv_Playing;
    OSD_SetVisiblityState(false);              /* (m110: the core restarted: the menu is closed) */
    printf("rc: playing %s (mapper %u, %u ROM banks, %u RAM banks%s) with no cartridge%s\n", g->title, _P.mbc, _P.rom_banks, _P.ram_banks, _P.rtc ? ", clock" : "",
           inram ? " (from Wi-Fi, in RAM only)" : "");
    return 0;
}

/* ================================================================ lane sdcard1: a game played from the microSD card
 *
 * The game file is read from the card 8 KiB at a time and written into PSRAM 1 KiB per QSPI write (the
 * RAM cart's ROM area: up to 4 MiB). Read-back (one cart_xs RREAD per KiB over the 115200 baud FPGA
 * link: ~13 ms a KiB) is the slow part, so by default only the first 16 KiB and every 64th KiB after it are
 * read back (a bench of 4096 + 1000 KiB read back after blind writes found no KiB wrong) (`rc sdverify`: 0 none, 1 that, 2 every KiB, 3 every KiB after a blind write of the whole
 * game: the bench's error count); a KiB that reads back wrong is written again and every KiB after
 * it is then read back.
 * Its save: /SAVES/<the game file's name without its extension>.sav on the card (raw save RAM, and the
 * MBC3 clock's 48-byte footer for a clock game: the emulators' format), written as the 1.1.0 store's
 * copies are (1.5 s after the game's last save RAM write, at most every 10 s, and when it stops), crash
 * safe (sd_file.c). A copy that is the same as the card's is not written. A card that cannot take it
 * (pulled, full): the copy goes to this unit's save store (the 1.1.0 one, by the game's crc32, up to
 * 60 KiB) marked "not on the card yet", and the card is tried again every 8 s; the next play of the
 * game from a card takes the store's copy.
 */
#define RC_SD_CHUNK    8192u
#define RC_SD_RETRY_MS 8000u
static volatile int _SaveBusy;
static volatile int64_t _PollUs;     /* lane savefix1: esp_timer time (runs through light sleep) of the last save poll */
#define RC_ENT_PENDING 2u                /* rc_ent_t.used: a save the card has not got yet */
static char    _SdPath[SDC_PATH_MAX];
/* (_Src, above: the game playing / asked for: 0 kept (flash), 1 in RAM (Wi-Fi), 2 the card) */
static uint8_t _SdVerify = 1;
static struct { uint32_t crc; uint8_t rtc[5]; uint8_t fail, have; uint32_t retry_at; } _Sv;
static int _SdStopping;              /* the copy at the stop: always tried, the clock always written */
static struct { uint32_t ms, kib, checked, misses, ms_card, ms_ram; } _SdLoad;   /* the last load (bench) */

static int sd_ram_write(uint32_t a, const uint8_t *b, uint8_t *chk, int verify, uint32_t *checked, uint32_t *misses)
{
    for (int t = 0; t < 4; t++) {
        RC_MARK();
        if (OSD_QspiPsramWrite(a, b)) return -4;
        RC_MARK();
        if (!verify) return 0;
        vTaskDelay(1);                                  /* the 1 KiB burst lands */
        int same;
        if (chk) {
            const int e = CfRc_PsramRead(a, chk, 1024);
            if (e) return e;
            same = !memcmp(b, chk, 1024);
        } else {                                        /* gamecomp1: no buffer of its own (the unpack's heap): compared in the window */
            if (!buf_ok()) return -1;
            const int e = CfRc_Op(CF_RC_O_RREAD, a, 1024, 0, 200);
            if (e) return e;
            { const int we = win_read_sure(1024); if (we) return we; }
            same = !memcmp(b, _Buf, 1024);
        }
        (*checked)++;
        if (same) return 0;
        (*misses)++;
    }
    printf("rc: sd: PSRAM %06lx does not read back the same\n", (unsigned long)a);
    return -5;
}

/* the save from the card (or the store's newer copy) into PSRAM, the clock set. 0 = there */
static int sd_save_load(uint8_t *buf, uint8_t *chk, FIL *f)
{
    memset(&_Sv, 0, sizeof _Sv);
    if (!_P.battery || (!_P.sav_size && !_P.rtc)) return 0;
    char sp[SDC_PATH_MAX + 8];
    if (sdc_save_path(_SdPath, sp, sizeof sp)) return 0;
    uint32_t ssz = 0, crc = 0, dummy = 0;
    const uint32_t blocks = (_P.sav_size + 1023u) & ~1023u;
    int src = 0;                                        /* 0 blank, 1 the card, 2 the store */
    uint64_t t_saved = 0;
    uint8_t rt[5] = { 0 };
    (void)index_load();
    const int ent = ent_for(_P.crc, _P.sav_size, 0);
    if (ent >= 0 && _Ix.e[ent].used == RC_ENT_PENDING && _Ix.e[ent].dcrc) src = 2;
    if (!src && Sd_ReadOpen(f, sp, &ssz) == 0) src = 1;
    for (uint32_t o = 0; o < blocks; o += 1024u) {
        memset(buf, 0xFF, 1024);
        if (src == 1 && o < ssz && o < _P.sav_size) {
            uint32_t got = 0;
            const uint32_t k = _P.sav_size - o < 1024u ? _P.sav_size - o : 1024u;
            (void)Sd_Read(f, o, buf, k, &got);
        } else if (src == 2 && esp_flash_read(NULL, buf, store_base() + (uint32_t)_Ix.e[ent].sect * 4096u + o, 1024) != ESP_OK) memset(buf, 0xFF, 1024);
        crc = cf_crc32(crc, buf, _P.sav_size - o < 1024u ? _P.sav_size - o : 1024u);
        if (sd_ram_write(CF_RC_RAM_BASE + o, buf, chk, 1, &dummy, &dummy)) { if (src == 1) Sd_Close(f); return -1; }
    }
    if (src == 1 && _P.rtc && ssz >= _P.sav_size + 44u) {
        uint32_t got = 0;
        const uint32_t fl = ssz - _P.sav_size >= 48u ? 48u : 44u;
        if (Sd_Read(f, _P.sav_size, buf, fl, &got) == 0 && got == fl && sdc_rtc_get(buf, fl, rt, &t_saved) == 0) _Sv.have = 1;
    }
    if (src == 2) { memcpy(rt, _Ix.e[ent].rtc, 5); t_saved = ix_tm(ent); _Sv.have = 1; }
    if (src == 1) Sd_Close(f);
    memset(&_Clk, 0, sizeof _Clk);
    if (_P.rtc) {
        clk_loaded(rt, _Sv.have ? t_saved : 0);                  /* (1.1.7: the time it spent on the card, if the real time is known) */
        (void)CfRc_Op(CF_RC_O_RRTC, (uint32_t)rt[0] | (uint32_t)rt[1] << 8 | (uint32_t)rt[2] << 16, (uint32_t)rt[3] | (uint32_t)rt[4] << 8, 1, 200);
    }
    _Sv.crc = src ? crc : 0;
    memcpy(_Sv.rtc, rt, 5);
    _Sv.fail = src == 2;                               /* the store's copy: onto the card at the first chance */
    printf("rc: sd: save %lu bytes %s%s\n", (unsigned long)_P.sav_size, src == 1 ? "from the card " : src == 2 ? "from this unit's store (not on the card yet)" : "blank",
           src == 1 ? sp : "");
    return 0;
}

/* the save RAM (and the clock) into its file on the card; 0 = the card has it */
static int sd_save_card(uint8_t *a, uint32_t crc_want, const uint8_t rt[5])
{
    char sp[SDC_PATH_MAX + 8];
    if (sdc_save_path(_SdPath, sp, sizeof sp)) return -1;
    if (Sd_Mount() != 0 || !Sd_Writable()) return -2;
    sd_wr_t *w = malloc(sizeof *w);
    if (!w) return -3;
    int e = Sd_WriteBegin(w, sp, 0);
    uint32_t crc = 0;
    for (uint32_t o = 0; !e && o < _P.sav_size; o += 1024u) {
        const uint32_t k = _P.sav_size - o < 1024u ? _P.sav_size - o : 1024u;
        if (CfRc_PsramRead(CF_RC_RAM_BASE + o, a, 1024)) { e = -4; break; }
        crc = cf_crc32(crc, a, k);
        e = Sd_WriteAt(w, o, a, k);
    }
    if (!e && _P.rtc) {
        uint8_t ft[SDC_RTC_FOOTER], kp[5];
        memcpy(kp, rt, 5);
        const uint64_t tk = clk_keep(kp);                       /* (1.1.7: the real time, or the last known one plus the time played) */
        sdc_rtc_put(ft, kp, tk);
        e = Sd_WriteAt(w, _P.sav_size, ft, sizeof ft);
    }
    if (!e && crc != crc_want) e = -6;                  /* (the game wrote while it was copied: the next copy has it) */
    if (!e) e = Sd_WriteCommit(w);
    else if (w->open) Sd_WriteAbort(w);
    free(w);
    return e;
}

static int save_copy_store(void);
static int save_copy_sd(void)
{
    printf("[%u ms] rc: save copy SD: start (%lu bytes)\n", (unsigned)esp_log_timestamp(), (unsigned long)_P.sav_size);
    if (!_P.battery || (!_P.sav_size && !_P.rtc)) { printf("[%u ms] rc: save copy SD: end (result 0, no save data)\n", (unsigned)esp_log_timestamp()); return 0; }
    const uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (_Sv.fail && !_SdStopping && (int32_t)(now - _Sv.retry_at) < 0) { printf("[%u ms] rc: save copy SD: end (result 1, retry deferred)\n", (unsigned)esp_log_timestamp()); return 1; }   /* (the card was not there: tried again later) */
    uint8_t *a = heap_caps_malloc(1024, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!a) { printf("[%u ms] rc: save copy SD: buffer allocation failed (-1)\n", (unsigned)esp_log_timestamp()); printf("[%u ms] rc: save copy SD: end (result -1)\n", (unsigned)esp_log_timestamp()); return -1; }
    uint32_t crc = 0;
    int e = 0;
    for (uint32_t o = 0; !e && o < _P.sav_size; o += 1024u) {
        const uint32_t k = _P.sav_size - o < 1024u ? _P.sav_size - o : 1024u;
        const int read = CfRc_PsramRead(CF_RC_RAM_BASE + o, a, 1024);
        if (read) { e = -4; printf("[%u ms] rc: save copy SD: PSRAM read failed at %lu (call %d, result %d)\n", (unsigned)esp_log_timestamp(), (unsigned long)o, read, e); }
        else crc = cf_crc32(crc, a, k);
    }
    uint8_t rt[5];
    memcpy(rt, _Sv.rtc, 5);
    const int rtc_op = !e && _P.rtc ? CfRc_Op(CF_RC_O_RRTC, 0, 0, 0, 200) : 0;
    if (!e && _P.rtc && rtc_op == 0) {
        const uint32_t r = CfRc_Crc();
        rt[0] = (uint8_t)(r & 0x3F); rt[1] = (uint8_t)((r >> 8) & 0x3F); rt[2] = (uint8_t)((r >> 16) & 0x1F); rt[3] = (uint8_t)(r >> 24); rt[4] = _St[16];
    }
    if (!e && _P.rtc && rtc_op != 0) printf("[%u ms] rc: save copy SD: RTC read failed (result %d)\n", (unsigned)esp_log_timestamp(), rtc_op);
    if (e) { heap_caps_free(a); printf("[%u ms] rc: save copy SD: end (result %d)\n", (unsigned)esp_log_timestamp(), e); return e; }
    /* the clock alone moves every second: a clock game's copy is written for its save RAM, and at the stop */
    const int changed = crc != _Sv.crc || (_P.rtc && _SdStopping && memcmp(rt, _Sv.rtc, 5));
    if (!changed && !_Sv.fail) { heap_caps_free(a); _P.saves++; _P.last_save = xTaskGetTickCount() * portTICK_PERIOD_MS; printf("[%u ms] rc: save copy SD: end (result 0, unchanged)\n", (unsigned)esp_log_timestamp()); return 0; }
    const int64_t t0 = esp_timer_get_time();
    e = sd_save_card(a, crc, rt);
    heap_caps_free(a);
    if (e == 0) {
        printf("rc: sd: save on the card (%lu bytes, crc %08lx, %lld ms)\n", (unsigned long)_P.sav_size, (unsigned long)crc, (long long)(esp_timer_get_time() - t0) / 1000);
        if (_Sv.fail) {                                 /* the store's copy is on the card now */
            (void)index_load();
            const int ent = ent_for(_P.crc, _P.sav_size, 0);
            if (ent >= 0 && _Ix.e[ent].used == RC_ENT_PENDING) { _Ix.e[ent].used = 1; (void)index_save(); }
        }
        _Sv.crc = crc; memcpy(_Sv.rtc, rt, 5); _Sv.fail = 0;
        _P.saves++;
        _P.last_save = xTaskGetTickCount() * portTICK_PERIOD_MS;
        printf("[%u ms] rc: save copy SD: end (result 0)\n", (unsigned)esp_log_timestamp());
        return 0;
    }
    printf("[%u ms] rc: save copy SD: card save failed (result %d)\n", (unsigned)esp_log_timestamp(), e);
    if (e == -6) { printf("[%u ms] rc: save copy SD: end (result 1, changed during copy)\n", (unsigned)esp_log_timestamp()); return 1; }                              /* the game wrote meanwhile: copied again soon */
    /* not on the card: this unit's store keeps it until the card takes it */
    _Sv.retry_at = now + RC_SD_RETRY_MS;
    printf("rc: sd: the card did not take the save (%d): kept in this unit until it does\n", e);
    _Sv.fail = 1;
    if (_P.sav_size <= 15u * 4096u) {
        (void)index_load();
        _P.ent = ent_for(_P.crc, _P.sav_size, 1);
        if (_P.ent >= 0) {
            if (save_copy_store() == 0) {
                uint8_t kp[5];
                memcpy(kp, rt, 5);
                _Ix.tm[_P.ent] = (uint32_t)clk_keep(kp);
                memcpy(_Ix.e[_P.ent].rtc, kp, 5);
                _Ix.e[_P.ent].used = RC_ENT_PENDING;
                (void)index_save();
            }
        }
        _P.ent = -1;
    }
    printf("[%u ms] rc: save copy SD: end (result -2)\n", (unsigned)esp_log_timestamp());
    return -2;
}

/* the game file into PSRAM, its save, the RAM cart on */
static int play_sd(void)
{
    printf("[%u ms] rc: play SD: start (%s)\n", (unsigned)esp_log_timestamp(), _SdPath);
    unsigned maj = 0, min = 0;
    extern int CfDev_FpgaVersion(unsigned *maj, unsigned *min);
    if (!CfDev_FpgaVersion(&maj, &min) || maj != 18 || min < CF_RC_FPGA_MINOR || CfDev_FpgaRecovery()) { result(kNwPv_NoFpga, "", ""); return -1; }
    _P.st = kNwPv_Loading; _P.pct = 0;
    printf("[%u ms] rc: play SD: mount start\n", (unsigned)esp_log_timestamp());
    const int mount = Sd_Mount();
    printf("[%u ms] rc: play SD: mount end (result %d)\n", (unsigned)esp_log_timestamp(), mount);
    if (mount != 0) { result(kNwPv_Result, "NO  CARD", "PUT  THE  CARD  BACK"); return -1; }
    /* the buffers: 8 KiB card reads when the heap has them, down to 2 KiB (right after receive mode the link is
       taking its heap back), a few tries a moment apart */
    FIL *f = NULL;
    uint8_t *buf = NULL, *chk = NULL;
    uint32_t chunk = RC_SD_CHUNK;
    for (int t = 0; t < 20 && (!f || !buf || !chk); t++) {
        if (!f) f = malloc(sizeof *f);
        if (!chk) chk = heap_caps_malloc(1024, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        for (chunk = RC_SD_CHUNK; !buf && chunk >= 2048u; chunk /= 2) if ((buf = heap_caps_malloc(chunk, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL))) break;
        if (!f || !buf || !chk) vTaskDelay(pdMS_TO_TICKS(250));
    }
    int rc = -1, started = 0;                          /* fr1: started: the core was touched (reset / held) */
    uint32_t size = 0, got = 0;
    const char *why = "";
    if (!f || !buf || !chk) { result(kNwPv_Result, "NOT  ENOUGH", "MEMORY"); goto out; }
    if (Sd_ReadOpen(f, _SdPath, &size) != 0) { printf("rc: sd: no game at \"%s\"\n", _SdPath); result(kNwPv_Result, "CANNOT  READ", "THE  GAME"); goto out; }
    if (Sd_Read(f, 0, buf, 0x150, &got) || got != 0x150) { Sd_Close(f); result(kNwPv_Result, "CANNOT  READ", "THE  GAME"); goto out; }
    {
        sdc_hdr_t h;
        (void)sdc_hdr_parse(buf, size, &h);
        memset(&_RamGame, 0, sizeof _RamGame);
        sdc_display_name(h.title, _SdPath, _RamGame.title, sizeof _RamGame.title);
        _RamGame.size = size;
    }
    if (header_parse(buf, size, &why)) { Sd_Close(f); result(kNwPv_Result, "CANNOT  PLAY  IT", why); goto out; }
    _P.st = kNwPv_Loading; _P.pct = 0;
    /* fr1: a card game played right after another one stopped (GAMES > PLAY while one plays): the stop's save copy may have
       left the FPGA's op engine busy, and one RESET then failed: the game did not start and the screen stayed white with no
       cartridge, until the unit was switched off. RESET until the engine is idle (as after an ESP32 restart), then hold */
    printf("[%u ms] rc: play SD: RESET/HOLD start\n", (unsigned)esp_log_timestamp());
    const int reset = CfRc_ResetIdle(1500);
    /* The stop already wrote the old game's ON|HOLD. Header parsing has changed _P;
       leave that RCFG untouched until staging is complete. */
    /* An empty cart released by Stop needs HOLD again before the next card load. */
    const int hold = reset == 0 ? (_SdKeepOn ? ((_CfgFlags & CF_RC_A_HOLD) ? 0 :
        CfRc_Cfg(0, 2, 0, 0, CF_RC_A_ON | CF_RC_A_HOLD)) : cfg_now(CF_RC_A_HOLD)) : -1;
    if (_SdKeepOn) printf("[%u ms] rc: play SD: retain old config (flags 0x03, reset result %d)\n", (unsigned)esp_log_timestamp(), reset);
    printf("[%u ms] rc: play SD: RESET/HOLD end (reset %d, hold %d)\n", (unsigned)esp_log_timestamp(), reset, hold);
    if (reset != 0 || hold) {
        printf("rc: sd: the FPGA did not get ready for %s (status %02x %02x)\n", _SdPath, _St[1], _St[3]);
        Sd_Close(f); started = 1; result(kNwPv_Result, "IT  DID  NOT  START", "TRY  AGAIN"); goto out;
    }
    started = 1;
    {
        const int64_t t0 = esp_timer_get_time();
        _SdLoad.ms_card = _SdLoad.ms_ram = 0;
        const uint32_t len = (size + 1023u) & ~1023u;
        uint32_t crc = 0, checked = 0, misses = 0;
        int all = _SdVerify == 2, e = 0;
        printf("[%u ms] rc: play SD: ROM staging start (%lu bytes)\n", (unsigned)esp_log_timestamp(), (unsigned long)size);
        for (uint32_t o = 0; o < len && !e; o += chunk) {
            const uint32_t k = len - o < chunk ? len - o : chunk;
            got = 0;
            const int64_t tr = esp_timer_get_time();
            if (Sd_Read(f, o, buf, k, &got) || (got < k && o + got < size)) { e = -3; break; }
            const int64_t tw = esp_timer_get_time();
            _SdLoad.ms_card += (uint32_t)(tw - tr);
            if (got < k) memset(buf + got, 0xFF, k - got);
            crc = cf_crc32(crc, buf, got);
            for (uint32_t q = 0; q < k && !e; q += 1024u) {
                const uint32_t kib = (o + q) >> 10;
                const int v = _SdVerify != 0 && _SdVerify != 3 && (all || kib < 16u || (kib & 63u) == 0);
                const uint32_t m0 = misses;
                e = sd_ram_write(CF_RC_ROM_BASE + o + q, buf + q, chk, v, &checked, &misses);
                if (misses != m0) all = 1;                  /* a miss: every KiB after it is read back */
            }
            _SdLoad.ms_ram += (uint32_t)(esp_timer_get_time() - tw);
            _P.pct = (int)((uint64_t)(o + k) * 100u / len);
        }
        if (!e && _SdVerify == 3) {                          /* bench: the whole game read back after a blind write */
            for (uint32_t o = 0; o < len && !e; o += chunk) {
                const uint32_t k = len - o < chunk ? len - o : chunk;
                got = 0;
                if (Sd_Read(f, o, buf, k, &got)) { e = -3; break; }
                if (got < k) memset(buf + got, 0xFF, k - got);
                for (uint32_t q = 0; q < k && !e; q += 1024u) {
                    e = CfRc_PsramRead(CF_RC_ROM_BASE + o + q, chk, 1024);
                    checked++;
                    if (!e && memcmp(chk, buf + q, 1024)) { misses++; e = sd_ram_write(CF_RC_ROM_BASE + o + q, buf + q, chk, 1, &checked, &misses); }
                }
            }
        }
        _SdLoad.ms = (uint32_t)((esp_timer_get_time() - t0) / 1000); _SdLoad.kib = len >> 10; _SdLoad.checked = checked; _SdLoad.misses = misses;
        printf("[%u ms] rc: play SD: ROM staging end (result %d)\n", (unsigned)esp_log_timestamp(), e);
        if (e) { Sd_Close(f); result(kNwPv_Result, "LOADING  FAILED", "TRY  AGAIN"); goto out; }
        printf("rc: sd: %s: %lu bytes from the card into PSRAM in %lu ms (%lu KiB/s; %lu KiB read back, %lu wrong, verify %u)\n", _SdPath, (unsigned long)size,
               (unsigned long)_SdLoad.ms, (unsigned long)(_SdLoad.ms ? (uint64_t)_SdLoad.kib * 1000u / _SdLoad.ms : 0), (unsigned long)checked, (unsigned long)misses, _SdVerify);
        printf("rc: sd: of which card reads %lu ms, PSRAM writes (and read-backs) %lu ms\n", (unsigned long)(_SdLoad.ms_card / 1000u), (unsigned long)(_SdLoad.ms_ram / 1000u));
        _P.crc = crc; _RamGame.crc = crc;
    }
    Sd_Close(f);
    printf("[%u ms] rc: play SD: save load start\n", (unsigned)esp_log_timestamp());
    const int save_load = sd_save_load(buf, chk, f);
    printf("[%u ms] rc: play SD: save load end (result %d)\n", (unsigned)esp_log_timestamp(), save_load);
    if (save_load) { result(kNwPv_Result, "LOADING  FAILED", "TRY  AGAIN"); goto out; }
    _P.ent = -1;
    if (rom_pad(size, _RamGame.title)) { result(kNwPv_Result, "LOADING  FAILED", "TRY  AGAIN"); goto out; }
    printf("[%u ms] rc: play SD: RAM cart on start\n", (unsigned)esp_log_timestamp());
    const uint8_t flags = CF_RC_A_ON | CF_RC_A_HOLD | CF_RC_A_INVAL | CF_RC_A_CLEAN | (_SdKeepOn ? CF_RC_A_MRESET : 0);
    const int cart_on = cfg_now(flags);
    printf("[%u ms] rc: play SD: new config (flags 0x%02x, result %d)\n", (unsigned)esp_log_timestamp(), flags, cart_on);
    printf("[%u ms] rc: play SD: RAM cart on end (result %d)\n", (unsigned)esp_log_timestamp(), cart_on);
    if (cart_on) { result(kNwPv_Result, "IT  DID  NOT  START", "TRY  AGAIN"); goto out; }
    vTaskDelay(pdMS_TO_TICKS(50));
    const int release = cfg_now(CF_RC_A_ON);
    printf("[%u ms] rc: play SD: release (flags 0x01, result %d)\n", (unsigned)esp_log_timestamp(), release);
    if (release) { result(kNwPv_Result, "IT  DID  NOT  START", "TRY  AGAIN"); goto out; }
    _P.pend = 0; _P.saves = 0;
    _P.st = kNwPv_Playing;
    OSD_SetVisiblityState(false);              /* (m110: the core restarted: the menu is closed) */
    printf("[%u ms] rc: play SD: game started (result 0)\n", (unsigned)esp_log_timestamp());
    printf("rc: playing %s from the card (mapper %u, %u ROM banks, %u RAM banks%s) with no cartridge\n", _RamGame.title, _P.mbc, _P.rom_banks, _P.ram_banks, _P.rtc ? ", clock" : "");
    _SdKeepOn = 0;
    rc = 0;
    (void)Sd_Ask(kSdOp_Refresh, NULL);        /* (a game just banked over Wi-Fi: the library and the next receive summary count it) */
out:
    /* fr1 (m110s for the card): a game that did not start lets the core go, back to the cartridge (else the screen stayed
       white, the core held, until the unit was switched off) */
    if (rc && started) core_back("card game failed after core reset");
    printf("[%u ms] rc: play SD: end (result %d)\n", (unsigned)esp_log_timestamp(), rc);
    free(f); heap_caps_free(buf); heap_caps_free(chk);
    return rc;
}

/* ================================================================ lane gamecomp1: the game library on the FPGA's boot flash
 * cf_lib.c keeps the games and their saves in the FPGA flash's GAME AREA (FPGA 18.31 ops: cf_rc_dev.c CfRc_GameArea*,
 * one caller at a time: every op here runs on this task). The library is opened (heap, ~5 KB) when SYSTEM > PLAY looks
 * at it, a game plays from it, or a send keeps a game; closed when none of those is on.
 * A game plays from it as a card game does: its bytes (SGZ1 unpacked, or the ROM as it is) into PSRAM, read back as
 * the card's path; its save from its newest journal record (or this unit's store when that holds a newer copy the
 * library has not taken yet); saves go back into the library 1.5 s after the game's last write (at most every 10 s,
 * and at the stop). A save the library cannot take goes to this unit's store (the 1.1.0 one) marked pending, and to
 * the library at the next chance. */
int CfRc_GameAreaErase(uint32_t a, uint32_t n);
int CfRc_GameAreaProg(uint32_t a, const uint8_t *b, uint32_t n);
int CfRc_GameAreaRead(uint32_t a, uint8_t *dst, uint32_t n);
int CfRc_GameAreaCrc(uint32_t a, uint32_t n, uint32_t *crc, int *all_ff);
static int ga_erase(void *u, uint32_t a, uint32_t n) { (void)u; return CfRc_GameAreaErase(a, n); }
static int ga_prog(void *u, uint32_t a, const uint8_t *b, uint32_t n) { (void)u; return CfRc_GameAreaProg(a, b, n); }
static int ga_read(void *u, uint32_t a, uint8_t *b, uint32_t n) { (void)u; return CfRc_GameAreaRead(a, b, n); }
static int ga_crc(void *u, uint32_t a, uint32_t n, uint32_t *c) { (void)u; int ff = 0; return CfRc_GameAreaCrc(a, n, c, &ff); }
static cf_lib_flash_t _LF = { NULL, 0, 0, ga_erase, ga_prog, ga_read, ga_crc };
static cf_lib_t *_L;
static uint32_t _LibUid;                 /* the library game playing (_Src 3) */
static volatile uint32_t _LibGen;        /* +1 at every change the page should see */
static volatile int _LibHold;            /* SYSTEM > PLAY is open: keep the library open */
static volatile int _LibArea = -1;       /* -1 not looked yet, 0 none, 1 there */
static struct {
    volatile int op;                     /* 0 none; RC_LIB_* */
    uint32_t uid, src, len, crc;
    cf_lib_ent_t meta;
    int play;                            /* add: then play it (from the library; from the staging when it does not fit) */
    volatile int busy, res;              /* res: 0 ok, < 0 cf_lib error */
    char why[24];
} _LibOp;
enum { RC_LIB_OPEN = 1, RC_LIB_DEL, RC_LIB_ADD, RC_LIB_MIGRATE, RC_LIB_FMT, RC_LIB_END,
       RC_LIB_TO_SD, RC_LIB_TO_STAGE, RC_LIB_SAVE_CARD, RC_LIB_SAVE_STAGE,   /* lane ui110dev: SYSTEM > GAMES's copies */
       RC_LIB_SHOPEN, RC_LIB_SVPUT,                                          /* lane share1 */
       RC_LIB_SVPROBE, RC_LIB_NETCLOSE };                                    /* lane savecloud1 */
static cf_lib_ent_t _ShEnt;              /* lane share1: the entry a share reads (RC_LIB_SHOPEN) */
/* lane savecloud1: SYSTEM > GAMES asks whether a kept game has a stored save (st: 0 not asked, 1 asked, 2 none, 3 yes) */
static struct { uint32_t uid; volatile int st; } _SvProbe;
static cf_lib_save_t _ShSv;
static cf_lib_save_t _LibSv;             /* the playing game's save as loaded / last written */
static int _LibLoad = 1;                 /* 1 (default): a container read from the FPGA flash by FREAD (its windows crc32-checked by the FPGA); 0 (lib fast fcopy): FCOPY into PSRAM scratch, then RREADs, which with the core held now and then fill a window a word late (unit 1 2026-10-22: Crystal from the library 2 of 10 that way, 10 of 10 by FREAD) */
/* the heap an unpack needs (~12 KB, a block of 8 KiB) is not there while the network link runs (unit 1, link up: 12.7 KB
   free, largest 6.4 KB): a library load / add holds the link as receive mode does (the room left, the idle connection
   closed) and waits for the heap, then lets it go */
static int _NetHeld;
static void net_hold_for_unpack(void)
{
    if (!CfRc_SdPlaying() && Sd_Busy() == kSdOp_None) Sd_Lend();   /* (the card's ~1.4 KB too) */
    if (_NetHeld || NW_RecvHeld()) return;           /* (receive mode holds it already: it lets it go itself) */
    NW_RecvHold(true);
    _NetHeld = 1;
    for (int t = 0; t < 30; t++) {
        if (heap_caps_get_free_size(MALLOC_CAP_8BIT) >= 10240u && heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) >= 4096u) break;   /* (a 4 KiB-block unpack: ~7.5 KB) */
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
static void net_release(void) { if (_NetHeld) { NW_RecvHold(false); _NetHeld = 0; } }
static int _LibPreZ;                     /* an add's container checked whole before the library opened (heap): its result, header */
static cf_sgz_hdr_t _LibPreH;
static void lib_snap(void);
static volatile int _SnapWant;                    /* m110q: the list's copy could not be made (lib_snap) */
static uint32_t _SnapTried;
/* what a download needs to plan its sectors with the library closed (taken at each look at the library) */
static uint32_t *_LibMap, _LibMapN, _LibMapSeq, _LibFreeB;   /* (_LibMap: heap, only on a unit with a library) */
static int _LibMapGames, _LibMapOk;
static cf_lib_tx_t *_LibEndTx;           /* RC_LIB_END: the stream to finish */

/* m110t: the library's ~5.5 KB in one piece. The final install gate (unit 1, right after a GET GAMES): 25,672 bytes free but
   the largest block 4,864, so SYSTEM > GAMES's ON CHROMATIC read nothing. What a job leaves that is let go anyway goes
   first (the read window's KiB, the online TLS client, the HTTP client: they come back by themselves when next needed),
   the heap a moment to come together; then the link paused for the allocation (gamecomp1, as a cart job does), then back */
static volatile int _LibOpenFail;         /* the last open found no memory: CfRc_LibCount says -3 until an open works */
static cf_lib_t *lib_alloc(void)
{
    cf_lib_t *L = malloc(sizeof *L);
    if (L) return L;
    const uint32_t f0 = (uint32_t)esp_get_free_heap_size(), b0 = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    CfRc_Trim();
    (void)CwlOnline_Quiet(3000);
    (void)NW_DropClient(3000);
    for (int t = 0; t < 10 && !(L = malloc(sizeof *L)); t++) vTaskDelay(pdMS_TO_TICKS(150));
    const char *how = "the network's clients let go";
    if (!L && !CWL_CartHeld()) {
        /* gamecomp1: the running link splits the heap (unit 1 2026-10-22: 11.6 KB free, the largest block 5,376 < 5,528) */
        (void)CWL_CartHold(true, 8000);
        for (int t = 0; t < 10 && !(L = malloc(sizeof *L)); t++) vTaskDelay(pdMS_TO_TICKS(150));
        (void)CWL_CartHold(false, 0);
        how = "the link paused";
    }
    printf("rc: lib: the heap in pieces (free %lu, largest %lu): %s, the library %s (free %lu, largest %lu)\n", (unsigned long)f0, (unsigned long)b0, how,
           L ? "opens" : "COULD NOT OPEN", (unsigned long)esp_get_free_heap_size(), (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    return L;
}
static int lib_open(void)
{
    if (_L) return 0;
    if (_LibArea == 0) return CF_LIB_E_NOAREA;
    uint32_t b = 0, n = 0;
    {
        /* the area's place is read from the FPGA flash's header area (FREAD): only once the FPGA answers (at boot it may
           not yet); a flash with no rf layout (1.0.x / 1.1.0-ab, or the FPGA too old for FREAD) has none */
        extern int CfDev_FpgaVersion(unsigned *maj, unsigned *min);
        unsigned maj = 0, min = 0;
        int known = 0;
        for (int t = 0; t < 20 && !(known = CfDev_FpgaVersion(&maj, &min)); t++) vTaskDelay(pdMS_TO_TICKS(500));
        if (!known) return CF_LIB_E_NOAREA;                            /* (not known yet: looked at again next time) */
        if (maj != 18 || min < CF_FOTA_FPGA_MINOR) { _LibArea = 0; printf("rc: lib: FPGA %u.%u has no game area ops\n", maj, min); return CF_LIB_E_NOAREA; }
        int ga = -1;
        for (int t = 0; t < 3 && (ga = CfRc_GameArea(&b, &n)) != 0; t++) { CfRc_GameAreaRescan(); (void)CfRc_Reset(); vTaskDelay(pdMS_TO_TICKS(300)); }
        if (ga) { printf("rc: lib: the boot flash's header area did not read (tried again later)\n"); return CF_LIB_E_NOAREA; }   /* (an rf-less flash: also here) */
    }
    _LF.base = b; _LF.size = n;
    _L = lib_alloc();
    if (!_L) {
        printf("rc: lib: no memory to open the library (%u bytes; free heap %lu, largest %lu)\n", (unsigned)sizeof *_L,
               (unsigned long)esp_get_free_heap_size(), (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        _LibOpenFail = 1;                                  /* m110t: SYSTEM > GAMES says so (and asks again), never a section gone */
        _LibGen++;
        return CF_LIB_E_FLASH;
    }
    _LibOpenFail = 0;
    const int64_t t0 = esp_timer_get_time();
    const int e = cf_lib_open(_L, &_LF);
    if (e) { free(_L); _L = NULL; printf("rc: lib: the library did not open (%d)\n", e); return e; }
    _LibArea = 1;
    lib_snap();
    printf("rc: lib: open, %u games, %lu of %lu bytes free (%lld ms)\n", _L->ix.n, (unsigned long)cf_lib_free_bytes(_L), (unsigned long)cf_lib_total_bytes(_L),
           (long long)((esp_timer_get_time() - t0) / 1000));
    return 0;
}
static void lib_close_if_idle(void)
{
    if (_L && !_LibHold && !(_Src == 3 && CfRc_Playing()) && !_LibOp.busy) {
        free(_L); _L = NULL;
        _SnapWant = 0;                                   /* (m110q: opened again later, with its copy) */
        portENTER_CRITICAL(&_LibMux);
        void *sn = _Snap; _Snap = NULL;
        portEXIT_CRITICAL(&_LibMux);
        free(sn);
    }
}
int CfRc_LibThere(void) { return _LibArea; }
__attribute__((weak)) const char *OSD_QspiHolder(void) { return "?"; }
/* gamecomp1: a watch over loads: when a load's mark has not moved for 15 s it prints where it is (a hang seen 1 in ~10 loads,
   the console blocked too) */
static void rc_watch_task(void *arg)
{
    (void)arg;
    uint32_t said = 0, load_since = 0, slow_said = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000), age = now - g_rc_mark_ms;
        extern volatile uint32_t g_sgz_mark;
        if (_P.st != kNwPv_Loading) load_since = 0; else if (!load_since) load_since = now;
        if (load_since && now - load_since > 60000u && now - slow_said > 15000u) {
            /* (a load that takes a minute or more: where it is, its error counts) */
            printf("rc: watch: a load for %lu s: sgz block %lu, mark line %lu (%lu ms ago), blocks read again %lu, window reads %lu differed %lu, checked fills %lu again %lu, st err %u done %02x\n",
                   (unsigned long)((now - load_since) / 1000u), (unsigned long)g_sgz_mark, (unsigned long)g_rc_mark, (unsigned long)age,
                   (unsigned long)cf_sgz_prof.retries, (unsigned long)_WinReads, (unsigned long)_WinDiff, (unsigned long)_RrFills, (unsigned long)_RrRedo, _St[3], _St[2]);
            slow_said = now;
        }
        if (_P.st == kNwPv_Loading && g_rc_mark && age > 15000u && g_rc_mark_ms != said) {
            printf("rc: watch: a load stuck at line %lu for %lu s (sgz block %lu); QSPI lock held by %s; free heap %lu\n", (unsigned long)g_rc_mark,
                   (unsigned long)(age / 1000u), (unsigned long)g_sgz_mark, OSD_QspiHolder(), (unsigned long)esp_get_free_heap_size());
            said = g_rc_mark_ms;
        }
    }
}
void CfRc_WatchStart(void) { static int on; if (!on) { on = 1; (void)xTaskCreate(rc_watch_task, "rc_watch", 2048, NULL, 1, NULL); } }
/* gamecomp1 bench (cart where: the cart console answers while a load hangs): the last load mark and its age */
void CfRc_Where(void)
{
    CfRc_WatchStart();
    extern volatile uint32_t g_sgz_mark;
    printf("rc: where: mark line %lu, %lu ms ago; sgz block %lu; free heap %lu; task %s\n", (unsigned long)g_rc_mark,
           (unsigned long)((uint32_t)(esp_timer_get_time() / 1000) - g_rc_mark_ms), (unsigned long)g_sgz_mark, (unsigned long)esp_get_free_heap_size(),
           _P.task ? "alive" : "none");
}

/* the library's bytes of the game (uid in u) */
static int lib_rd(void *u, uint32_t off, uint8_t *b, uint32_t n) { return cf_lib_read(_L, (uint32_t)(uintptr_t)u, off, b, n) ? -1 : 0; }
/* PSRAM scratch above the RAM cart's 4 MiB ROM area: a library game's container while it unpacks (FCOPY's target) */
#define RC_SCRATCH      CF_RC_SCRATCH_BASE                 /* 0x500000 (fpgacomp1) */
#define RC_SCRATCH_MAX  CF_RC_SCRATCH_SIZE                 /* 3 MiB */
static int lib_rd_scratch(void *u, uint32_t off, uint8_t *b, uint32_t n) { (void)u; return psram_read_any(RC_SCRATCH + off, b, n) ? -1 : 0; }
static uint32_t _ScratchEnd;
static int lib_rd_scratch_fast(void *u, uint32_t off, uint8_t *b, uint32_t n) { (void)u; return win_timed_lim(CF_RC_O_RREAD, RC_SCRATCH + off, b, n, _FastUsRread, _ScratchEnd) ? -1 : 0; }
/* the library's flash read, timed (no status wait): swapped in for one unpack */
static int ga_read_fast(void *u, uint32_t a, uint8_t *b, uint32_t n) { (void)u; return win_timed_lim(CF_RC_O_FREAD, a, b, n, _FastUsFread, _LF.base + _LF.size); }
static int ga_read(void *u, uint32_t a, uint8_t *b, uint32_t n);
/* a game's stored bytes by its extents (a copy of its entry: the library itself is closed for the unpack's heap) */
static cf_lib_ent_t _LoadEnt;
static int ent_rd(uint32_t off, uint8_t *b, uint32_t n, int fast)
{
    while (n) {
        uint32_t base = 0, a = 0, room = 0;
        int k = 0;
        for (; k < _LoadEnt.next; k++) {
            const uint32_t len = (uint32_t)_LoadEnt.ext[k].count * CF_LIB_SECT;
            if (off < base + len) { a = _LF.base + (uint32_t)_LoadEnt.ext[k].start * CF_LIB_SECT + (off - base); room = base + len - off; break; }
            base += len;
        }
        if (k == _LoadEnt.next) return -1;
        const uint32_t q = n < room ? n : room;
        if (fast ? win_timed_lim(CF_RC_O_FREAD, a, b, q, _FastUsFread, a + room) : CfRc_GameAreaRead(a, b, q)) return -1;
        b += q; off += q; n -= q;
    }
    return 0;
}
static int lib_rd_fast(void *u, uint32_t off, uint8_t *b, uint32_t n) { (void)u; return ent_rd(off, b, n, 1); }
static int lib_rd_ent(void *u, uint32_t off, uint8_t *b, uint32_t n) { (void)u; return ent_rd(off, b, n, 0); }
/* zlib's crc32_combine: the crc32 of A then B from crc(A), crc(B) and len(B) */
static uint32_t gf2_times(const uint32_t *mat, uint32_t vec) { uint32_t sum = 0; while (vec) { if (vec & 1u) sum ^= *mat; vec >>= 1; mat++; } return sum; }
static void gf2_square(uint32_t *sq, const uint32_t *mat) { for (int n = 0; n < 32; n++) sq[n] = gf2_times(mat, mat[n]); }
static uint32_t crc_combine(uint32_t c1, uint32_t c2, uint32_t len2)
{
    uint32_t even[32], odd[32], row = 1;
    if (!len2) return c1;
    odd[0] = 0xEDB88320u;
    for (int n = 1; n < 32; n++) { odd[n] = row; row <<= 1; }
    gf2_square(even, odd);
    gf2_square(odd, even);
    do {
        gf2_square(even, odd);
        if (len2 & 1u) c1 = gf2_times(even, c1);
        len2 >>= 1;
        if (!len2) break;
        gf2_square(odd, even);
        if (len2 & 1u) c1 = gf2_times(odd, c1);
        len2 >>= 1;
    } while (len2);
    return c1 ^ c2;
}
/* the game's stored bytes into PSRAM at dst by FCOPY, extent by extent (the RAM cart off): 0 = all there, their crc32
   (combined over the extents) = the index's stored_crc. -1: no FCOPY here (FREAD then), -2 a copy that did not check */
static int lib_copy(const cf_lib_ent_t *e, uint32_t dst)
{
    if (e->stored_size > (dst == RC_SCRATCH ? RC_SCRATCH_MAX : CF_RC_ROM_MAX)) return -1;
    uint32_t off = 0, all = 0;
    for (int k = 0; k < e->next && off < e->stored_size; k++) {
        uint32_t len = (uint32_t)e->ext[k].count * CF_LIB_SECT, c = 0;
        if (len > e->stored_size - off) len = e->stored_size - off;
        const uint32_t a = _LF.base + (uint32_t)e->ext[k].start * CF_LIB_SECT, n2 = (len + 1u) & ~1u;
        const int ce = CfRc_GameAreaCopy(a, dst + off, n2, &c);
        if (ce) { printf("rc: lib: FCOPY %06lx+%lu -> %06lx: %d\n", (unsigned long)a, (unsigned long)n2, (unsigned long)(dst + off), ce); return -1; }
        if (n2 != len) {                                   /* (an odd length: the copy took one byte more; the crc of the exact bytes from the flash) */
            int ff = 0;
            if (CfRc_GameAreaCrc(a, len, &c, &ff)) return -2;
        }
        all = crc_combine(all, c, len);
        off += len;
    }
    if (all != e->stored_crc) { printf("rc: lib: FCOPY: crc32 %08lx, the index says %08lx\n", (unsigned long)all, (unsigned long)e->stored_crc); return -2; }
    return 0;
}
static int lib_to_scratch(const cf_lib_ent_t *e) { return lib_copy(e, RC_SCRATCH); }
static int play_lib_go(const cf_lib_ent_t *ep, uint8_t *hdr);
static int play_lib_inner(void);
/* gamecomp1: a load needs no network: the link session ends and Wi-Fi stops for it (as a USB cart job does: CWL_CartHold),
   its buffers back for the unpack (an 8 KiB-block game needs ~11.5 KB of >= 1 KiB pieces), then the session the mode wants
   starts again by itself. Unit 1 2026-10-22: Emerald Color (8 KiB blocks) could not get them; a staged Yellow SGZ1 played
   right after its report had 10.4 KB free, the largest block 2.4 KB */
static bool load_quiet_begin(const char *what)
{
    net_hold_for_unpack();
    const bool held = CWL_CartHeld();
    const uint32_t f0 = (uint32_t)esp_get_free_heap_size(), b0 = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (!held) { (void)CWL_CartHold(true, 8000); (void)CwlOnline_Quiet(5000); (void)NW_DropClient(5000); }
    printf("rc: %s: the link off for the load: free heap %lu (largest %lu) -> %lu (largest %lu)\n", what, (unsigned long)f0, (unsigned long)b0,
           (unsigned long)esp_get_free_heap_size(), (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    return held;
}
static void load_quiet_end(bool held)
{
    if (!held) (void)CWL_CartHold(false, 0);
    net_release();
}
static int play_lib(void)
{
    const bool held = load_quiet_begin("lib");
    const int r = play_lib_inner();
    load_quiet_end(held);
    return r;
}
static int play_lib_inner(void)
{
    unsigned maj = 0, min = 0;
    extern int CfDev_FpgaVersion(unsigned *maj, unsigned *min);
    if (!CfDev_FpgaVersion(&maj, &min) || maj != 18 || min < CF_RC_FPGA_MINOR) { result(kNwPv_NoFpga, "", ""); return -1; }
    if (lib_open()) { result(kNwPv_Result, "LOADING  FAILED", "TRY  AGAIN"); return -1; }
    printf("rc: lib: load step 1 (open)\n");
    const int i = cf_lib_find(_L, _LibUid);
    if (i < 0) { result(kNwPv_Result, "NO  SUCH  GAME", ""); return -1; }
    const cf_lib_ent_t e = _L->ix.e[i];
    uint8_t *const hdr = malloc(0x150);              /* (heap: static RAM is the TLS gate's) */
    if (!hdr) { result(kNwPv_Result, "NOT  ENOUGH", "MEMORY"); return -1; }
    const int r = play_lib_go(&e, hdr);
    free(hdr);
    return r;
}
static int play_lib_go(const cf_lib_ent_t *ep, uint8_t *hdr)
{
    const cf_lib_ent_t e = *ep;
    const char *why = "";
    cf_sgz_hdr_t zh;
    memset(&_RamGame, 0, sizeof _RamGame);
    snprintf(_RamGame.title, sizeof _RamGame.title, "%.16s", e.title);
    _RamGame.size = e.rom_size; _RamGame.crc = e.rom_crc32;
    if (cf_lib_read(_L, e.uid, 0, hdr, (e.flags & CF_LIB_F_SGZ) ? CF_SGZ_HDR : 0x150u)) { result(kNwPv_Result, "CANNOT  READ", "THE  GAME"); return -1; }
    if (e.flags & CF_LIB_F_SGZ) {
        if (cf_sgz_parse(hdr, e.stored_size, &zh)) { result(kNwPv_Result, "THE  GAME  IS", "DAMAGED"); return -1; }
        memset(hdr, 0, 0x150);
        memcpy(hdr + 0x134, zh.cart, sizeof zh.cart);
    }
    if (header_parse(hdr, e.rom_size, &why)) { result(kNwPv_Result, "CANNOT  PLAY  IT", why); return -1; }
    printf("rc: lib: load step 2 (header)\n");
    _P.st = kNwPv_Loading; _P.pct = 0;
    /* FCOPY first, with the RAM cart OFF (the FPGA's copy writes PSRAM only then: under HOLD its crc32 came back right but the
       PSRAM kept its old bytes, unit 1 2026-10-22): an SGZ1 container into PSRAM scratch, a raw game straight into the ROM
       area; then the core held for the unpack */
    if (CfRc_Reset() != 0 || CfRc_Cfg(0, 0, 0, 0, 0)) { result(kNwPv_Result, "IT  DID  NOT  START", "TRY  AGAIN"); return -1; }
    const int64_t tc = esp_timer_get_time();
    const int sc = (e.flags & CF_LIB_F_SGZ) ? (_LibLoad == 1 ? -1 : lib_to_scratch(&e)) : lib_copy(&e, CF_RC_ROM_BASE);
    const int64_t tz = esp_timer_get_time();
    _LoadEnt = e;
    if (sc == 0 || (e.flags & CF_LIB_F_SGZ)) { free(_L); _L = NULL; }   /* (the library's ~5 KB back for the unpack: read by the entry's extents; opened again for the saves) */
    printf("rc: lib: load step 3 (copied: %d)\n", sc);
    if (CfRc_Reset() != 0 || cfg_now(CF_RC_A_HOLD)) { result(kNwPv_Result, "IT  DID  NOT  START", "TRY  AGAIN"); return -1; }
    printf("rc: lib: load step 4 (the core held)\n");
    const int64_t t0 = esp_timer_get_time();
    uint32_t crc = 0;
    if (e.flags & CF_LIB_F_SGZ) {
        int ze = -99;
        const int fast = _FastOn;
        if (fast) {
            /* the blocks' bytes by timed reads (a block that does not check is read again the checked way); the header,
               the table and those blocks by the checked reads */
            _ScratchEnd = RC_SCRATCH + ((e.stored_size + 1u) & ~1u);
            if (_TagOn) (void)tag_probe();                 /* (no tagged op at all with tags off: on r1831e a later untagged window came back
                                                              in the tagged layout, 2 bytes on, unit 1 10-22) */
            _TagWaits = _TagPolls = _TagLate = 0;
            memset(&_Ahead, 0, sizeof _Ahead);
            ze = z_to_psram2(sc == 0 ? lib_rd_scratch_fast : lib_rd_fast, sc == 0 ? lib_rd_scratch : lib_rd_ent, (void *)(uintptr_t)e.uid, e.stored_size, &zh);
            ahead_done();
            if (ze && sc == 0 && cf_sgz_prof.fail_len) {
                /* gamecomp1 diagnosis (r1831e): the block that failed both reads, its bytes in PSRAM scratch (checked RREADs) against the
                   library's flash (checked FREADs, crc32 by the FPGA): which side is wrong */
                const uint32_t fs = cf_sgz_prof.fail_src, fl = cf_sgz_prof.fail_len;
                uint8_t *pa = malloc(256), *pb = malloc(256);
                long first = -1; uint32_t ndiff = 0; int re = 0;
                uint8_t sa[8] = { 0 }, sb[8] = { 0 };
                for (uint32_t o = 0; pa && pb && o < fl && !re; o += 256u) {
                    const uint32_t k = fl - o < 256u ? fl - o : 256u;
                    re = lib_rd_scratch(NULL, fs + o, pa, k) ? 1 : lib_rd_ent((void *)(uintptr_t)e.uid, fs + o, pb, k) ? 2 : 0;
                    for (uint32_t i = 0; !re && i < k; i++) if (pa[i] != pb[i]) { if (first < 0) { first = (long)(o + i); for (int j = 0; j < 8 && i + j < k; j++) { sa[j] = pa[i + j]; sb[j] = pb[i + j]; } } ndiff++; }
                }
                free(pa); free(pb);
                printf("rc: lib: block %lu (container +%lu, %lu bytes) failed twice; scratch vs flash: %s, %lu bytes differ, first at +%ld: scratch %02x %02x %02x %02x %02x %02x flash %02x %02x %02x %02x %02x %02x\n",
                       (unsigned long)cf_sgz_prof.fail_block, (unsigned long)fs, (unsigned long)fl, re ? (re == 1 ? "scratch read failed" : "flash read failed") : ndiff ? "DIFFER" : "same",
                       (unsigned long)ndiff, first, sa[0], sa[1], sa[2], sa[3], sa[4], sa[5], sb[0], sb[1], sb[2], sb[3], sb[4], sb[5]);
            }
            printf("rc: lib: read ahead: %lu hits, %lu misses; tag %s: %lu waits, %lu polls, %lu late\n", (unsigned long)_Ahead.hits, (unsigned long)_Ahead.misses,
                   _Tag == 1 && _TagOn ? "on" : "off", (unsigned long)_TagWaits, (unsigned long)_TagPolls, (unsigned long)_TagLate);
        } else ze = z_to_psram(sc == 0 ? lib_rd_scratch : lib_rd_ent, (void *)(uintptr_t)e.uid, e.stored_size, &zh);
        printf("rc: lib: the container came by %s%s (copy %lld ms, unpack %lld ms)\n", sc == 0 ? "FCOPY into PSRAM scratch" : "FREAD",
               fast ? ", read timed" : ", read checked", (long long)((tz - tc) / 1000), (long long)((esp_timer_get_time() - tz) / 1000));
        if (ze) { cfg_now(0); result(kNwPv_Result, ze == CF_SGZ_E_READ || ze == CF_SGZ_E_WRITE || ze == CF_SGZ_E_MEM ? "LOADING  FAILED" : "THE  GAME  IS", ze == CF_SGZ_E_READ || ze == CF_SGZ_E_WRITE || ze == CF_SGZ_E_MEM ? "TRY  AGAIN" : "DAMAGED"); return -1; }
        crc = zh.rom_crc32;
    } else if (sc == 0) {
        crc = e.rom_crc32;                                /* (raw: stored = the game; its crc32 checked by the copy) */
        printf("rc: lib: %s copied into the ROM area by FCOPY\n", e.title);
    } else {
        uint8_t *b = heap_caps_malloc(1024, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL), *chk = heap_caps_malloc(1024, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        uint32_t checked = 0, misses = 0;
        int all = 0, er = !b || !chk;
        for (uint32_t o = 0; !er && o < e.rom_size; o += 1024u) {
            const uint32_t k = e.rom_size - o < 1024u ? e.rom_size - o : 1024u;
            if (cf_lib_read(_L, e.uid, o, b, k)) { er = 1; break; }
            if (k < 1024u) memset(b + k, 0xFF, 1024u - k);
            crc = cf_crc32(crc, b, k);
            const uint32_t m0 = misses, kib = o >> 10;
            er = sd_ram_write(CF_RC_ROM_BASE + o, b, chk, all || kib < 16u || (kib & 63u) == 0, &checked, &misses) != 0;
            if (misses != m0) all = 1;
            _P.pct = (int)((uint64_t)(o + k) * 100u / e.rom_size);
        }
        heap_caps_free(b); heap_caps_free(chk);
        if (er || crc != e.rom_crc32) { cfg_now(0); result(kNwPv_Result, er ? "LOADING  FAILED" : "THE  GAME  IS", er ? "TRY  AGAIN" : "DAMAGED"); return -1; }
    }
    printf("rc: lib: %s: %lu bytes (%s %lu) from the FPGA flash into PSRAM in %lld ms\n", e.title, (unsigned long)e.rom_size, (e.flags & CF_LIB_F_SGZ) ? "SGZ1" : "raw",
           (unsigned long)e.stored_size, (long long)((esp_timer_get_time() - t0) / 1000));
    _P.crc = crc;
    /* its save: the library's newest, or this unit's store's newer copy (pending), else blank */
    memset(&_Sv, 0, sizeof _Sv);
    memset(&_LibSv, 0, sizeof _LibSv);
    {
        uint8_t *b = heap_caps_malloc(1024, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL), *chk = heap_caps_malloc(1024, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        int src = 0, er = !b || !chk;
        uint8_t rt[5] = { 0 };
        (void)index_load();
        const int ent = _P.battery && _P.sav_size ? ent_for(crc, _P.sav_size, 0) : -1;
        if (!er && ent >= 0 && _Ix.e[ent].used == RC_ENT_PENDING && _Ix.e[ent].dcrc) src = 2;
        if (!er && !_L) (void)lib_open();
        if (!er && !src && _P.battery && _P.sav_size && _L && cf_lib_save_get(_L, e.uid, &_LibSv) == 0 && _LibSv.seq) src = 1;
        uint32_t scrc = 0, dummy = 0;
        for (uint32_t o = 0; !er && o < ((_P.sav_size + 1023u) & ~1023u); o += 1024u) {
            const uint32_t k = _P.sav_size - o < 1024u ? _P.sav_size - o : 1024u;
            memset(b, 0xFF, 1024);
            if (src == 1 && cf_lib_save_read(_L, e.uid, &_LibSv, o, b, k)) memset(b, 0xFF, 1024);
            if (src == 2 && esp_flash_read(NULL, b, store_base() + (uint32_t)_Ix.e[ent].sect * 4096u + o, 1024) != ESP_OK) memset(b, 0xFF, 1024);
            scrc = cf_crc32(scrc, b, k);
            er = sd_ram_write(CF_RC_RAM_BASE + o, b, chk, 1, &dummy, &dummy) != 0;
        }
        heap_caps_free(b); heap_caps_free(chk);
        if (er) { cfg_now(0); result(kNwPv_Result, "LOADING  FAILED", "TRY  AGAIN"); return -1; }
        uint64_t t_saved = 0;
        if (src == 1) { memcpy(rt, _LibSv.rtc, 5); t_saved = sdc_tsec_unpack(_LibSv.tsec, CwlTime_Anchor()); }
        if (src == 2) { memcpy(rt, _Ix.e[ent].rtc, 5); t_saved = ix_tm(ent); }
        memset(&_Clk, 0, sizeof _Clk);
        if (_P.rtc) clk_loaded(rt, t_saved);                /* 1.1.7: caught up by the time it spent in the library */
        if (_P.rtc) (void)CfRc_Op(CF_RC_O_RRTC, (uint32_t)rt[0] | (uint32_t)rt[1] << 8 | (uint32_t)rt[2] << 16, (uint32_t)rt[3] | (uint32_t)rt[4] << 8, 1, 200);
        _Sv.crc = src ? scrc : 0;
        memcpy(_Sv.rtc, rt, 5);
        _Sv.fail = src == 2;                          /* the store's copy goes into the library at the first chance */
        printf("rc: lib: save %lu bytes %s (crc %08lx)\n", (unsigned long)_P.sav_size, src == 1 ? "from the library" : src == 2 ? "from this unit's store (not in the library yet)" : "blank",
               (unsigned long)scrc);
    }
    _P.ent = -1;
    if (rom_pad(e.rom_size, e.title)) { cfg_now(0); result(kNwPv_Result, "LOADING  FAILED", "TRY  AGAIN"); return -1; }
    if (cfg_now(CF_RC_A_ON | CF_RC_A_HOLD | CF_RC_A_INVAL | CF_RC_A_CLEAN)) { cfg_now(0); result(kNwPv_Result, "IT  DID  NOT  START", "TRY  AGAIN"); return -1; }
    vTaskDelay(pdMS_TO_TICKS(50));
    if (cfg_now(CF_RC_A_ON)) { result(kNwPv_Result, "IT  DID  NOT  START", "TRY  AGAIN"); return -1; }
    _P.pend = 0; _P.saves = 0;
    if (_P.battery && _P.sav_size && _L) (void)cf_lib_save_prepare(_L, e.uid);   /* savefast1: the first save has no erase to wait for */
    _P.st = kNwPv_Playing;
    OSD_SetVisiblityState(false);              /* (m110: the core restarted: the menu is closed) */
    printf("rc: playing %s from the library (mapper %u, %u ROM banks, %u RAM banks%s) with no cartridge\n", e.title, _P.mbc, _P.rom_banks, _P.ram_banks, _P.rtc ? ", clock" : "");
    return 0;
}

/* the save RAM (PSRAM) as the library's put reads it */
static int lib_sv_get(void *u, uint32_t off, uint8_t *b, uint32_t n) { (void)u; return CfRc_PsramRead(CF_RC_RAM_BASE + off, b, (n + 1u) & ~1u) ? -1 : 0; }
static int save_copy_lib(void)
{
    if (!_P.battery || !_P.sav_size) return 0;
    const uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (_Sv.fail && !_SdStopping && (int32_t)(now - _Sv.retry_at) < 0) return 1;
    uint8_t *a = heap_caps_malloc(1024, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!a) return -1;
    uint32_t crc = 0;
    int e = 0;
    for (uint32_t o = 0; !e && o < _P.sav_size; o += 1024u) {
        const uint32_t k = _P.sav_size - o < 1024u ? _P.sav_size - o : 1024u;
        if (CfRc_PsramRead(CF_RC_RAM_BASE + o, a, 1024)) e = -4; else crc = cf_crc32(crc, a, k);
    }
    heap_caps_free(a);
    if (e) return e;
    uint8_t rt[5];
    memcpy(rt, _Sv.rtc, 5);
    if (_P.rtc && CfRc_Op(CF_RC_O_RRTC, 0, 0, 0, 200) == 0) {
        const uint32_t r = CfRc_Crc();
        rt[0] = (uint8_t)(r & 0x3F); rt[1] = (uint8_t)((r >> 8) & 0x3F); rt[2] = (uint8_t)((r >> 16) & 0x1F); rt[3] = (uint8_t)(r >> 24); rt[4] = _St[16];
    }
    const int changed = crc != _Sv.crc || (_P.rtc && _SdStopping && memcmp(rt, _Sv.rtc, 5));
    if (!changed && !_Sv.fail) { _P.saves++; _P.last_save = now; return 0; }
    const int64_t t0 = esp_timer_get_time();
    uint8_t kp[5];
    memcpy(kp, rt, 5);
    const uint64_t tk = clk_keep(kp);                               /* the real time, or the last known one plus the time played */
    e = (!_L && lib_open()) ? CF_LIB_E_NOAREA : 0;              /* (the clock in whole seconds: no rounding, no drift) */
    if (!e) { _GaRingOpen = CfRc_RingBegin() == 0; e = cf_lib_save_put_t(_L, _LibUid, _P.sav_size, kp, sdc_tsec_pack(tk), lib_sv_get, NULL); _GaRingOpen = 0; }
    if (e == 0) {
        printf("rc: lib: save reached flash %lu ms after the game's first write, %lld ms for the flash\n", (unsigned long)(xTaskGetTickCount() * portTICK_PERIOD_MS - _P.pend_first), (long long)((esp_timer_get_time() - t0) / 1000));
        (void)cf_lib_save_prepare(_L, _LibUid);          /* savefast1: the next save has no erase to wait for */
        /* the bytes the game may have written meanwhile: the next copy has them (the journal has a whole save either way) */
        printf("rc: lib: save in the library (%lu bytes, crc %08lx, %lld ms)\n", (unsigned long)_P.sav_size, (unsigned long)crc, (long long)((esp_timer_get_time() - t0) / 1000));
        if (_Sv.fail) {
            (void)index_load();
            const int ent = ent_for(_P.crc, _P.sav_size, 0);
            if (ent >= 0 && _Ix.e[ent].used == RC_ENT_PENDING) { _Ix.e[ent].used = 1; (void)index_save(); }
        }
        _Sv.crc = crc; memcpy(_Sv.rtc, rt, 5); _Sv.fail = 0;
        _P.saves++; _P.last_save = now;
        return 0;
    }
    _Sv.retry_at = now + RC_SD_RETRY_MS;
    printf("rc: lib: the library did not take the save (%d): kept in this unit's store until it does\n", e);
    _Sv.fail = 1;
    if (_P.sav_size <= 15u * 4096u) {
        (void)index_load();
        _P.ent = ent_for(_P.crc, _P.sav_size, 1);
        if (_P.ent >= 0 && save_copy_store() == 0) { memcpy(_Ix.e[_P.ent].rtc, kp, 5); _Ix.tm[_P.ent] = (uint32_t)tk; _Ix.e[_P.ent].used = RC_ENT_PENDING; (void)index_save(); }
        _P.ent = -1;
    }
    return -2;
}

/* ---- what SYSTEM > PLAY shows: a copy made by this task after each change, read under the lock */
/* m110q: 1 = the last copy could not be made (no memory): no copy is kept (unit 2, the overnight QA: a zeroed copy was left,
   SYSTEM > GAMES said ON CHROMATIC: NO GAMES YET and 0 KB free, `lib st` games 0 total 0, with 2 games kept); CfRc_LibCount
   says -3 (NOT READ: TRYING, the page asks again) and the task makes it again every 3 s */
static void lib_snap(void)
{
    if (!_L) return;
    cf_lib_view_ent_t *t = calloc(CF_LIB_MAX_GAMES, sizeof *t);
    if (t && !_Snap && !(_Snap = calloc(1, sizeof *_Snap))) { free(t); t = NULL; }
    if (!t) {
        portENTER_CRITICAL(&_LibMux);
        void *sn = _Snap; _Snap = NULL;
        portEXIT_CRITICAL(&_LibMux);
        free(sn);
        if (!_SnapWant) printf("rc: lib: no memory for the list's copy (free heap %lu, largest %lu): made again later\n",
                               (unsigned long)esp_get_free_heap_size(), (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        _SnapWant = 1; _SnapTried = (uint32_t)(esp_timer_get_time() / 1000);
        _LibGen++;
        return;
    }
    if (_SnapWant) printf("rc: lib: the list's copy made again\n");
    _SnapWant = 0;
    int n = 0;
    for (int i = 0; i < _L->ix.n && i < CF_LIB_MAX_GAMES; i++, n++) {
        const cf_lib_ent_t *x = &_L->ix.e[i];
        t[i].uid = x->uid; t[i].rom_size = x->rom_size; t[i].stored_size = x->stored_size; t[i].save_size = x->save_size; t[i].flags = x->flags;
        t[i].rom_crc32 = x->rom_crc32;
        snprintf(t[i].title, sizeof t[i].title, "%s", x->title);
    }
    const uint32_t fr = cf_lib_free_bytes(_L), to = cf_lib_total_bytes(_L);
    if (!_LibMap) _LibMap = malloc(CF_LIB_MAX_SECT / 8);
    if (_LibMap) { cf_lib_free_map(_L, _LibMap, &_LibMapN, &_LibMapSeq); _LibMapGames = _L->ix.n; _LibFreeB = fr; _LibMapOk = 1; }
    portENTER_CRITICAL(&_LibMux);
    _Snap->n = n; _Snap->free = fr; _Snap->total = to;
    memcpy(_Snap->e, t, CF_LIB_MAX_GAMES * sizeof *t);
    portEXIT_CRITICAL(&_LibMux);
    free(t);
    _LibGen++;
}
/* -1 no library on this unit, -2 not read yet (being opened), else the games */
int CfRc_LibCount(uint32_t *free_b, uint32_t *total_b, uint32_t *gen)
{
    if (gen) *gen = _LibGen;
    if (_LibArea == 0) return -1;
    int n = -2;
    portENTER_CRITICAL(&_LibMux);
    if (_Snap) { n = _Snap->n; if (free_b) *free_b = _Snap->free; if (total_b) *total_b = _Snap->total; }
    else if (_LibOpenFail || (_SnapWant && _L)) n = -3;  /* m110t: not read (no memory): the page says so and asks again (m110q: or its copy) */
    portEXIT_CRITICAL(&_LibMux);
    return n;
}
int CfRc_LibHasGame(uint32_t rom_crc32, uint32_t rom_size)
{
    int has = 0;
    portENTER_CRITICAL(&_LibMux);
    for (int i = 0; _Snap && i < _Snap->n && !has; i++) has = _Snap->e[i].rom_crc32 == rom_crc32 && _Snap->e[i].rom_size == rom_size;
    portEXIT_CRITICAL(&_LibMux);
    return has;
}
int CfRc_LibGet(int i, cf_lib_view_ent_t *o)
{
    int ok = 0;
    portENTER_CRITICAL(&_LibMux);
    if (_Snap && i >= 0 && i < _Snap->n) { *o = _Snap->e[i]; ok = 1; }
    portEXIT_CRITICAL(&_LibMux);
    return ok ? 0 : -1;
}
static void ask_task(int req);
static int lib_ask(int op)
{
    if (_LibOp.busy) return -1;
    _LibOp.busy = 1; _LibOp.op = op; _LibOp.res = 0;
    ask_task(_P.req ? _P.req : 4);                      /* (4: wake up, nothing else) */
    return 0;
}
void CfRc_LibHold(int on)
{
    _LibHold = on;
    if (on && !_L && _LibArea != 0 && lib_ask(RC_LIB_OPEN) == 0) _LibOpenFail = 0;   /* (m110t: READING again while it tries) */
    if (!on && _L) ask_task(_P.req ? _P.req : 4);       /* m110s: the task closes it once idle (it may not be running now) */
    if (on && _L && _SnapWant) ask_task(_P.req ? _P.req : 4);   /* m110q: its copy made again (the task's loop) */
}
int CfRc_LibHeld(void) { return _LibHold; }
int CfRc_LibBusy(void) { return _LibOp.busy; }
int CfRc_LibResult(void) { return _LibOp.res; }
int CfRc_LibDelete(uint32_t uid) { if (_LibOp.busy) return -1; _LibOp.uid = uid; return lib_ask(RC_LIB_DEL); }
void CfRc_LibPlay(uint32_t uid)
{
    if (CfRc_Playing() || _P.req == 2) { printf("rc: lib: a game is playing: stop it first\n"); return; }
    _LibUid = uid; _InRam = 0; _Src = 3; _P.notkept = 0; ask_task(1);
}
int CfRc_LibPlayingUid(void) { return (_Src == 3 && CfRc_Playing()) ? (int)_LibUid : 0; }
/* lane ui110dev: PLAY on SYSTEM > GAMES while another game plays: that one stops (its save copied), then this one starts
   (rc_task, once the stop is done) */
static volatile int _NextKind;           /* 0 none, 1 a kept game (_NextUid), 2 a card game (_NextPath) */
static uint32_t _NextUid;
static char *_NextPath;
void CfRc_PlayAfterStop(int card, uint32_t uid, const char *path)
{
    if (!CfRc_Playing() && _P.req != 2) { if (card) CfRc_PlaySd(path); else CfRc_LibPlay(uid); return; }
    char *p = card && path ? strdup(path) : NULL;
    if (card && !p) return;
    free(_NextPath);
    _NextPath = p; _NextUid = uid; _NextKind = card ? 2 : 1;
    printf("rc: the game playing stops first, then %s\n", card ? path : "the kept game");
    CfRc_Stop();
}
/* a game a send staged (src, len in this ESP32's flash: SGZ1 or the ROM, already hashed by the download): kept in the
   library when it fits (then played from it if play), else played from the staging area in RAM only. With no library
   on this unit: kept in the staging area, as 1.1.0. 0 = asked */
/* a stream's plan from the last look at the library (no FPGA op: any task); the save region planned at 64 KiB (two
   slots of 32 KiB) until the game's header says its own. 0 = it fits, tx ready */
int CfRc_LibPlanStream(uint32_t stored_size, const char *title, cf_lib_tx_t *tx)
{
    if (_LibArea != 1 || !_LibMapOk) return CF_LIB_E_NOAREA;
    cf_lib_ent_t m;
    memset(&m, 0, sizeof m);
    m.stored_size = stored_size; m.save_size = 0x8000u;
    snprintf(m.title, sizeof m.title, "%s", title ? title : "");
    return cf_lib_plan(_LibMap, _LibMapN, _LibMapSeq, _LibMapGames, &m, tx);
}
/* the first bytes of a stream hold the game's header (an SGZ1 container's copy of 0x134..0x14F at 48, or the ROM's own):
   the plan made again with the game's own save region before anything is written, so a game whose save does not fit is
   refused at once (not after the whole download) */
static int lib_replan(cf_lib_tx_t *tx, const uint8_t *b, uint32_t n)
{
    uint8_t ty, ra;
    if (n >= CF_SGZ_HDR && cf_sgz_is(b, 4)) { ty = b[48 + 0x13]; ra = b[48 + 0x15]; }
    else if (n >= 0x150u) { ty = b[0x147]; ra = b[0x149]; }
    else return 0;                                       /* (a first piece this short: the end sets the save) */
    const uint32_t sv = cf_lib_cart_save(ty, ra);
    if (sv == tx->e.save_size) return 0;
    cf_lib_ent_t m = tx->e;
    m.save_size = sv;
    cf_lib_tx_t nt;
    const int e = cf_lib_plan(_LibMap, _LibMapN, _LibMapSeq, _LibMapGames, &m, &nt);
    if (e) { printf("rc: lib: %s needs %lu bytes with its %lu-byte save: more than the library has free (%d)\n", tx->e.title,
                    (unsigned long)cf_lib_room_for(tx->e.stored_size, sv), (unsigned long)sv, e); return e; }
    *tx = nt;
    return 0;
}
int CfRc_LibTxWrite(cf_lib_tx_t *tx, const uint8_t *b, uint32_t n)
{
    if (!_LibMapOk) return CF_LIB_E_NOAREA;
    if (!tx->have) { const int e = lib_replan(tx, b, n); if (e) return e; }
    return cf_lib_tx_write(&_LF, tx, b, n);
}
int CfRc_LibStreamEnd(cf_lib_tx_t *tx)
{
    if (_LibOp.busy) return -1;
    _LibEndTx = tx;
    return lib_ask(RC_LIB_END);
}
uint32_t CfRc_LibUid(void) { return _LibOp.uid; }
/* ---- lane share1: a game in the library shared with another Chromatic (read by its extents, FREAD: each KiB checked by the
   FPGA's crc32), and a save that came with a game put into the library */
int CfRc_LibShareOpen(uint32_t uid, cf_lib_ent_t *e, cf_lib_save_t *sv)
{
    for (int t = 0; t < 100 && _LibOp.busy; t++) vTaskDelay(pdMS_TO_TICKS(100));
    if (_LibOp.busy) return -1;
    _LibOp.uid = uid;
    if (lib_ask(RC_LIB_SHOPEN)) return -1;
    for (int t = 0; t < 300 && _LibOp.busy; t++) vTaskDelay(pdMS_TO_TICKS(100));
    if (_LibOp.busy || _LibOp.res) return _LibOp.busy ? -1 : _LibOp.res;
    *e = _ShEnt; *sv = _ShSv;
    return 0;
}
int CfRc_LibShareRead(const cf_lib_ent_t *e, const cf_lib_save_t *sv, int save, uint32_t off, uint8_t *b, uint32_t n)
{
    if (_LibArea != 1 || !_LF.size) return -1;
    if (save) {
        if (!sv || !sv->seq || !e->sext.count || off + n > e->save_size) return -1;
        const uint32_t slot = ((e->save_size + CF_LIB_SECT - 1u) / CF_LIB_SECT) * CF_LIB_SECT;
        return CfRc_GameAreaRead(_LF.base + (uint32_t)e->sext.start * CF_LIB_SECT + (uint32_t)sv->slot * slot + off, b, n) ? -1 : 0;
    }
    if (off + n > e->stored_size) return -1;
    while (n) {
        uint32_t base = 0, a = 0, room = 0;
        int k = 0;
        for (; k < e->next; k++) {
            const uint32_t len = (uint32_t)e->ext[k].count * CF_LIB_SECT;
            if (off < base + len) { a = _LF.base + (uint32_t)e->ext[k].start * CF_LIB_SECT + (off - base); room = base + len - off; break; }
            base += len;
        }
        if (k == e->next) return -1;
        const uint32_t q = n < room ? n : room;
        if (CfRc_GameAreaRead(a, b, q)) return -1;
        b += q; off += q; n -= q;
    }
    return 0;
}
/* lane mygames1: the next keep / play / stream end of a Wi-Fi play send finds the game's save in the save RAM (it came with
   the game, cf_dev.c dl_work): put into the kept game (then the cartridge gets the core back unless it plays), or played with
   (a game played and not kept). Set by the nw task right before it asks; taken (and cleared) by this task. */
static volatile int _GivenNext;
void CfRc_SaveComesWith(int on) { _GivenNext = on ? 1 : 0; }
static int lib_put_given(uint32_t uid);
/* ---- lane savecloud1: SAVE TO MY GAMES. Asked by the page when a kept game's page opens: 1 yes, 0 none, -1 not known yet
   (asked; the page asks again on its next draw). Nothing is asked while a game plays (the journal is not read then). */
int CfRc_LibSaveKnown(uint32_t uid)
{
    if (_SvProbe.uid == uid && _SvProbe.st >= 2) return _SvProbe.st == 3;
    if (_SvProbe.uid == uid && _SvProbe.st == 1) return -1;
    if (_LibArea != 1 || _LibOp.busy || CfRc_Playing()) return -1;
    _SvProbe.uid = uid; _SvProbe.st = 1;
    _LibOp.uid = uid;
    if (lib_ask(RC_LIB_SVPROBE)) _SvProbe.st = 0;
    return -1;
}
void CfRc_LibSaveForget(void) { _SvProbe.st = 0; _SvProbe.uid = 0; }
/* the library closed on the task for a handshake's heap; 0 = done */
int CfRc_LibNetClose(void)
{
    for (int t = 0; t < 100 && _LibOp.busy; t++) vTaskDelay(pdMS_TO_TICKS(50));
    if (_LibOp.busy || lib_ask(RC_LIB_NETCLOSE)) return -1;
    for (int t = 0; t < 100 && _LibOp.busy; t++) vTaskDelay(pdMS_TO_TICKS(50));
    return _LibOp.busy ? -1 : 0;
}
int CfRc_LibSavePutFromRam(uint32_t uid)
{
    if (_LibOp.busy) return -1;
    _LibOp.uid = uid;
    return lib_ask(RC_LIB_SVPUT);
}
/* lane share1: room for a game of stored bytes + its save (0 = it fits; else the bytes missing; ~0u: no library here) */
uint32_t CfRc_LibRoomNeed(uint32_t stored_size, uint32_t save_size)
{
    if (_LibArea != 1 || !_LibMapOk) return ~0u;
    const uint32_t want = cf_lib_room_for(stored_size, save_size);
    if (_LibMapGames >= CF_LIB_MAX_GAMES) return want;
    return want <= _LibFreeB ? 0 : want - _LibFreeB;
}
/* recv/start: ",lib1" and the library's summary when this unit has one (the server sends a big container only then) */
void CfRc_LibSummary(char *json, size_t n, const char **caps)
{
    *caps = "";
    json[0] = 0;
    if (_LibArea != 1 || !_LibMapOk) return;
    *caps = ",lib1";
    snprintf(json, n, ",\"lib\":{\"games\":%d,\"free\":%lu}", _LibMapGames, (unsigned long)_LibFreeB);
}
int CfRc_LibKeepStaged(uint32_t src, uint32_t len, uint32_t crc, const char *title, int play)
{
    if (_LibOp.busy || CfRc_Playing()) return -1;
    _LibOp.crc = crc;
    memset(&_LibOp.meta, 0, sizeof _LibOp.meta);
    snprintf(_LibOp.meta.title, sizeof _LibOp.meta.title, "%s", title ? title : "");
    _LibOp.src = src; _LibOp.len = len; _LibOp.play = play;
    return lib_ask(RC_LIB_ADD);
}

/* a game streamed into the library's sectors (a Wi-Fi download, a USB copy in parts): its header read back from the flash
   (an SGZ1 container's, or the ROM's), the save region set to the game's own save, then the index that has it */
static int lib_stream_end(cf_lib_tx_t *tx, uint32_t *uid)
{
    /* (right after a Wi-Fi download the heap is at its lowest and in pieces: a moment, and again) */
    uint8_t *h = NULL;
    for (int t = 0; t < 10 && !(h = malloc(0x150)); t++) vTaskDelay(pdMS_TO_TICKS(300));
    if (!h) {
        printf("rc: lib: no memory to finish %s (free heap %lu, largest %lu)\n", tx->e.title, (unsigned long)esp_get_free_heap_size(),
               (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        return CF_LIB_E_FLASH;
    }
    const char *why = "";
    int e = cf_lib_tx_read(&_LF, tx, 0, h, tx->have < 0x150u ? tx->have : 0x150u);
    if (!e && cf_sgz_is(h, 4)) {
        cf_sgz_hdr_t zh;
        if (cf_sgz_parse(h, tx->e.stored_size, &zh)) e = CF_LIB_E_ARG;
        else { tx->e.flags |= CF_LIB_F_SGZ; tx->e.rom_size = zh.rom_size; tx->e.rom_crc32 = zh.rom_crc32; memset(h, 0, 0x150); memcpy(h + 0x134, zh.cart, sizeof zh.cart); }
    } else if (!e) { tx->e.rom_size = tx->e.stored_size; tx->e.rom_crc32 = tx->crc; }
    if (!e && header_parse(h, tx->e.rom_size, &why)) { printf("rc: lib: cannot play it (%s): not kept\n", why); e = CF_LIB_E_ARG; }
    if (!e) {
        if (_P.rtc) tx->e.flags |= CF_LIB_F_RTC;
        if (!tx->e.title[0]) snprintf(tx->e.title, sizeof tx->e.title, "%.16s", (const char *)h + 0x134);
        e = cf_lib_tx_set_save(_L, tx, _P.battery ? _P.sav_size : 0);
    }
    free(h);
    const int64_t t0 = esp_timer_get_time();
    const int dup = e ? -1 : cf_lib_find_rom(_L, tx->e.rom_crc32, tx->e.rom_size);   /* lane ui110dev: kept already (a stream has no check before) */
    if (!e) e = cf_lib_add_end(_L, tx, uid);
    if (!e && dup >= 0) {
        const uint32_t was = _L->ix.e[dup].uid;
        if (was != *uid && cf_lib_del(_L, *uid) == 0) { printf("rc: lib: %s is in the library already (uid %lu): the new copy let go\n", tx->e.title, (unsigned long)was); *uid = was; }
    }
    printf("rc: lib: %s streamed in (%s, %lu bytes for a %lu-byte game, save %lu): e=%d (the end's read-back %lld ms), %lu bytes free\n", tx->e.title,
           (tx->e.flags & CF_LIB_F_SGZ) ? "SGZ1" : "raw", (unsigned long)tx->e.stored_size, (unsigned long)tx->e.rom_size, (unsigned long)tx->e.save_size, e,
           (long long)((esp_timer_get_time() - t0) / 1000), (unsigned long)cf_lib_free_bytes(_L));
    return e;
}
/* ---- library ops (this task) */
static int stage_get(void *u, uint32_t off, uint8_t *b, uint32_t n) { return CfDev_FlashReadMm((uint32_t)(uintptr_t)u + off, b, n); }
static void lib_pct(void *u, int p) { (void)u; _P.pct = p; }
/* a game already in this ESP32's flash (src, len: an SGZ1 container or the ROM) into the library: 0, or a cf_lib error.
   A container is checked whole first (every block, the game's hashes); meta: title (and rom_* for a raw one) */
static int lib_add_go(uint32_t src, uint32_t len, cf_lib_ent_t *m, uint32_t *uid, uint8_t *h, uint8_t *b);
static int lib_add_from_flash(uint32_t src, uint32_t len, cf_lib_ent_t *m, uint32_t *uid)
{
    uint8_t *h = malloc(0x150 + 1024);              /* (heap: static RAM is the TLS gate's) */
    if (!h) return CF_LIB_E_FLASH;
    const int e = lib_add_go(src, len, m, uid, h, h + 0x150);
    free(h);
    return e;
}
static int lib_add_go(uint32_t src, uint32_t len, cf_lib_ent_t *m, uint32_t *uid, uint8_t *h, uint8_t *b)
{
    if (esp_flash_read(NULL, h, src, 0x150) != ESP_OK) return CF_LIB_E_FLASH;
    const char *why = "";
    m->stored_size = len;
    if (cf_sgz_is(h, 0x150)) {
        cf_sgz_hdr_t zh;
        const int ze = _LibPreZ ? _LibPreZ : cf_sgz_parse(h, len, &zh);   /* (the whole check ran before the library opened: lib_ops) */
        if (!ze && _LibPreZ == 0) zh = _LibPreH;
        if (ze) { printf("rc: lib: the container is not whole (%s)\n", cf_sgz_why(ze)); return CF_LIB_E_ARG; }
        m->flags |= CF_LIB_F_SGZ;
        m->rom_size = zh.rom_size; m->rom_crc32 = zh.rom_crc32;
        memset(h, 0, 0x150);
        memcpy(h + 0x134, zh.cart, sizeof zh.cart);
    } else {
        uint32_t c = 0;
        for (uint32_t o = 0; o < len; o += 1024u) {
            const uint32_t k = len - o < 1024u ? len - o : 1024u;
            if (esp_flash_read(NULL, b, src + o, k) != ESP_OK) return CF_LIB_E_FLASH;
            c = cf_crc32(c, b, k);
        }
        m->rom_size = len; m->rom_crc32 = c;
    }
    if (header_parse(h, m->rom_size, &why)) { printf("rc: lib: cannot play it (%s): not kept\n", why); return CF_LIB_E_ARG; }
    m->save_size = _P.battery ? _P.sav_size : 0;
    if (_P.rtc) m->flags |= CF_LIB_F_RTC;
    if (!m->title[0]) snprintf(m->title, sizeof m->title, "%.16s", (const char *)h + 0x134);
    const int f = cf_lib_find_rom(_L, m->rom_crc32, m->rom_size);
    if (f >= 0) { *uid = _L->ix.e[f].uid; printf("rc: lib: %s is in the library already\n", _L->ix.e[f].title); return 0; }
    const int64_t t0 = esp_timer_get_time();
    const int e = cf_lib_add(_L, m, stage_get, (void *)(uintptr_t)src, uid, lib_pct);
    printf("rc: lib: add %s (%s, %lu bytes for a %lu-byte game, save %lu): e=%d in %lld ms, %lu bytes free\n", m->title, (m->flags & CF_LIB_F_SGZ) ? "SGZ1" : "raw",
           (unsigned long)len, (unsigned long)m->rom_size, (unsigned long)m->save_size, e, (long long)((esp_timer_get_time() - t0) / 1000), (unsigned long)cf_lib_free_bytes(_L));
    _LibGen++;
    return e;
}
/* 1.1.0's kept game (the ESP32's idle slot) moves into the library once there is one: its save from this unit's store */
static void lib_migrate(void)
{
    if (!_GameOk || lib_open()) return;
    cf_lib_ent_t m;
    memset(&m, 0, sizeof m);
    snprintf(m.title, sizeof m.title, "%s", _Game.title);
    uint32_t uid = 0;
    _P.battery = 0; _P.sav_size = 0; _P.rtc = 0;
    const int e = lib_add_from_flash(_Game.base, _Game.size, &m, &uid);
    if (e) { printf("rc: lib: 1.1.0's kept game %s stays where it is for now (%d)\n", _Game.title, e); return; }
    /* its save (the store keeps it by the game's crc32): into the library too */
    (void)index_load();
    const int ent = m.save_size ? ent_for(m.rom_crc32, m.save_size, 0) : -1;
    if (ent >= 0 && _Ix.e[ent].dcrc) {
        _LibUid = uid;
        const uint32_t base = store_base() + (uint32_t)_Ix.e[ent].sect * 4096u;
        const int se = cf_lib_save_put_t(_L, uid, m.save_size, _Ix.e[ent].rtc, sdc_tsec_pack(ix_tm(ent)), stage_get, (void *)(uintptr_t)base);
        printf("rc: lib: its save into the library: e=%d\n", se);
        if (se) return;                                 /* (the game stays kept as before too: done again next time) */
    }
    printf("rc: lib: 1.1.0's kept game %s is in the library now: the ESP32 copy is let go\n", _Game.title);
    game_store(0);
}
/* ---- lane ui110dev: SYSTEM > GAMES's copies between the library and the card / the staging area (on this task; the card's
   task asks and waits: sd.c kSdOp_Job, cf_dev.c gj_*). The library is closed while a game unpacks (its ~5.5 KB is the
   unpack's) and read by the entry's extents, as a library play does; the link is off (load_quiet_begin). */
static struct {
    char path[SDC_PATH_MAX];            /* RC_LIB_SAVE_CARD: the save file on the card; RC_LIB_TO_SD: the game written */
    uint32_t src, size;                 /* RC_LIB_SAVE_STAGE: the save in this ESP32's flash */
    uint32_t out_size, out_crc;         /* RC_LIB_TO_STAGE: the game's size and crc32 as written */
    uint8_t  out_sha[32];
    volatile int pct;
    const char *why;                    /* a failure in the page's words ("ROW1|ROW2"), NULL none */
} _Cp;
typedef struct {
    int (*put)(void *u, uint32_t off, const uint8_t *b, uint32_t n);
    void *pu;
    uint32_t crc, total;
    mbedtls_sha256_context sha;
} cp_sink_t;
static int cp_put(void *u, uint32_t off, const uint8_t *b, uint32_t n)
{
    cp_sink_t *k = u;
    k->crc = cf_crc32(k->crc, b, n);
    (void)mbedtls_sha256_update(&k->sha, b, n);
    if (k->total) _Cp.pct = (int)((uint64_t)(off + n) * 100u / k->total);
    return k->put(k->pu, off, b, n);
}
static int cp_rd(void *u, uint32_t off, uint8_t *b, uint32_t n) { (void)u; return ent_rd(off, b, n, 0); }
/* the game (uid) unpacked (or copied, a raw one) in order into put; *crc / sha = what put got. 0, else a cf_lib / cf_sgz error */
static int lib_game_out(uint32_t uid, int (*put)(void *u, uint32_t off, const uint8_t *b, uint32_t n), void *pu, cf_lib_ent_t *eo, uint32_t *crc, uint8_t sha[32])
{
    if (lib_open()) return CF_LIB_E_NOAREA;
    const int i = cf_lib_find(_L, uid);
    if (i < 0) return CF_LIB_E_NONE;
    _LoadEnt = _L->ix.e[i];
    *eo = _LoadEnt;
    free(_L); _L = NULL;                                /* (its heap is the unpack's; opened again after) */
    cp_sink_t *k = malloc(sizeof *k);
    if (!k) return CF_SGZ_E_MEM;
    k->put = put; k->pu = pu; k->crc = 0; k->total = _LoadEnt.rom_size;
    mbedtls_sha256_init(&k->sha);
    (void)mbedtls_sha256_starts(&k->sha, 0);
    int e = 0;
    if (_LoadEnt.flags & CF_LIB_F_SGZ) {
        cf_sgz_hdr_t h;
        for (int t = 0; t < 3; t++) { e = cf_sgz_unpack(cp_rd, cp_put, k, _LoadEnt.stored_size, &h, NULL); if (e != CF_SGZ_E_MEM) break; vTaskDelay(pdMS_TO_TICKS(300)); }
    } else {
        uint8_t *b = malloc(1024);
        if (!b) e = CF_SGZ_E_MEM;
        for (uint32_t o = 0; !e && o < _LoadEnt.rom_size; o += 1024u) {
            const uint32_t n = _LoadEnt.rom_size - o < 1024u ? _LoadEnt.rom_size - o : 1024u;
            if (ent_rd(o, b, n, 0)) e = CF_SGZ_E_READ;
            else if (cp_put(k, o, b, n)) e = CF_SGZ_E_WRITE;
        }
        free(b);
        if (!e && k->crc != _LoadEnt.rom_crc32) e = CF_SGZ_E_ROM;
    }
    (void)mbedtls_sha256_finish(&k->sha, sha);
    mbedtls_sha256_free(&k->sha);
    *crc = k->crc;
    free(k);
    (void)lib_open();
    return e;
}
static int cp_card_put(void *u, uint32_t off, const uint8_t *b, uint32_t n) { return Sd_WriteAt((sd_wr_t *)u, off, b, n) ? -1 : 0; }
int CfDev_StageWrite(uint32_t off, const uint8_t *b, uint32_t n);   /* cf_dev.c: into the staging area (erased first) */
static int cp_stage_put(void *u, uint32_t off, const uint8_t *b, uint32_t n) { (void)u; return CfDev_StageWrite(off, b, n) ? -1 : 0; }
/* the game (uid) and its save onto the card: /GAMES/<its name>.gb(c) (a free name), its save /SAVES/<the same>.sav
   (+ the clock's footer for an MBC3 clock game). 0 = both there (_Cp.path = the game's file) */
static int lib_to_sd(uint32_t uid)
{
    _Cp.why = "NOT  COPIED|TRY  AGAIN";
    if (Sd_Mount() != 0 || !Sd_Writable()) { _Cp.why = "NOT  COPIED|CARD  NOT  READY"; return -1; }
    if (lib_open()) return -1;
    const int i = cf_lib_find(_L, uid);
    if (i < 0) return -1;
    const cf_lib_ent_t e0 = _L->ix.e[i];
    /* its name on the card from its header's colour flag and its title */
    uint8_t h[CF_SGZ_HDR];
    int cgb = 0;
    if (cf_lib_read(_L, uid, 0, h, sizeof h)) return -1;
    if (e0.flags & CF_LIB_F_SGZ) cgb = (h[48 + 0x143 - 0x134] & 0x80) != 0;
    else { uint8_t c = 0; if (cf_lib_read(_L, uid, 0x143, &c, 1) == 0) cgb = (c & 0x80) != 0; }
    char file[64], path[SDC_PATH_MAX];
    if (sdc_clean_file(e0.title[0] ? e0.title : "GAME", cgb ? ".gbc" : ".gb", file, sizeof file)) snprintf(file, sizeof file, "GAME%s", cgb ? ".gbc" : ".gb");
    if (Sd_FreeName(SDC_GAMES_DIR, file, path, sizeof path)) { _Cp.why = "NOT  COPIED|CARD  WRITE  FAILED"; return -1; }
    const uint32_t free_mb_need = (e0.rom_size + e0.save_size) / (1024u * 1024u) + 1u;
    { uint32_t t = 0, f = 0; Sd_Space(&t, &f); if (f && f < free_mb_need) { _Cp.why = "NOT  COPIED|THE  CARD  IS  FULL"; return -1; } }
    sd_wr_t *w = malloc(sizeof *w);
    if (!w) return -1;
    int er = Sd_WriteBegin(w, path, e0.rom_size);
    cf_lib_ent_t e;
    uint32_t crc = 0;
    uint8_t sha[32];
    if (!er) er = lib_game_out(uid, cp_card_put, w, &e, &crc, sha);
    if (!er) er = Sd_WriteCommit(w);
    else if (w->open) Sd_WriteAbort(w);
    free(w);
    printf("rc: lib: %s onto the card as %s: e=%d (crc32 %08lx)\n", e0.title, path, er, (unsigned long)crc);
    if (er) { _Cp.why = er == CF_SGZ_E_WRITE ? "NOT  COPIED|CARD  WRITE  FAILED" : "NOT  COPIED|TRY  AGAIN"; return -1; }
    snprintf(_Cp.path, sizeof _Cp.path, "%s", path);
    /* its save, when it has one */
    cf_lib_save_t sv;
    if (e0.save_size && _L && cf_lib_save_get(_L, uid, &sv) == 0 && sv.seq) {
        char sp[SDC_PATH_MAX + 8];
        uint8_t *b = malloc(1024);
        sd_wr_t *ws = malloc(sizeof *ws);
        int se = !b || !ws || sdc_save_path(path, sp, sizeof sp) || Sd_WriteBegin(ws, sp, 0);
        for (uint32_t o = 0; !se && o < e0.save_size; o += 1024u) {
            const uint32_t n = e0.save_size - o < 1024u ? e0.save_size - o : 1024u;
            se = cf_lib_save_read(_L, uid, &sv, o, b, n) || Sd_WriteAt(ws, o, b, n);
        }
        if (!se && (e0.flags & CF_LIB_F_RTC)) { uint8_t ft[SDC_RTC_FOOTER]; sdc_rtc_put(ft, sv.rtc, sdc_tsec_unpack(sv.tsec, CwlTime_Anchor()));   /* (1.1.7: when the clock was true, not now) */ se = Sd_WriteAt(ws, e0.save_size, ft, sizeof ft); }
        if (!se) se = Sd_WriteCommit(ws); else if (ws && ws->open) Sd_WriteAbort(ws);
        free(b); free(ws);
        printf("rc: lib: its save (%lu bytes) onto the card as %s: e=%d\n", (unsigned long)e0.save_size, sp, se);
        if (se) { _Cp.why = "GAME  COPIED|ITS  SAVE  WAS  NOT"; return -2; }
    }
    _Cp.why = NULL;
    return 0;
}
/* the game (uid) unpacked into this ESP32's staging area (erased before: the card's task did it), for a cartridge write */
static int lib_to_stage(uint32_t uid)
{
    cf_lib_ent_t e;
    _Cp.why = "NOT  WRITTEN|TRY  AGAIN";
    if (lib_open()) return -1;
    const int i = cf_lib_find(_L, uid);
    if (i < 0) return -1;
    if (_L->ix.e[i].rom_size > CF_INST_WRITABLE_RC) { _Cp.why = "NOT  WRITTEN|TOO  BIG  FOR  A  CART"; return -1; }
    const int er = lib_game_out(uid, cp_stage_put, NULL, &e, &_Cp.out_crc, _Cp.out_sha);
    _Cp.out_size = e.rom_size;
    printf("rc: lib: %s into the staging area for the cartridge: e=%d (%lu bytes, crc32 %08lx)\n", e.title, er, (unsigned long)e.rom_size, (unsigned long)_Cp.out_crc);
    if (er) return -1;
    _Cp.why = NULL;
    return 0;
}
/* a save into the library for game uid: from the card (_Cp.path, with the clock's footer) or this ESP32's flash (_Cp.src) */
typedef struct { FIL *f; uint32_t fsize, src; } sv_in_t;
static int sv_in_get(void *u, uint32_t off, uint8_t *b, uint32_t n)
{
    sv_in_t *s = u;
    memset(b, 0xFF, n);
    if (s->f) { uint32_t got = 0; if (off < s->fsize) { const uint32_t k = s->fsize - off < n ? s->fsize - off : n; if (Sd_Read(s->f, off, b, k, &got)) return -1; } return 0; }
    return esp_flash_read(NULL, b, s->src + off, n) == ESP_OK ? 0 : -1;
}
static int lib_save_in(uint32_t uid, int from_card)
{
    if (lib_open()) return -1;
    const int i = cf_lib_find(_L, uid);
    if (i < 0) return -1;
    const uint32_t size = _L->ix.e[i].save_size;
    if (!size) return 0;                                /* (a game with no save) */
    sv_in_t s = { NULL, 0, _Cp.src };
    uint8_t rtc[5] = { 0 };
    uint64_t tk = 0;                                    /* 1.1.7: when the clock was true (0 = not known) */
    if (from_card) {
        s.f = malloc(sizeof *s.f);
        if (!s.f) return -1;
        if (Sd_ReadOpen(s.f, _Cp.path, &s.fsize) != 0) { free(s.f); return 0; }   /* (no save on the card: a blank one) */
        if ((_L->ix.e[i].flags & CF_LIB_F_RTC) && s.fsize >= size + 44u) {
            uint8_t ft[48]; uint32_t got = 0; uint64_t t = 0;
            const uint32_t fl = s.fsize - size >= 48u ? 48u : 44u;
            if (Sd_Read(s.f, size, ft, fl, &got) == 0 && got == fl && sdc_rtc_get(ft, fl, rtc, &t) == 0) {
                const uint64_t now = Sd_UnixTime();
                tk = t;
                if (now && t && now > t) { sdc_rtc_advance(rtc, now - t); tk = now; }
            }
        }
    }
    const uint32_t tsec = sdc_tsec_pack(tk);
    int e = cf_lib_save_put_t(_L, uid, size, rtc, tsec, sv_in_get, &s);
    if (e == CF_LIB_E_FLASH) {
        /* (unit 1, the first save into a library just made: one flash op refused; the FPGA's op engine reset, once more) */
        printf("rc: lib: the save's flash op failed: again\n");
        (void)CfRc_Reset();
        vTaskDelay(pdMS_TO_TICKS(200));
        e = cf_lib_save_put_t(_L, uid, size, rtc, tsec, sv_in_get, &s);
    }
    if (s.f) { Sd_Close(s.f); free(s.f); }
    printf("rc: lib: a save (%lu bytes) into the library for game %lu from %s: e=%d\n", (unsigned long)size, (unsigned long)uid, from_card ? _Cp.path : "the staging area", e);
    return e;
}
int CfRc_LibCopy(int what, uint32_t uid, const char *path, uint32_t src)
{
    if (_LibOp.busy || CfRc_Playing()) return -1;
    _LibOp.uid = uid;
    _Cp.src = src; _Cp.pct = 0; _Cp.why = NULL;
    snprintf(_Cp.path, sizeof _Cp.path, "%s", path ? path : "");
    return lib_ask(what == 0 ? RC_LIB_TO_SD : what == 1 ? RC_LIB_TO_STAGE : what == 2 ? RC_LIB_SAVE_CARD : RC_LIB_SAVE_STAGE);
}
const char *CfRc_LibCopyWhy(void) { return _Cp.why; }
int CfRc_LibCopyPct(void) { return _Cp.pct; }
const char *CfRc_LibCopyPath(void) { return _Cp.path; }
void CfRc_LibCopyStaged(uint32_t *size, uint32_t *crc, uint8_t sha[32]) { *size = _Cp.out_size; *crc = _Cp.out_crc; memcpy(sha, _Cp.out_sha, 32); }

static void lib_ops(void)
{
    const int op = _LibOp.op;
    if (!op) return;
    _LibOp.op = 0;
    if (op == RC_LIB_ADD) {
        /* the whole container checked first, with the library closed: the unpack's heap (~12 KB) and the library's (~5 KB)
           do not fit together; reopened after (~0.2 s) */
        uint8_t mg[4] = { 0 };
        _LibPreZ = 0;
        if (esp_flash_read(NULL, mg, _LibOp.src, 4) == ESP_OK && cf_sgz_is(mg, 4)) {
            if (_L) { free(_L); _L = NULL; }
            net_hold_for_unpack();
            const uint32_t fh = esp_get_free_heap_size(), big = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
            for (int t = 0; t < 10; t++) {
                _LibPreZ = cf_sgz_verify(zf_rd, (void *)(uintptr_t)_LibOp.src, _LibOp.len, &_LibPreH);
                if (_LibPreZ != CF_SGZ_E_MEM) break;
                vTaskDelay(pdMS_TO_TICKS(300));
            }
            printf("rc: lib: the container checked whole: %s (free heap %lu, largest %lu)\n", cf_sgz_why(_LibPreZ), (unsigned long)fh, (unsigned long)big);
            CfDev_FlashMmDrop();
        }
    }
    if (op == RC_LIB_ADD && _LibOp.play == 2) {
        /* lane ui110dev ("lib": "none"): played from the staging area in RAM only, nothing kept; the library closed first */
        if (_L) { free(_L); _L = NULL; }
        if (_Snap) { portENTER_CRITICAL(&_LibMux); void *sn = _Snap; _Snap = NULL; portEXIT_CRITICAL(&_LibMux); free(sn); }
        memset(&_TmpGame, 0, sizeof _TmpGame);
        _TmpGame.base = _LibOp.src; _TmpGame.size = _LibOp.len;
        snprintf(_TmpGame.title, sizeof _TmpGame.title, "%.16s", _LibOp.meta.title);
        _InRam = 0; _Src = 4; _P.notkept = 0;
        _SaveGiven = _GivenNext; _GivenNext = 0;      /* lane mygames1 */
        (void)play_now();
        net_release();
        _LibOp.res = 0;
        _LibOp.busy = 0;
        _LibGen++;
        return;
    }
    if (op == RC_LIB_NETCLOSE) {
        /* lane savecloud1: the library closed for a TLS handshake's heap (SAVE TO MY GAMES reads the save by its extents, not
           through the open library); the page's copy of the list stays, the page opens it again when the job is over */
        if (_L && !(_Src == 3 && CfRc_Playing())) { free(_L); _L = NULL; }
        _SnapWant = 0;
        _LibOp.res = 0;
        _LibOp.busy = 0;
        return;
    }
    const int given = (op == RC_LIB_ADD || op == RC_LIB_END) ? _GivenNext : 0;   /* lane mygames1 */
    if (given) _GivenNext = 0;
    int e = lib_open();
    if (!e && op == RC_LIB_DEL) {
        if (CfRc_Playing() && _Src == 3 && _LibUid == _LibOp.uid) e = CF_LIB_E_ARG;   /* (not the game playing) */
        else e = cf_lib_del(_L, _LibOp.uid);
        printf("rc: lib: delete %lu e=%d, %lu bytes free\n", (unsigned long)_LibOp.uid, e, (unsigned long)(_L ? cf_lib_free_bytes(_L) : 0));
        _LibGen++;
    } else if (!e && op == RC_LIB_ADD) {
        uint32_t uid = 0;
        _P.st = kNwPv_Loading; _P.pct = 0;
        snprintf(_RamGame.title, sizeof _RamGame.title, "%.16s", _LibOp.meta.title);
        e = lib_add_from_flash(_LibOp.src, _LibOp.len, &_LibOp.meta, &uid);
        _P.st = _GameOk ? kNwPv_Ready : kNwPv_None;
        _LibOp.uid = uid;
        if (!e && given) (void)lib_put_given(uid);     /* lane mygames1: before it plays (the play loads the kept save) */
        if (!_LibOp.play && given) CfRc_RamRelease();
        if (_LibOp.play) {
            if (!e) { _LibUid = uid; _InRam = 0; _Src = 3; _P.notkept = 0; (void)play_now(); }
            else {
                /* not kept (full, or no room for one more): played from the staging area, in RAM only. The library closed first
                   (its ~5.5 KB and the list's copy back: unit 1 2026-10-22, a staged Yellow SGZ1 got 13 KB free in pieces of 3.8 KB) */
                if (_L) { free(_L); _L = NULL; }
                if (_Snap) { portENTER_CRITICAL(&_LibMux); void *sn = _Snap; _Snap = NULL; portEXIT_CRITICAL(&_LibMux); free(sn); }
                memset(&_TmpGame, 0, sizeof _TmpGame);
                _TmpGame.base = _LibOp.src; _TmpGame.size = _LibOp.len;
                snprintf(_TmpGame.title, sizeof _TmpGame.title, "%.16s", _LibOp.meta.title);
                _InRam = 0; _Src = 4; _P.notkept = e == CF_LIB_E_FULL || e == CF_LIB_E_MANY ? 1 : 2;
                _SaveGiven = given;                    /* lane mygames1 */
                (void)play_now();
            }
        }
    } else if (op == RC_LIB_MIGRATE) lib_migrate();
    else if (!e && op == RC_LIB_FMT) { e = cf_lib_format(_L); printf("rc: lib: format e=%d\n", e); }
    else if (!e && op == RC_LIB_END) {
        uint32_t uid = 0;
        e = lib_stream_end(_LibEndTx, &uid);
        _LibOp.uid = uid;
        if (given) { if (!e) (void)lib_put_given(uid); CfRc_RamRelease(); }   /* lane mygames1 (a play after it holds the core again) */
        (void)release_if_held("lib: streamed in");                          /* 1.1.2: never left dark after a keep */
    }
    else if (!e && (op == RC_LIB_TO_SD || op == RC_LIB_TO_STAGE)) {
        /* lane ui110dev: the link off for the unpack's heap, as a play */
        const bool held = load_quiet_begin("copy");
        e = op == RC_LIB_TO_SD ? lib_to_sd(_LibOp.uid) : lib_to_stage(_LibOp.uid);
        load_quiet_end(held);
    }
    else if (!e && (op == RC_LIB_SAVE_CARD || op == RC_LIB_SAVE_STAGE)) e = lib_save_in(_LibOp.uid, op == RC_LIB_SAVE_CARD);
    else if (!e && op == RC_LIB_SHOPEN) {
        /* lane share1: a copy of the game's entry and its newest save record, for a share to read from (by the extents) */
        const int i = cf_lib_find(_L, _LibOp.uid);
        if (i < 0) e = CF_LIB_E_NONE;
        else { _ShEnt = _L->ix.e[i]; memset(&_ShSv, 0, sizeof _ShSv); if (_ShEnt.save_size && cf_lib_save_get(_L, _ShEnt.uid, &_ShSv)) memset(&_ShSv, 0, sizeof _ShSv); }
    }
    else if (!e && op == RC_LIB_SVPROBE) {
        /* lane savecloud1: does the kept game have a save in the journal (whole: its size is the game's)? */
        cf_lib_save_t sv;
        const int i = cf_lib_find(_L, _LibOp.uid);
        int has = 0;
        if (i >= 0 && _L->ix.e[i].save_size && cf_lib_save_get(_L, _LibOp.uid, &sv) == 0 && sv.seq && sv.size == _L->ix.e[i].save_size) has = 1;
        if (_SvProbe.uid == _LibOp.uid) _SvProbe.st = has ? 3 : 2;
    }
    else if (!e && op == RC_LIB_SVPUT) {
        /* lane share1: the save that came with a shared game (in the save RAM) as the game's save in the library */
        const int i = cf_lib_find(_L, _LibOp.uid);
        static const uint8_t rt0[5] = { 0 };
        if (i < 0) e = CF_LIB_E_NONE;
        else if (!_L->ix.e[i].save_size) e = 0;
        else { e = cf_lib_save_put(_L, _LibOp.uid, _L->ix.e[i].save_size, rt0, lib_sv_get, NULL); printf("rc: lib: the save that came with it into the library: e=%d (%lu bytes)\n", e, (unsigned long)_L->ix.e[i].save_size); }
    }
    else if (e && op == RC_LIB_ADD && _LibOp.play) {
        /* no library (this FPGA flash has none): the 1.1.0 way, the game kept in the staging area */
        CfRc_Keep(_LibOp.src, _LibOp.len, _LibOp.crc, _LibOp.meta.title);
        _InRam = 0; _Src = 0; _SaveGiven = given; (void)play_now();
    }
    if (op == RC_LIB_SVPROBE && _SvProbe.st == 1) _SvProbe.st = 0;     /* (not answered: asked again) */
    lib_snap();
    net_release();
    _LibOp.res = e;
    _LibOp.busy = 0;
    _LibGen++;
}

/* lane mygames1: the save in the save RAM (it came with the game) as kept game uid's save; this task, the library open */
static int lib_put_given(uint32_t uid)
{
    const int i = _L ? cf_lib_find(_L, uid) : -1;
    static const uint8_t rt0[5] = { 0 };
    if (i < 0) return CF_LIB_E_NONE;
    if (!_L->ix.e[i].save_size) { printf("rc: lib: the save that came with it: the game keeps none\n"); return 0; }
    const int e = cf_lib_save_put(_L, uid, _L->ix.e[i].save_size, rt0, lib_sv_get, NULL);
    printf("rc: lib: the save that came with it into the kept game: e=%d (%lu bytes)\n", e, (unsigned long)_L->ix.e[i].save_size);
    return e;
}

static uint32_t _StopDone;
uint32_t CfRc_StopDone(void) { return __atomic_load_n(&_StopDone, __ATOMIC_ACQUIRE); }

static void stop_now(void)
{
    printf("[%u ms] rc: stop: start (state %d, pending %d)\n", (unsigned)esp_log_timestamp(), _P.st, _P.pend);
    _SdStopping = 1;
    int save = 0;
    if (_P.pend || _P.st == kNwPv_Playing) {
        printf("[%u ms] rc: stop: save copy start (%lu bytes)\n", (unsigned)esp_log_timestamp(), (unsigned long)_P.sav_size);
        const int64_t t0 = esp_timer_get_time();
        save = save_copy();
        printf("[%u ms] rc: stop: save copy end (%lu bytes, result %d, %lld ms)\n", (unsigned)esp_log_timestamp(), (unsigned long)_P.sav_size, save, (long long)(esp_timer_get_time() - t0) / 1000);
    }
    _SdStopping = 0;
    const int hold = cfg_now(CF_RC_A_ON | CF_RC_A_HOLD);
    printf("[%u ms] rc: stop: step 1 ON|HOLD (result %d)\n", (unsigned)esp_log_timestamp(), hold);
    const int64_t idle_t0 = esp_timer_get_time();
    uint8_t ramcart_status = _St[7];
    int ramcart_idle = 0;
    while ((esp_timer_get_time() - idle_t0) < 500000) {
        const uint32_t n0 = _StN;
        const uint8_t s = CF_XS_P_STATUS;
        xs_send(&s, 1);
        while (_StN == n0 && (esp_timer_get_time() - idle_t0) < 500000)
            vTaskDelay(pdMS_TO_TICKS(1));
        if (_StN != n0) {
            ramcart_status = _St[7];
            if (!(ramcart_status & 0x08)) {
                ramcart_idle = 1;
                break;
            }
        }
        if ((esp_timer_get_time() - idle_t0) < 500000) vTaskDelay(pdMS_TO_TICKS(5));
    }
    const unsigned idle_ms = (unsigned)((esp_timer_get_time() - idle_t0) / 1000);
    if (ramcart_idle)
        printf("[%u ms] rc: stop: step 2 ram cart idle after %u ms (status 0x%02x)\n", (unsigned)esp_log_timestamp(), idle_ms, ramcart_status);
    else
        printf("[%u ms] rc: stop: step 2 busy timeout (status 0x%02x)\n", (unsigned)esp_log_timestamp(), ramcart_status);
    const int switching_card = _NextKind == 2 && _Src == 2;
    int release = 0;
    if (switching_card) {
        _SdKeepOn = 1;
        printf("[%u ms] rc: stop: next card game, retain ON|HOLD (flags 0x03, result %d)\n", (unsigned)esp_log_timestamp(), hold);
    } else {
        const int cart = CfRc_CartIn();   /* diagnostic only for a card game: never power the slot */
        if (_Src == 2)
            printf("[%u ms] rc: stop: cartridge detect %d -> no-slot path (card game)\n", (unsigned)esp_log_timestamp(), cart);
        else
            printf("[%u ms] rc: stop: cartridge detect %d\n", (unsigned)esp_log_timestamp(), cart);
        if (_Src == 2) {
            _SdKeepOn = 1;
            /* Replace the ROM while held; never expose the old game or power the slot. */
            const uint32_t size = 32768u;
            uint8_t *blank = heap_caps_malloc(1024, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
            uint32_t checked = 0, misses = 0, staged = 0;
            int e = hold ? hold : !ramcart_idle ? -1 : blank ? 0 : -1;
            if (blank) memset(blank, 0xFF, 1024);
            while (!e && staged < size) {
                e = sd_ram_write(CF_RC_ROM_BASE + staged, blank, NULL, 1, &checked, &misses);
                if (!e) staged += 1024u;
            }
            if (blank) heap_caps_free(blank);
            printf("rc: stop: empty cart staged (%u bytes, result %d)\n", (unsigned)staged, e);
            const uint8_t flags = CF_RC_A_ON | CF_RC_A_HOLD | CF_RC_A_MRESET | CF_RC_A_INVAL | CF_RC_A_CLEAN;
            if (!e) e = CfRc_Cfg(0, 2, 0, 0, flags);   /* no MBC, two 16 KiB banks, no RAM or RTC */
            if (!e) {
                vTaskDelay(pdMS_TO_TICKS(50));
                e = CfRc_Cfg(0, 2, 0, 0, CF_RC_A_ON);
            }
            release = e;                               /* on failure, leave HOLD set */
            printf("rc: stop: empty cart released (flags 0x%02x, result %d)\n", CF_RC_A_ON, release);
            const uint32_t n0 = _StN;
            const uint8_t s = CF_XS_P_STATUS;
            xs_send(&s, 1);
            for (int i = 0; i < 50 && _StN == n0; i++) vTaskDelay(pdMS_TO_TICKS(2));
            const int fresh = _StN != n0;
            const uint8_t rc = _St[7];
            printf("rc: stop: status on=%d hold=%d busy=%d\n", fresh ? !!(rc & 0x20) : -1,
                   fresh ? !!(rc & 0x40) : -1, fresh ? !!(rc & 0x08) : -1);
        } else if (!cart) {
            _SdKeepOn = 1;
            const uint8_t flags = CF_RC_A_ON | CF_RC_A_HOLD | CF_RC_A_MRESET | CF_RC_A_INVAL;
            release = cfg_now(flags);
            printf("[%u ms] rc: stop: no cartridge, keep slot off (flags 0x%02x, result %d)\n", (unsigned)esp_log_timestamp(), flags, release);
        } else {
            _SdKeepOn = 0;
            const int off = CfRc_Cfg(0, 0, 0, 0, CF_RC_A_HOLD);
            printf("[%u ms] rc: stop: step 3 cart off, HOLD (result %d)\n", (unsigned)esp_log_timestamp(), off);
            vTaskDelay(pdMS_TO_TICKS(300));                     /* the slot powers up (~70 ms) */
            printf("[%u ms] rc: stop: step 3 slot settled\n", (unsigned)esp_log_timestamp());
            release = CfRc_Cfg(0, 0, 0, 0, 0);
            printf("[%u ms] rc: stop: step 4 release (result %d)\n", (unsigned)esp_log_timestamp(), release);
        }
    }
    _P.ent = -1;
    _P.st = _GameOk ? kNwPv_Ready : kNwPv_None;
    if (_Buf) { heap_caps_free(_Buf); _Buf = NULL; }   /* (the window buffer: back to the heap with the game) */
    _P.notkept = 0;
    printf("rc: %s\n", _Src == 2 && !switching_card && !release ? "empty RAM cart, slot off" : _SdKeepOn ? "RAM cart held, slot off" : "back to the cartridge");
    printf("[%u ms] rc: stop: end (save %d, hold %d, release %d)\n", (unsigned)esp_log_timestamp(), save, hold, release);
    __atomic_add_fetch(&_StopDone, 1u, __ATOMIC_RELEASE);
}

/* the task lives only while there is something to do (a load, a game playing, a request): its 4 KiB stack is
   heap the TLS gate needs (receive mode, the update check) the rest of the time */
static portMUX_TYPE _TaskMux = portMUX_INITIALIZER_UNLOCKED;
/* gamecomp1: after a keep the report's TLS handshake needs ~21 KB: the library task ends (its stack back) and closes the library
   half a second after its last op; wait for that (up to ms), then the window buffer back too. 1 = settled */
int CfRc_LibSettle(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 50) {
        portENTER_CRITICAL(&_TaskMux);
        const int gone = _P.task == NULL;
        portEXIT_CRITICAL(&_TaskMux);
        if (gone && !_LibOp.busy) { CfRc_Trim(); if (!CfRc_SdPlaying() && Sd_Busy() == kSdOp_None) Sd_Lend(); return 1; }   /* (the card's ~1.4 KB too) */
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    CfRc_Trim();
    return 0;
}
static volatile uint32_t _PlayDone;      /* m110s: plays finished (started or not) */
uint32_t CfRc_PlayDone(void) { return _PlayDone; }
static void rc_task(void *arg)
{
    (void)arg;
    int idle = 0;
    for (;;) {
        const int r = _P.req;
        if (r == 1) { _P.req = 0; (void)play_now(); _PlayDone++; }
        else if (r == 2) { _P.req = 0; if (_P.st == kNwPv_Playing) stop_now(); }
        if (_NextKind && _P.req == 0 && !CfRc_Playing()) {
            /* lane ui110dev: the game asked for after the stop */
            const int k = _NextKind;
            printf("[%u ms] rc: queued next game: take start (kind %d, stop state %d)\n", (unsigned)esp_log_timestamp(), k, _P.st);
            _NextKind = 0;
            if (k == 2 && _NextPath) { snprintf(_SdPath, sizeof _SdPath, "%s", _NextPath); _Src = 2; }
            else { _LibUid = _NextUid; _Src = 3; _P.notkept = 0; }
            free(_NextPath); _NextPath = NULL;
            _InRam = 0;
            _P.req = 1;
            printf("[%u ms] rc: queued next game: take end (request %d, source %d)\n", (unsigned)esp_log_timestamp(), _P.req, _Src);
            continue;
        }
        else if (r == 3) { _P.req = 0; if (_P.st == kNwPv_Result || _P.st == kNwPv_NoFpga) _P.st = _GameOk ? kNwPv_Ready : kNwPv_None; }
        else if (r == 4) _P.req = 0;
        lib_ops();                                      /* lane gamecomp1 */
        if (_P.st == kNwPv_Playing) {
            /* the save: the FPGA's dirty flag, cleared at once; copied once writes have stopped */
            _PollUs = esp_timer_get_time();
            const uint8_t s = CF_XS_P_STATUS;
            const uint32_t n0 = _StN;
            xs_send(&s, 1);
            for (int i = 0; i < 25 && _StN == n0; i++) vTaskDelay(pdMS_TO_TICKS(2));
            const uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
            if (_StN != n0 && !(_St[7] & 0x20)) {
                /* the FPGA is not in RAM cart mode (reloaded, a cart put in): the game is gone */
                printf("rc: the RAM cart stopped by itself (FPGA reloaded?)\n");
                _P.ent = -1; _P.st = _GameOk ? kNwPv_Ready : kNwPv_None;
            } else if (_StN != n0 && (_St[7] & 0x10)) {
                (void)cfg_now(CF_RC_A_ON | CF_RC_A_CLEAN);
                if (!_P.pend) _P.pend_first = now;
                _P.pend = 1; _P.pend_at = now;
            }
            /* lane sdcard1: a clock game from the card: its clock copied once a minute too (a cut loses at most that) */
            if (_Src == 2 && _P.rtc && !_P.pend && now - _P.last_save >= 60000u) { _P.pend = 1; _P.pend_at = now - RC_QUIET_MS; _P.pend_first = now; _SdStopping = 1; }
            if (_P.pend && (now - _P.pend_at >= RC_QUIET_MS || now - _P.pend_first >= RC_MAX_MS)) {
                _P.pend = 0;
                _SaveBusy = 1;                       /* savefast1: no nap inside a copy or its erase ahead */
                const int sc = save_copy();
                _SaveBusy = 0;
                _SdStopping = 0;
                if (sc < 0 && _Src != 2) printf("rc: save: the copy to the store failed (tried again)\n");
                if (sc) { _P.pend = 1; _P.pend_at = now; }
            }
        }
        if (_SnapWant && _L && !_LibOp.busy && (uint32_t)(esp_timer_get_time() / 1000) - _SnapTried >= 3000u) lib_snap();   /* m110q */
        if (_P.st == kNwPv_Playing || _P.st == kNwPv_Loading || _P.req || _LibOp.busy) idle = 0;
        else if (++idle >= 5) {
            lib_close_if_idle();                         /* half a second with nothing to do: the task ends */
            int bye = 0;
            portENTER_CRITICAL(&_TaskMux);
            if (_P.req == 0) { _P.task = NULL; bye = 1; }
            portEXIT_CRITICAL(&_TaskMux);
            if (bye) vTaskDelete(NULL);
            idle = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(_P.st == kNwPv_Playing ? 250 : 100));
    }
}

/* a request for the task (set under the lock it checks before it ends), and the task started if it is not running */
static void ask_task(int req)
{
    int start;
    portENTER_CRITICAL(&_TaskMux);
    _P.req = req;
    start = _P.task == NULL;
    if (start) _P.task = (TaskHandle_t)1;              /* (taken: no second start) */
    portEXIT_CRITICAL(&_TaskMux);
    if (start) {
        TaskHandle_t h = NULL;
        if (xTaskCreate(rc_task, "rc", 4096, NULL, 4, &h) != pdPASS) { portENTER_CRITICAL(&_TaskMux); _P.task = NULL; portEXIT_CRITICAL(&_TaskMux); }
        else _P.task = h;
    }
}
void CfRc_Init(void)
{
    store_migrate();
    (void)index_load();
    game_load();
    _P.st = _GameOk ? kNwPv_Ready : kNwPv_None;   /* (no task until something is asked of it) */
    /* lane gamecomp1: 1.1.0's kept game moves into the FPGA flash's library when this unit has one */
    uint32_t gb = 0, gs = 0;
    (void)gb; (void)gs;
    (void)lib_ask(_GameOk ? RC_LIB_MIGRATE : RC_LIB_OPEN);   /* (on the task, once the FPGA answers: the open fills the map a download plans with) */
}
int CfRc_Playing(void) { return _P.st == kNwPv_Playing || _P.st == kNwPv_Loading; }
int CfRc_Loading(void) { return _P.st == kNwPv_Loading || _P.req == 1; }
/* fwshrink1: no light sleep while the RAM cart works the FPGA: a load (ESP32 flash / the library -> PSRAM over QSPI and the
   op engine), a library job, a stop or a play asked for. Light sleep (pwrmgr.c: 100 ms with no FPGA UART traffic) pauses the
   FPGA RX / TX tasks and flushes UART1: the QSPI writes of a load bring no UART traffic, so the unit slept in the middle of
   one and the next op's status was lost ("THE FPGA DID NOT ANSWER"; unit 1, a staged 1 MiB play: 6 of 6 with nothing written
   to the console, 0 of 29 when the console was written to: a keystroke keeps the unit awake 10 s). A game that only plays
   still lets the unit sleep (it wakes on the FPGA's UART, as with a cartridge). */
/* lane savefix1: also while a save copy is pending, and while the poll is overdue (a timer wake: see cf_rc_sleep.h) */
int CfRc_KeepAwake(void)
{
    const int64_t age = esp_timer_get_time() - _PollUs;
    return cf_rc_keep_awake(_P.st == kNwPv_Loading, _P.req, _LibOp.busy, _P.pend || _SaveBusy, _P.st == kNwPv_Playing, age < 0 ? 0u : (uint32_t)(age / 1000));
}
/* lane savefix1: a game playing wakes the unit from light sleep this often (pwrmgr.c), so the save poll above keeps running */
uint32_t CfRc_SleepTimerMs(void) { return cf_rc_sleep_timer_ms(_P.st == kNwPv_Playing); }
static int playing(void) { return CfRc_Playing(); }
void CfRc_PlayKept(void) { _InRam = 0; _Src = 0; ask_task(1); }
/* lane sdcard1: a game on the microSD card (its path from the root) */
void CfRc_SdSetPath(const char *path) { if (!CfRc_Playing()) snprintf(_SdPath, sizeof _SdPath, "%s", path); }
void CfRc_PlaySd(const char *path)
{
    /* (a game playing is stopped first, its save copied: the path and source are its until then) */
    if (CfRc_Playing() || _P.req == 2) { printf("rc: sd: a game is playing: stop it first\n"); return; }
    if (path) snprintf(_SdPath, sizeof _SdPath, "%s", path);
    _InRam = 0; _Src = 2; ask_task(1);
}
const char *CfRc_SdPath(void) { return _Src == 2 ? _SdPath : ""; }
int CfRc_SdPlaying(void) { return _Src == 2 && CfRc_Playing(); }
void CfRc_PlayInRam(uint32_t size, uint32_t crc, const char *title)
{
    memset(&_RamGame, 0, sizeof _RamGame);
    _RamGame.size = size; _RamGame.crc = crc;
    snprintf(_RamGame.title, sizeof _RamGame.title, "%s", title ? title : "A GAME");
    _SaveGiven = 0;
    _InRam = 1; _Src = 1; ask_task(1);
}

/* ---- 1.1.0: a game streamed into PSRAM as it downloads (the HTTP task; never the nw task: these wait for the FPGA's
 * status). Begin stops a RAM cart that plays and holds the core; Put takes the image's bytes in order (any lengths:
 * whole KiB go out, each written then read back through the window); End writes the last part KiB. 0 = done. */
int CfRc_RamBegin(void)
{
    if (CfRc_Playing()) {
        CfRc_Stop();
        for (int i = 0; i < 100 && CfRc_Playing(); i++) vTaskDelay(pdMS_TO_TICKS(50));
        if (CfRc_Playing()) return -1;
    }
    if (!_Carry && !(_Carry = heap_caps_malloc(1024, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL))) return -2;
    _CarryAt = 0; _CarryBase = CF_RC_ROM_BASE; _CarryMax = CF_RC_ROM_MAX; _CarryDirty = 0; _CarryAll = 1;
    memset(_Carry, 0xFF, 1024);
    if (CfRc_Reset() != 0 || CfRc_Cfg(0, 0, 0, 0, CF_RC_A_HOLD)) return -3;   /* the core waits while its game changes */
    return 0;
}
static int carry_out(void)
{
    if (!buf_ok()) return -2;
    const uint32_t kib = _CarryAt >> 10;
    if (!_CarryAll && kib >= 16u && (kib & 63u)) {
        if (OSD_QspiPsramWrite(_CarryBase + _CarryAt, _Carry)) return -4;
        _CarryAt += 1024; memset(_Carry, 0xFF, 1024); _CarryDirty = 0;
        return 0;
    }
    for (int tries = 0; tries < 3; tries++) {
        if (OSD_QspiPsramWrite(_CarryBase + _CarryAt, _Carry)) return -4;
        vTaskDelay(1);                                  /* the 1 KiB burst lands */
        int e = CfRc_Op(CF_RC_O_RREAD, _CarryBase + _CarryAt, 1024, 0, 200);
        if (e) return e;
        if (OSD_QspiWinRead(0, _Buf, 1024)) return -5;
        if (!memcmp(_Buf, _Carry, 1024)) { _CarryAt += 1024; memset(_Carry, 0xFF, 1024); _CarryDirty = 0; return 0; }
    }
    printf("rc: PSRAM %06lx: the game's KiB does not read back the same\n", (unsigned long)(_CarryBase + _CarryAt));
    return -6;
}
int CfRc_RamPut(uint32_t off, const uint8_t *b, uint32_t n)
{
    if (!_Carry || off < _CarryAt || off + n > _CarryMax) return -1;
    while (n) {
        const uint32_t at = off - _CarryAt;             /* inside the carry KiB */
        if (at >= 1024) return -1;                      /* (a gap: the bytes come in order) */
        uint32_t k = 1024 - at;
        if (k > n) k = n;
        memcpy(_Carry + at, b, k);
        _CarryDirty = 1;
        off += k; b += k; n -= k;
        if (at + k == 1024) { const int e = carry_out(); if (e) return e; }
    }
    return 0;
}
int CfRc_RamEnd(uint32_t size)
{
    int e = 0;
    if (_Carry && size > _CarryAt) e = carry_out();
    if (_Carry) { heap_caps_free(_Carry); _Carry = NULL; }
    return e;
}
/* lane share1: the writer moves to another PSRAM region (a part KiB written out first): the ROM area, the scratch above it
   (CF_RC_SCRATCH_BASE: a game's container to unpack after), or the save RAM (CF_RC_RAM_BASE: a save that came with a game) */
int CfRc_RamRegion(uint32_t base, uint32_t max)
{
    if (!_Carry) return -1;
    int e = 0;
    if (_CarryDirty) e = carry_out();
    _CarryBase = base; _CarryMax = max; _CarryAt = 0; _CarryDirty = 0;
    _CarryAll = base != CF_RC_SCRATCH_BASE;            /* (a container in scratch: its own crc32s check it at the unpack) */
    memset(_Carry, 0xFF, 1024);
    return e;
}
/* lane share1: bytes [off, off + n) of PSRAM scratch (checked reads) */
static int psram_read_any(uint32_t a, uint8_t *dst, uint32_t n);
int CfRc_ScratchRead(uint32_t off, uint8_t *b, uint32_t n) { return (off + n > CF_RC_SCRATCH_SIZE) ? -1 : psram_read_any(CF_RC_SCRATCH_BASE + off, b, n); }
/* lane share1: the same by timed window reads (no status wait; the caller's SGZ1 crc32s catch a stale window): len = the
   container's length (nothing past it is read ahead) */
int CfRc_ScratchReadFast(uint32_t off, uint8_t *b, uint32_t n, uint32_t len)
{
    if (off + n > CF_RC_SCRATCH_SIZE) return -1;
    return win_timed_lim(CF_RC_O_RREAD, CF_RC_SCRATCH_BASE + off, b, n, _FastUsRread, CF_RC_SCRATCH_BASE + ((len + 1u) & ~1u));
}
void CfRc_ScratchFastDone(void) { ahead_done(); }
/* lane share1: scratch about to be read back (a container to unpack onto the card): the RAM cart off and the core let go first
   (window reads with the core held come a word late now and then); the writer's KiB freed */
int CfRc_ScratchReadBegin(void)
{
    if (_Carry) { heap_caps_free(_Carry); _Carry = NULL; }
    if (CfRc_Playing()) return -1;
    return CfRc_Reset() == 0 && CfRc_Cfg(0, 0, 0, 0, 0) == 0 ? 0 : -1;
}
/* lane share1: PSRAM was written for something that does not play now: the writer's KiB freed, the core let go (the cartridge
   in the slot starts again, as after a RAM game) */
void CfRc_RamRelease(void)
{
    if (_Carry) { heap_caps_free(_Carry); _Carry = NULL; }
    if (CfRc_Playing()) return;
    (void)CfRc_Cfg(0, 0, 0, 0, CF_RC_A_HOLD);
    vTaskDelay(pdMS_TO_TICKS(300));
    (void)CfRc_Cfg(0, 0, 0, 0, 0);
    printf("rc: PSRAM written for a share: back to the cartridge\n");
}
/* lane share1: a game in PSRAM played (not kept): raw in the ROM area (CfRc_PlayInRam), or an SGZ1 container in scratch
   (unpacked into the ROM area first). save_given: the save RAM holds the game's save (it came with it): kept and put in
   this unit's store at the start, not replaced by the store's or a blank one */
void CfRc_PlayScratch(uint32_t len, const char *title, int save_given)
{
    memset(&_RamGame, 0, sizeof _RamGame);
    _RamGame.size = len;
    snprintf(_RamGame.title, sizeof _RamGame.title, "%s", title ? title : "A GAME");
    _SaveGiven = save_given;
    _InRam = 0; _Src = 5; _P.notkept = 2; ask_task(1);
}
void CfRc_PlayInRamSave(uint32_t size, uint32_t crc, const char *title, int save_given)
{
    memset(&_RamGame, 0, sizeof _RamGame);
    _RamGame.size = size; _RamGame.crc = crc;
    snprintf(_RamGame.title, sizeof _RamGame.title, "%s", title ? title : "A GAME");
    _SaveGiven = save_given;
    _P.notkept = 2;
    _InRam = 1; _Src = 1; ask_task(1);
}
void CfRc_Stop(void) { if (_P.task) ask_task(2); }
int CfRc_State(void) { return _P.st; }

void CfRc_View(NwPlayView_t *v)
{
    memset(v, 0, sizeof *v);
    v->Kind = _P.st;
    const int active = _P.st == kNwPv_Playing || _P.st == kNwPv_Loading;
    snprintf(v->Title, sizeof v->Title, "%s", ((_InRam || _Src == 2 || _Src == 3 || _Src == 5) && active) ? _RamGame.title : (_Src == 4 && active) ? _TmpGame.title : _GameOk ? _Game.title : "");
    v->Pct = _P.pct;
    if (_P.st == kNwPv_Result) snprintf(v->Note, sizeof v->Note, "%s|%s", _P.r1, _P.r2);
    else if (_P.st == kNwPv_Playing && _P.notkept) snprintf(v->Note, sizeof v->Note, "%s", _P.notkept == 1 ? "NOT  KEPT:  NO  ROOM" : "NOT  KEPT");
    else if (_P.st == kNwPv_Playing) snprintf(v->Note, sizeof v->Note, "%s", (!_P.sav_size && !_P.rtc) || !_P.battery ? "NO  SAVE" : (_Src == 2 && _Sv.fail) ? "CARD  OUT:  SAVE  KEPT" : (_Src == 2 && _P.saves && !_P.pend) ? "SAVED  ON  THE  CARD" : (_Src == 3 && _Sv.fail) ? "SAVE  KEPT  IN  UNIT" : (_P.pend ? "SAVING" : (_P.saves ? "SAVED" : "SAVE  READY")));
}
void CfRc_Button(int a)
{
    switch (_P.st) {
    case kNwPv_Ready:   if (a) CfRc_PlayKept(); break;
    case kNwPv_Playing: if (a) CfRc_Stop(); break;
    case kNwPv_Result: case kNwPv_NoFpga: ask_task(3); break;
    default: break;
    }
}

/* lane sdcard1: the FPGA still in RAM cart mode with no game of this boot playing (the ESP32 restarted under a
   game): back to the cartridge, so a cart job can reach the slot. 1 = it was on */
int CfRc_SlotBack(void)
{
    if (CfRc_Playing()) return 0;
    const uint8_t s = CF_XS_P_STATUS;
    const uint32_t n0 = _StN;
    xs_send(&s, 1);
    for (int i = 0; i < 50 && _StN == n0; i++) vTaskDelay(pdMS_TO_TICKS(2));
    if (_StN == n0 || !(_St[7] & 0x20)) return 0;
    printf("rc: the RAM cart was still on (from before a restart): back to the cartridge\n");
    (void)CfRc_Cfg(0, 0, 0, 0, CF_RC_A_HOLD);
    vTaskDelay(pdMS_TO_TICKS(300));
    (void)CfRc_Cfg(0, 0, 0, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(200));
    return 1;
}
/* lane sdcard1: a cartridge in the slot (the FPGA's CART_DET, cart_xs status byte 11 bit 0); 0 when no status came */
int CfRc_CartIn(void)
{
    const uint8_t s = CF_XS_P_STATUS;
    const uint32_t n0 = _StN;
    xs_send(&s, 1);
    for (int i = 0; i < 50 && _StN == n0; i++) vTaskDelay(pdMS_TO_TICKS(2));
    return _StN != n0 && (_St[11] & 1);
}
/* ---------------------------------------------------------------- console */
static void show_status(void)
{
    const uint8_t s = CF_XS_P_STATUS;
    const uint32_t n0 = _StN;
    xs_send(&s, 1);
    for (int i = 0; i < 50 && _StN == n0; i++) vTaskDelay(pdMS_TO_TICKS(2));
    if (_StN == n0) { printf("rc: no status (FPGA 18.30 loaded?)\n"); return; }
    const uint8_t rc = _St[7], sl = _St[11];
    static const char *const dens[8] = { "?", "32 Mbit (4 MiB)", "?", "64 Mbit (8 MiB)", "?", "128 Mbit (16 MiB)", "?", "256 Mbit (32 MiB)" };
    printf("rc: psram %s (MR2 density %u), ramcart %s%s%s%s, cart %s, err %u, done seq %02x\n",
           dens[rc & 7], rc & 7, (rc & 0x20) ? "ON" : "off", (rc & 0x40) ? " hold" : "", (rc & 0x10) ? " dirty" : "",
           (rc & 0x08) ? " busy" : "", (sl & 1) ? "in the slot" : "none", _St[3], _St[2]);
}

static uint32_t num(const char *s) { return (uint32_t)strtoul(s, NULL, 0); }
static int fwr_get(void *u, uint32_t off, uint8_t *b, uint32_t n) { return esp_flash_read(NULL, b, (uint32_t)(uintptr_t)u + off, n) == ESP_OK ? 0 : -1; }

/* fwshrink1: the console benches' 1 KiB buffers come from the heap for the command's run (they were static: ~7 KB of
   RAM held for good, which every TLS handshake missed); freed when the command's block ends */
static void rc_buf_free(uint8_t **p) { if (*p) heap_caps_free(*p); }
#define RC_BENCH_BUF(name) uint8_t *name __attribute__((cleanup(rc_buf_free))) = heap_caps_malloc(1024, MALLOC_CAP_DMA)
#define RC_BENCH_NOMEM(what) do { printf("rc: %s e=1 (no memory for its buffers)\n", what); return 0; } while (0)

static int cmd_rc(int argc, char **argv)
{
    if (argc < 2 || !strcmp(argv[1], "st")) { show_status(); return 0; }
    const char *c = argv[1];

    if (!strcmp(c, "ftag") && argc >= 4) {           /* bench: a tagged FREAD (arg bit 0): word 0 = {seq, 00}, the bytes from word 1 */
        const uint32_t a = num(argv[2]), n = num(argv[3]);
        if (!buf_ok() || n > 1022 || (n & 1u)) return 0;
        const int e = CfRc_Op(CF_RC_O_FREAD, a, n, 1, 300);
        const uint8_t seq = _St[2];
        const uint32_t want = CfRc_Crc();
        const int r = OSD_QspiWinRead(0, _Buf, (uint16_t)(n + 2u));
        printf("rc: ftag %06lx %lu e=%d r=%d tag %02x %02x (seq %02x) data crc %08lx want %08lx %s\n", (unsigned long)a, (unsigned long)n, e, r,
               _Buf[0], _Buf[1], seq, (unsigned long)crc32_step(0, _Buf + 2, n), (unsigned long)want,
               (_Buf[0] == 0 && _Buf[1] == seq && crc32_step(0, _Buf + 2, n) == want) ? "OK" : "BAD");
        return 0;
    }
    if (!strcmp(c, "fcopy") && argc >= 5) {          /* bench: rc fcopy FLASH PSRAM LEN [check]: FCOPY, timed; check = read back */
        const uint32_t fa = num(argv[2]), pa = num(argv[3]), len = num(argv[4]);
        uint32_t crc = 0, fc = 0; int ff = 0;
        const TickType_t t0 = xTaskGetTickCount();
        int e = CfRc_FlashCopy(fa, pa, len, &crc);
        const uint32_t ms = (xTaskGetTickCount() - t0) * portTICK_PERIOD_MS;
        int e2 = CfRc_FlashCrc(fa, len, &fc, &ff);
        printf("rc: fcopy %06lx -> psram %06lx %lu e=%d crc %08lx (flash crc %08lx e=%d) %lu ms, %lu KiB/s\n", (unsigned long)fa, (unsigned long)pa,
               (unsigned long)len, e, (unsigned long)crc, (unsigned long)fc, e2, (unsigned long)ms, (unsigned long)(ms ? (uint64_t)len * 1000u / 1024u / ms : 0));
        if (!e && argc >= 6) {                         /* the PSRAM copy read back through the RAM cart's window, crc32 */
            uint8_t *b = malloc(1024); uint32_t pc = 0; int er = b ? 0 : -22;
            const TickType_t t1 = xTaskGetTickCount();
            for (uint32_t o = 0; o < len && !er; o += 1024u) { const uint32_t k = len - o < 1024u ? len - o : 1024u; er = CfRc_PsramRead(pa + o, b, k); if (!er) pc = cf_fota_crc32(pc, b, k); }
            free(b);
            printf("rc: fcopy check: PSRAM reads back crc %08lx e=%d (%s, %lu ms)\n", (unsigned long)pc, er, pc == crc ? "same" : "DIFFERENT",
                   (unsigned long)((xTaskGetTickCount() - t1) * portTICK_PERIOD_MS));
        }
        return 0;
    }
    if (!strcmp(c, "fjlog")) { extern void CfDev_FjLogPrint(int clear); CfDev_FjLogPrint(argc >= 3 && !strcmp(argv[2], "clear")); return 0; }
    if (!strcmp(c, "fop") && argc >= 4) {            /* bench: FREAD ops only (no window read), N times */
        const int times = argc >= 5 ? (int)num(argv[4]) : 1;
        for (int i = 0; i < times; i++) {
            const int e = CfRc_Op(CF_RC_O_FREAD, num(argv[2]), num(argv[3]), 0, 300);
            printf("rc: fop %d e=%d crc %08lx done %02x err %02x\n", i, e, (unsigned long)CfRc_Crc(), _St[2], _St[3]);
            if (e) break;
        }
        return 0;
    }
    if (!strcmp(c, "frdbg")) { _FrDbg = argc >= 3 ? (int)num(argv[2]) : 1; return 0; }
    if (!strcmp(c, "sraw")) {                        /* the last cart_xs status packet, raw (bench) */
        printf("rc: sraw");
        for (int i = 0; i < 32; i++) printf(" %02x", _St[i]);
        printf("\n");
        return 0;
    }
    if (!strcmp(c, "reset")) { printf("rc: reset -> err %u\n", CfRc_Reset()); return 0; }
    if (!strcmp(c, "peek") && argc >= 3) {
        static uint8_t b[16];
        const uint32_t a = num(argv[2]) & ~1u;
        int e = CfRc_PsramRead(a, b, 16);
        printf("rc: peek %06lx e=%d:", (unsigned long)a, e);
        for (int i = 0; i < 16; i++) printf(" %02x", b[i]);
        printf("\n");
        return 0;
    }
    if (!strcmp(c, "keep") && argc >= 5) {
        /* bench: keep the game already in the flash at ADDR (LEN bytes) as this Chromatic's game */
        RC_BENCH_BUF(b);
        if (!b) RC_BENCH_NOMEM("keep");
        const uint32_t a = num(argv[2]), n = num(argv[3]);
        uint32_t crc = 0;
        for (uint32_t o = 0; o < n; o += 1024u) {
            const uint32_t k = n - o < 1024u ? n - o : 1024u;
            if (esp_flash_read(NULL, b, a + o, k) != ESP_OK) break;
            crc = crc32_step(crc, b, k);
        }
        CfRc_Keep(a, n, crc, argv[4]);
        printf("rc: keep %s at %06lx %lu bytes crc %08lx e=0\n", argv[4], (unsigned long)a, (unsigned long)n, (unsigned long)crc);
        if (_P.st == kNwPv_None) _P.st = kNwPv_Ready;
        return 0;
    }
    if (!strcmp(c, "play")) {
        CfRc_PlayKept();
        /* m110: the task takes the request (req back to 0) a moment before the load says LOADING, so the old loop could
           answer "e=1 state 1" (READY) for a play that was starting; wait as sdplay does: up to 3 s for it to leave
           READY / NONE, then while it loads (unit 2, 2026-10-24: the page's Just play it over USB) */
        vTaskDelay(pdMS_TO_TICKS(300));
        for (int i = 0; i < 600 && (_P.req || _P.st == kNwPv_Loading || ((_P.st == kNwPv_Ready || _P.st == kNwPv_None) && i < 30)); i++) vTaskDelay(pdMS_TO_TICKS(100));
        printf("rc: play e=%d state %d %s %s\n", _P.st == kNwPv_Playing ? 0 : 1, _P.st, _P.r1, _P.r2);
        return 0;
    }
    if (!strcmp(c, "sdplay") && argc >= 3) {
        /* lane sdcard1: a game on the card played with no cartridge: rc sdplay PATH [VERIFY 0-3] */
        if (argc >= 4) _SdVerify = (uint8_t)num(argv[3]);
        CfRc_PlaySd(argv[2]);
        vTaskDelay(pdMS_TO_TICKS(300));
        for (int i = 0; i < 1200 && (_P.req || _P.st == kNwPv_Loading || ((_P.st == kNwPv_Ready || _P.st == kNwPv_None) && i < 30)); i++) vTaskDelay(pdMS_TO_TICKS(100));
        printf("rc: sdplay e=%d state %d %s %s: %lu KiB in %lu ms, %lu read back, %lu wrong (verify %u)\n", _P.st == kNwPv_Playing ? 0 : 1, _P.st, _P.r1, _P.r2,
               (unsigned long)_SdLoad.kib, (unsigned long)_SdLoad.ms, (unsigned long)_SdLoad.checked, (unsigned long)_SdLoad.misses, _SdVerify);
        return 0;
    }
    if (!strcmp(c, "wbench")) {
        /* lane sdcard1 bench: N 1 KiB PSRAM writes (the RAM cart's ROM area), timed */
        const uint32_t n = argc >= 3 ? num(argv[2]) : 256;
        uint8_t *b = heap_caps_malloc(1024, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!b) return 0;
        memset(b, 0x5A, 1024);
        const int64_t t0 = esp_timer_get_time();
        int e = 0;
        for (uint32_t i = 0; i < n && !e; i++) e = OSD_QspiPsramWrite(CF_RC_ROM_BASE + 0x300000u + (i & 511u) * 1024u, b);
        const int64_t us = esp_timer_get_time() - t0;
        heap_caps_free(b);
        printf("rc: wbench e=%d %lu writes in %lld us (%lld us each)\n", e, (unsigned long)n, (long long)us, n ? (long long)us / n : 0);
        return 0;
    }
    if (!strcmp(c, "sdverify") && argc >= 3) { _SdVerify = (uint8_t)num(argv[2]); printf("rc: sdverify %u e=0\n", _SdVerify); return 0; }
    if (!strcmp(c, "stop")) {
        CfRc_Stop();
        for (int i = 0; i < 300 && (_P.req || CfRc_Playing()); i++) vTaskDelay(pdMS_TO_TICKS(100));   /* (the save is copied first) */
        vTaskDelay(pdMS_TO_TICKS(500));
        printf("rc: stop e=0 state %d (saves %lu)\n", _P.st, (unsigned long)_P.saves);
        return 0;
    }
    if (!strcmp(c, "page")) {
        /* bench: SYSTEM > PLAY from the console: rc page [show|a|b], then what it shows */
        if (argc >= 3 && !strcmp(argv[2], "show")) NW_PlayOpenMenu();
        if (argc >= 3 && (!strcmp(argv[2], "a") || !strcmp(argv[2], "b"))) CfRc_Button(!strcmp(argv[2], "a"));
        vTaskDelay(pdMS_TO_TICKS(300));
        NwPlayView_t v;
        CfRc_View(&v);
        printf("rc: page e=0 kind %d \"%s\" \"%s\" pct %d\n", v.Kind, v.Title, v.Note, v.Pct);
        return 0;
    }
    if (!strcmp(c, "saves")) {
        (void)index_load();
        printf("rc: store at %06lx seq %lu\n", (unsigned long)store_base(), (unsigned long)_Ix.seq);
        for (int i = 0; i < RC_ENTRIES; i++) if (_Ix.e[i].used)
            printf("rc:  game %08lx save %lu bytes at sector %u crc %08lx\n", (unsigned long)_Ix.e[i].crc, (unsigned long)_Ix.e[i].size, _Ix.e[i].sect, (unsigned long)_Ix.e[i].dcrc);
        printf("rc: saves e=0 (playing %d, pending %d, copies %lu)\n", _P.st == kNwPv_Playing, _P.pend, (unsigned long)_P.saves);
        return 0;
    }
    if (!strcmp(c, "epeek") && argc >= 3) {
        static uint8_t b[16];
        const uint32_t a = num(argv[2]);
        int e = esp_flash_read(NULL, b, a, 16) == ESP_OK ? 0 : 1;
        printf("rc: epeek %06lx e=%d:", (unsigned long)a, e);
        for (int i = 0; i < 16; i++) printf(" %02x", b[i]);
        printf("\n");
        return 0;
    }
    if (!strcmp(c, "dbg")) {
        for (int k = 0; k < (argc >= 3 ? (int)num(argv[2]) : 1); k++) {
            int e = CfRc_Op(35, 0, 0, 0, 200);
            const uint32_t r = CfRc_Crc();
            const uint32_t fl = u24(_St + 16);
            printf("rc: dbg e=%d a=%04lx fills=%lu flags=%04lx [rd wr want hit tagok stall busy inval | ws %lu mbc %lu]\n", e,
                   (unsigned long)(r & 0xFFFF), (unsigned long)(r >> 16), (unsigned long)fl,
                   (unsigned long)((fl >> 4) & 7), (unsigned long)((fl >> 1) & 7));
            if (argc >= 3) vTaskDelay(pdMS_TO_TICKS(100));
        }
        return 0;
    }
    if (!strcmp(c, "dummy") && argc >= 3) {
        extern uint8_t OSD_QspiWinDummy;
        OSD_QspiWinDummy = (uint8_t)num(argv[2]);
        printf("rc: dummy %u e=0\n", OSD_QspiWinDummy);
        return 0;
    }
    if (!strcmp(c, "wt") && argc >= 3) {
        RC_BENCH_BUF(pat);
        RC_BENCH_BUF(back);
        if (!pat || !back) RC_BENCH_NOMEM("wt");
        const uint32_t a = num(argv[2]) & ~1023u;
        for (int i = 0; i < 1024; i++) pat[i] = (uint8_t)(i ^ (i >> 8) ^ 0x5A);
        int e = OSD_QspiPsramWrite(a, pat);
        vTaskDelay(2);
        static const uint16_t lens[4] = { 1024, 16, 16, 2 };
        static const uint16_t offs[4] = { 0, 0, 512, 0 };
        for (int t = 0; t < 4 && !e; t++) {
            memset(back, 0xEE, 1024);
            e = CfRc_PsramRead(a + offs[t], back, lens[t]);
            int bad = 0, first = -1;
            for (int i = 0; i < lens[t]; i++) if (back[i] != pat[offs[t] + i]) { bad++; if (first < 0) first = i; }
            printf("rc: wt %06lx read %u at +%u e=%d bad %d first %d: %02x %02x %02x %02x (want %02x %02x %02x %02x)\n",
                   (unsigned long)a, lens[t], offs[t], e, bad, first, back[0], back[1], back[2], back[3],
                   pat[offs[t]], pat[offs[t] + 1], pat[offs[t] + 2], pat[offs[t] + 3]);
        }
        return 0;
    }
    if (!strcmp(c, "fscan")) {
        cf_fota_layout_t L;
        const int e = cf_fota_scan(CfRc_FotaHost(), &L);
        uint32_t gb = 0, gs = 0;
        const int ge = cf_fota_game_region(&L, &gb, &gs);
        printf("rc: fscan e=%d kind %d (%s) live at %03lx full at %06lx next %03lx used %d free %d game area %06lx+%lu%s\n", e, L.kind,
               L.kind == CF_FL_RF ? "rf" : L.kind == CF_FL_RF_FULL ? "rf, full" : L.kind == CF_FL_RF_REC ? "rf, recovery boots" : "legacy",
               (unsigned long)L.live_at, (unsigned long)L.full_base, (unsigned long)L.next_at, L.used, L.free, (unsigned long)gb,
               (unsigned long)gs, ge ? " (none)" : "");
        return 0;
    }
    if (!strcmp(c, "finst") && argc >= 5) {          /* the FPGA update from this ESP32's flash (bench) */
        if (CfDev_FpgaSlot() < 0 && !CfDev_FpgaRecovery()) { printf("rc: finst: booted slot unknown\n"); return 0; }
        const uint32_t src = num(argv[2]), len = num(argv[3]), crc = num(argv[4]);
        const TickType_t t0 = xTaskGetTickCount();
        uint8_t *b4 = malloc(4096);
        /* A resume needs the original slot; the currently live header can belong to the other design. */
        const int resume = argc >= 6 && !strcmp(argv[5], "resume");
        const int slot = argc >= 7 && !strcmp(argv[6], "gb") ? CF_FOTA_SLOT_GB :
                         argc >= 7 && !strcmp(argv[6], "gg") ? CF_FOTA_SLOT_GG : -1;
        const int e = resume && slot < 0 ? CF_FOTA_E_LAYOUT :
                      b4 ? cf_fota_install(CfRc_FotaHost(), fwr_get, (void *)(uintptr_t)src, len, crc, resume ? slot : -1, NULL, NULL, b4) : -22;
        CfRc_GameAreaRescan();
        free(b4);
        cf_fota_layout_t L;
        (void)cf_fota_scan(CfRc_FotaHost(), &L);
        printf("rc: finst %lu bytes from %06lx crc %08lx e=%d (%lu ms) kind %d live at %03lx free %d\n", (unsigned long)len,
               (unsigned long)src, (unsigned long)crc, e, (unsigned long)((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS), L.kind,
               (unsigned long)L.live_at, L.free);
        return 0;
    }
    if (!strcmp(c, "finv")) {                        /* bench: all full headers invalidated (the recovery design boots next) */
        if (CfDev_FpgaSlot() < 0 && !CfDev_FpgaRecovery()) { printf("rc: finv: booted slot unknown\n"); return 0; }
        cf_fota_layout_t L;
        const int e = cf_fota_invalidate(CfRc_FotaHost(), &L);
        printf("rc: finv e=%d kind %d\n", e, L.kind);
        return 0;
    }
    if (!strcmp(c, "fread") && argc >= 4) {
        const uint32_t a = num(argv[2]), len = num(argv[3]);
        uint8_t *b = malloc(1024);
        uint32_t crc = 0; int e = b ? 0 : -22; uint8_t first[8] = { 0 };
        const TickType_t t0 = xTaskGetTickCount();
        for (uint32_t o = 0; o < len && !e; o += 1024u) {
            const uint32_t k = len - o < 1024u ? len - o : 1024u;
            e = CfRc_FlashRead(a + o, b, k);
            if (!e) { crc = cf_fota_crc32(crc, b, k); if (!o) memcpy(first, b, 8); }
        }
        free(b);
        const uint32_t ms = (xTaskGetTickCount() - t0) * portTICK_PERIOD_MS;
        printf("rc: fread %06lx %lu e=%d crc %08lx first %02x%02x%02x%02x%02x%02x%02x%02x (%lu ms, %lu KiB/s)\n", (unsigned long)a,
               (unsigned long)len, e, (unsigned long)crc, first[0], first[1], first[2], first[3], first[4], first[5], first[6], first[7],
               (unsigned long)ms, (unsigned long)(ms ? (uint64_t)len * 1000u / 1024u / ms : 0));
        return 0;
    }
    if (!strcmp(c, "ga") && argc >= 5) {             /* the game area ops: ga erase|prog|read|crc ADDR LEN [SEED] */
        const char *w = argv[2];
        const uint32_t a = num(argv[3]), len = num(argv[4]), seed = argc >= 6 ? num(argv[5]) : 1;
        uint8_t *b = malloc(4096);
        int e = b ? 0 : -22; uint32_t crc = 0; int ff = 0;
        const TickType_t t0 = xTaskGetTickCount();
        uint32_t x = seed * 2654435761u + 1u;
        if (!e && !strcmp(w, "erase")) e = CfRc_GameAreaErase(a, len);
        else if (!e && !strcmp(w, "prog")) {
            e = CfRc_RingBegin();
            for (uint32_t o = 0; o < len && !e; o += 4096u) {
                const uint32_t k = len - o < 4096u ? len - o : 4096u;
                for (uint32_t i = 0; i < k; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; b[i] = (uint8_t)x; }
                crc = cf_fota_crc32(crc, b, k);
                if (!ga_in(a + o, k)) e = -10; else e = CfRc_FlashProg(a + o, b, k);
            }
        } else if (!e && !strcmp(w, "read")) {
            for (uint32_t o = 0; o < len && !e; o += 4096u) {
                const uint32_t k = len - o < 4096u ? len - o : 4096u;
                e = CfRc_GameAreaRead(a + o, b, k);
                if (!e) crc = cf_fota_crc32(crc, b, k);
            }
        } else if (!e && !strcmp(w, "crc")) e = CfRc_GameAreaCrc(a, len, &crc, &ff);
        else e = -23;
        free(b);
        const uint32_t ms = (xTaskGetTickCount() - t0) * portTICK_PERIOD_MS;
        printf("rc: ga %s %06lx %lu e=%d crc %08lx ff %d (%lu ms, %lu KiB/s)\n", w, (unsigned long)a, (unsigned long)len, e,
               (unsigned long)crc, ff, (unsigned long)ms, (unsigned long)(ms ? (uint64_t)len * 1000u / 1024u / ms : 0));
        return 0;
    }
    if (!strcmp(c, "fid")) {
        int e = CfRc_Op(CF_RC_O_FID, 0, 0, 0, 200);
        printf("rc: fid e=%d jedec %06lx\n", e, (unsigned long)(CfRc_Crc() & 0xFFFFFFu));
        return 0;
    }
    if (!strcmp(c, "fcrc") && argc >= 4) {
        const uint32_t a = num(argv[2]), n = num(argv[3]);
        const TickType_t t0 = xTaskGetTickCount();
        int e = CfRc_Op(CF_RC_O_FCRC, a, n, 0, 20000);
        printf("rc: fcrc %06lx+%lu e=%d crc %08lx%s (%lu ms)\n", (unsigned long)a, (unsigned long)n, e,
               (unsigned long)CfRc_Crc(), CfRc_AllFF() ? " all FF" : "", (unsigned long)((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS));
        return 0;
    }
    if (!strcmp(c, "probe")) {
        RC_BENCH_BUF(pat);
        RC_BENCH_BUF(back);
        if (!pat || !back) RC_BENCH_NOMEM("probe");
        for (int i = 0; i < 1024; i++) pat[i] = (uint8_t)(i * 7 + 3);
        if (OSD_QspiPsramWrite(0x100000, pat)) { printf("rc: psram write failed\n"); return 0; }
        vTaskDelay(2);
        int e = CfRc_PsramRead(0x100000, back, 1024);
        int same = !e && !memcmp(pat, back, 1024);
        int swapped = 1;
        for (int i = 0; i < 1024 && !e; i += 2) if (back[i] != pat[i + 1] || back[i + 1] != pat[i]) { swapped = 0; break; }
        printf("rc: probe e=%d %s (first %02x %02x %02x %02x)\n", e, same ? "read back the same" : (swapped ? "BYTES SWAPPED" : "DIFFERENT"),
               back[0], back[1], back[2], back[3]);
        /* size: 0x300000 vs 0x700000 (an 8 MiB part has both; a 4 MiB part folds them) */
        for (int i = 0; i < 1024; i++) pat[i] = 0xA5;
        OSD_QspiPsramWrite(0x300000, pat);
        for (int i = 0; i < 1024; i++) pat[i] = 0x5A;
        OSD_QspiPsramWrite(0x700000, pat);
        vTaskDelay(2);
        e = CfRc_PsramRead(0x300000, back, 4);
        printf("rc: 0x300000 after writing 0x700000: %02x (%s)\n", back[0], back[0] == 0xA5 ? "8 MiB addressable" : "folds: 4 MiB");
        show_status();
        return 0;
    }
    if (!strcmp(c, "load") && argc >= 4) {
        const uint32_t src = num(argv[2]), len = num(argv[3]);
        uint32_t crc = 0;
        const TickType_t t0 = xTaskGetTickCount();
        CfRc_Cfg(0, 0, 0, 0, CF_RC_A_HOLD);                 /* the core waits while its game changes */
        int e = CfRc_LoadFromFlash(src, CF_RC_ROM_BASE, len, &crc);
        printf("rc: load %lu bytes from flash %06lx e=%d crc %08lx (%lu ms)\n", (unsigned long)len, (unsigned long)src, e,
               (unsigned long)crc, (unsigned long)((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS));
        return 0;
    }
    if (!strcmp(c, "ram") && argc >= 3) {
        RC_BENCH_BUF(ff);
        if (!ff) RC_BENCH_NOMEM("ram");
        const uint32_t len = num(argv[argc - 1]);
        int e = 0;
        if (argc >= 4) e = CfRc_LoadFromFlash(num(argv[2]), CF_RC_RAM_BASE, len, NULL);
        else {
            memset(ff, 0xFF, 1024);
            for (uint32_t o = 0; o < len && !e; o += 1024) e = OSD_QspiPsramWrite(CF_RC_RAM_BASE + o, ff);
        }
        printf("rc: save RAM %lu bytes e=%d\n", (unsigned long)len, e);
        return 0;
    }
    if (!strcmp(c, "on") && argc >= 5) {
        const int rtc = argc >= 6 && !strcmp(argv[5], "rtc");
        int e = CfRc_Cfg((uint8_t)num(argv[2]), (uint16_t)num(argv[3]), (uint8_t)num(argv[4]), rtc,
                         CF_RC_A_ON | CF_RC_A_HOLD | CF_RC_A_INVAL | CF_RC_A_CLEAN);
        vTaskDelay(pdMS_TO_TICKS(50));
        if (!e) e = CfRc_Cfg((uint8_t)num(argv[2]), (uint16_t)num(argv[3]), (uint8_t)num(argv[4]), rtc, CF_RC_A_ON);
        printf("rc: on e=%d\n", e);
        show_status();
        return 0;
    }
    if (!strcmp(c, "off")) {
        int e = CfRc_Cfg(0, 0, 0, 0, CF_RC_A_HOLD);
        vTaskDelay(pdMS_TO_TICKS(300));                     /* the slot powers up (~70 ms) */
        if (!e) e = CfRc_Cfg(0, 0, 0, 0, 0);
        printf("rc: off e=%d\n", e);
        return 0;
    }
    if (!strcmp(c, "save") && argc >= 3) {
        RC_BENCH_BUF(b);
        if (!b) RC_BENCH_NOMEM("save");
        uint8_t first[8] = { 0 };
        const uint32_t len = num(argv[2]);
        uint32_t crc = 0;
        int e = 0;
        for (uint32_t o = 0; o < len && !e; o += 1024) {
            e = CfRc_PsramRead(CF_RC_RAM_BASE + o, b, 1024);
            if (!e) crc = crc32_step(crc, b, 1024);
            if (o == 0 && !e) memcpy(first, b, 8);
        }
        printf("rc: save RAM %lu bytes e=%d crc %08lx starts %02x %02x %02x %02x %02x %02x %02x %02x\n", (unsigned long)len, e, (unsigned long)crc,
               first[0], first[1], first[2], first[3], first[4], first[5], first[6], first[7]);
        return 0;
    }
    if (!strcmp(c, "rtc")) {
        if (argc >= 6) {
            const uint32_t s = num(argv[2]), m = num(argv[3]), h = num(argv[4]), d = num(argv[5]);
            const uint32_t f = s | m << 8 | h << 16;
            const uint32_t n = (d & 0xFFu) | ((d >> 8) & 1u) << 8;
            printf("rc: rtc set e=%d\n", CfRc_Op(CF_RC_O_RRTC, f, n, 1, 200));
        }
        int e = CfRc_Op(CF_RC_O_RRTC, 0, 0, 0, 200);
        const uint32_t r = CfRc_Crc();
        printf("rc: rtc e=%d %lu days %02lu:%02lu:%02lu (DH %02x)\n", e, (unsigned long)(((r >> 24) & 0xFF) | ((uint32_t)_St[16] & 1u) << 8),
               (unsigned long)((r >> 16) & 0x1F), (unsigned long)((r >> 8) & 0x3F), (unsigned long)(r & 0x3F), _St[16]);
        return 0;
    }
    if (!strcmp(c, "time")) {                          /* 1.1.7 bench: rc time [block|unblock] */
        if (argc >= 4 && !strcmp(argv[2], "set")) printf("rc: time by hand: %s\n", CwlTime_SetByHand(strtoull(argv[3], NULL, 10)) ? "set" : "refused");   /* (bench: as the clock page) */
        else if (argc >= 3) { const int b = !strcmp(argv[2], "block"); CwlTime_Block(b); }
        CwlTime_Print();
        printf("rc: game clocks use: %llu\n", (unsigned long long)Sd_UnixTime());
        return 0;
    }
    printf("rc: st | reset | fid | fcrc A N | probe | load FLASH LEN | ram [FLASH] LEN | on MBC ROMBANKS RAMBANKS [rtc] | off | save LEN | rtc [S M H D] | time [block|unblock|set UNIX]\n");
    return 0;
}

/* lane gamecomp1: `lib` - the game library on the FPGA flash (bench + the cart page over USB)
 *   lib st                          lib: st e=0 area 1 games N free F total T z1   (e=1: no library on this unit)
 *   lib ls                          lib: game UID "TITLE" rom R stored S sgz|raw save V   (one line each), then lib: ls e=0 N
 *   lib imp ADDR LEN keep|play NAME a game staged in this ESP32's flash (SGZ1 or the ROM) into the library (play: then plays it)
 *   lib del UID | lib play UID      lib: del e=0 / lib: play e=0
 *   lib fmt yes                     bench: an empty library */
/* the USB stream's state across the restarts (NULL: forgotten) */
static int libtx_save(const cf_lib_tx_t *t)
{
    nvs_handle_t h;
    if (nvs_open("rc", NVS_READWRITE, &h) != ESP_OK) return -1;
    const esp_err_t e = t ? nvs_set_blob(h, "libtx", t, sizeof *t) : nvs_erase_key(h, "libtx");
    (void)nvs_commit(h);
    nvs_close(h);
    return (e == ESP_OK || (!t && e == ESP_ERR_NVS_NOT_FOUND)) ? 0 : -1;
}
static int libtx_load(cf_lib_tx_t *t)
{
    nvs_handle_t h;
    size_t n = sizeof *t;
    if (nvs_open("rc", NVS_READONLY, &h) != ESP_OK) return -1;
    const esp_err_t e = nvs_get_blob(h, "libtx", t, &n);
    nvs_close(h);
    return (e == ESP_OK && n == sizeof *t) ? 0 : -1;
}
static int lib_wait(int secs) { for (int i = 0; i < secs * 10 && _LibOp.busy; i++) vTaskDelay(pdMS_TO_TICKS(100)); return _LibOp.busy ? -99 : _LibOp.res; }
static volatile int _FcOsd;
static void fc_osd_task(void *arg) { (void)arg; int p = 0; while (_FcOsd) { _P.st = kNwPv_Loading; _P.pct = p; p = (p + 7) % 100; vTaskDelay(pdMS_TO_TICKS(20)); } _P.st = kNwPv_None; _P.pct = 0; _FcOsd = -1; vTaskDelete(NULL); }
static int kp_playing(void) { return CfRc_Playing(); }
static int kp_loading(void) { return _P.st == kNwPv_Loading; }
static int kp_stop_pending(void) { return _P.req == 2; }
static void kp_stop(void) { CfRc_Stop(); }
static int kp_lib_busy(void) { return _LibOp.busy; }
static void kp_delay_ms(int ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }
static void kp_say(const char *m) { printf("%s\n", m); }
#include "cf_rc_keep.inc"
static int cmd_lib(int argc, char **argv)
{
    const char *c = argc >= 2 ? argv[1] : "st";
    if (!strcmp(c, "st") || !strcmp(c, "ls")) {
        const int held = _LibHold;
        CfRc_LibHold(1);
        (void)lib_wait(30);
        uint32_t fr = 0, to = 0;
        const int n = CfRc_LibCount(&fr, &to, NULL);
        if (!strcmp(c, "ls")) for (int i = 0; i < n; i++) {
            cf_lib_view_ent_t v;
            if (CfRc_LibGet(i, &v)) break;
            printf("lib: game %lu \"%s\" rom %lu stored %lu %s save %lu\n", (unsigned long)v.uid, v.title, (unsigned long)v.rom_size, (unsigned long)v.stored_size,
                   (v.flags & CF_LIB_F_SGZ) ? "sgz" : "raw", (unsigned long)v.save_size);
        }
        if (!strcmp(c, "ls")) printf("lib: ls e=%d %d\n", n < 0 ? 1 : 0, n < 0 ? 0 : n);
        else printf("lib: st e=%d area %d games %d free %lu total %lu z1%s\n", n < 0 ? 1 : 0, n == -1 ? 0 : 1, n < 0 ? 0 : n, (unsigned long)fr, (unsigned long)to,
                    CfRc_Playing() ? " playing 1" : "");   /* lane m110x: a game plays from memory (a keep stops it first) */
        CfRc_LibHold(held);
        return 0;
    }
    /* a container bigger than one staging part, over USB in parts (the ESP32 restarts between them: the stream's state is
       kept in NVS "rc"/"libtx"): begin LEN NAME, then part ADDR LEN OFF for each staged part, then end CRC32 (of all LEN) */
    cf_lib_tx_t *const tp = (!strcmp(c, "imp") && argc >= 4) ? malloc(sizeof *tp) : NULL;   /* (heap: static RAM is the TLS gate's) */
#define t (*tp)
    if (tp && argc >= 5 && !strcmp(argv[2], "begin")) {
        int e = kp_keep_ready();                         /* lane m110x: a game playing from memory stops first */
        if (!e) { CfRc_LibHold(1); (void)lib_wait(30); CfRc_LibHold(0); }
        if (!e) e = CfRc_LibPlanStream(num(argv[3]), argv[4], &t);
        if (!e) e = libtx_save(&t) ? CF_LIB_E_FLASH : 0;
        printf("lib: imp begin e=%d\n", e);
        free(tp);
        return 0;
    }
    if (tp && argc >= 6 && !strcmp(argv[2], "part")) {
        const uint32_t a = num(argv[3]), n = num(argv[4]), off = num(argv[5]);
        int e = libtx_load(&t) ? CF_LIB_E_NONE : (t.have != off ? CF_LIB_E_ARG : 0);
        if (!e) e = kp_keep_ready();                     /* lane m110x (was -98 while a game played) */
        if (!e && !_LibMapOk) { CfRc_LibHold(1); (void)lib_wait(30); CfRc_LibHold(0); }   /* (the area's place: known once it was looked at) */
        uint8_t *b = e ? NULL : malloc(2048);
        if (!e && !b) e = CF_LIB_E_FLASH;
        const int64_t t0 = esp_timer_get_time();
        for (uint32_t o = 0; !e && o < n; o += 2048u) {
            const uint32_t k = n - o < 2048u ? n - o : 2048u;
            e = esp_flash_read(NULL, b, a + o, k) != ESP_OK ? CF_LIB_E_FLASH : CfRc_LibTxWrite(&t, b, k);
        }
        free(b);
        if (!e) e = libtx_save(&t) ? CF_LIB_E_FLASH : 0;
        printf("lib: imp part e=%d %lu bytes at %lu (%lld ms)\n", e, (unsigned long)n, (unsigned long)off, (long long)((esp_timer_get_time() - t0) / 1000));
        free(tp);
        return 0;
    }
    if (tp && !strcmp(argv[2], "end")) {
        int e = libtx_load(&t) ? CF_LIB_E_NONE : (t.crc != (uint32_t)strtoul(argv[3], NULL, 16) || t.have != t.e.stored_size ? CF_LIB_E_VERIFY : 0);
        if (!e) e = kp_keep_ready();
        if (!e) e = CfRc_LibStreamEnd(&t) ? -98 : lib_wait(120);
        libtx_save(NULL);
        printf("lib: imp end e=%d uid %lu\n", e, (unsigned long)(e ? 0 : CfRc_LibUid()));
        free(tp);
        return 0;
    }
#undef t
    free(tp);
    if (!strcmp(c, "imp") && argc >= 6) {
        const uint32_t a = num(argv[2]), n = num(argv[3]);
        const int r = kp_keep_ready();                   /* lane m110x: a game playing from memory stops first (was -98) */
        const int e0 = r ? r : CfRc_LibKeepStaged(a, n, 0, argv[5], !strcmp(argv[4], "play")) ? -98 : 0;
        const int e = e0 ? e0 : lib_wait(300);
        printf("lib: imp e=%d uid %lu\n", e, (unsigned long)(e ? 0 : _LibOp.uid));
        return 0;
    }
    if (!strcmp(c, "del") && argc >= 3) {
        const int e0 = CfRc_LibDelete(num(argv[2]));
        printf("lib: del e=%d\n", e0 ? -98 : lib_wait(60));
        return 0;
    }
    if (!strcmp(c, "play") && argc >= 3) {
        const int64_t t0 = esp_timer_get_time();
        if (_P.st == kNwPv_Result || _P.st == kNwPv_NoFpga) _P.st = _GameOk ? kNwPv_Ready : kNwPv_None;   /* (an old result is not this play's) */
        CfRc_LibPlay(num(argv[2]));
        vTaskDelay(pdMS_TO_TICKS(300));                  /* (the task takes the request) */
        for (int i = 0; i < 1800 && (_P.req || _P.st == kNwPv_Loading || _P.st == kNwPv_Ready || _P.st == kNwPv_None); i++) vTaskDelay(pdMS_TO_TICKS(100));
        printf("lib: play took %lld ms\n", (long long)((esp_timer_get_time() - t0) / 1000));
        printf("lib: play e=%d state %d %s %s\n", _P.st == kNwPv_Playing ? 0 : 1, _P.st, _P.r1, _P.r2);
        return 0;
    }
    if (!strcmp(c, "fmt") && argc >= 3 && !strcmp(argv[2], "yes")) {
        if (CfRc_Playing() || _LibOp.busy) { printf("lib: fmt e=1 (busy)\n"); return 0; }
        _LibOp.busy = 1; _LibOp.op = RC_LIB_FMT; _LibOp.res = 0;
        ask_task(_P.req ? _P.req : 4);
        printf("lib: fmt e=%d\n", lib_wait(60));
        return 0;
    }
    if (!strcmp(c, "peek") && argc >= 5) {
        /* bench: lib peek UID OFF LEN: the game's container bytes OFF..+LEN (copied to scratch first) three ways: one checked read
           (psram_read_any, as the library's reads), checked reads split at the PSRAM's 1 KiB rows (CfRc_PsramRead), the flash */
        const uint32_t uid = num(argv[2]), off = num(argv[3]), len = num(argv[4]);
        if (len == 0 || len > 1000) { printf("lib: peek e=1\n"); return 0; }
        const int held = _LibHold;
        CfRc_LibHold(1); (void)lib_wait(30);
        const int i = _L ? cf_lib_find(_L, uid) : -1;
        if (i < 0) { CfRc_LibHold(held); printf("lib: peek e=1 (no game)\n"); return 0; }
        const cf_lib_ent_t e = _L->ix.e[i];
        _LoadEnt = e;
        uint8_t *x = malloc(len), *y = malloc(len), *z = malloc(len);
        (void)CfRc_Reset(); (void)CfRc_Cfg(0, 0, 0, 0, 0);
        const int ce = lib_to_scratch(&e);
        if (argc >= 6 && !strcmp(argv[5], "hold")) { (void)CfRc_Reset(); (void)CfRc_Cfg(5, 128, 4, 0, CF_RC_A_HOLD); }
        const int ra = x ? psram_read_any(RC_SCRATCH + off, x, len) : -1, rb = y ? CfRc_PsramRead(RC_SCRATCH + off, y, len) : -1, rc = z ? ent_rd(off, z, len, 0) : -1;
        uint32_t da = 0, db = 0; long fa = -1, fb = -1;
        for (uint32_t j = 0; x && y && z && j < len; j++) { if (x[j] != z[j]) { da++; if (fa < 0) fa = (long)j; } if (y[j] != z[j]) { db++; if (fb < 0) fb = (long)j; } }
        printf("lib: peek +%lu %lu (row offset %lu): copy e=%d; one read e=%d %lu wrong (first +%ld); row-split e=%d %lu wrong (first +%ld); flash e=%d\n",
               (unsigned long)off, (unsigned long)len, (unsigned long)((RC_SCRATCH + off) & 1023u), ce, ra, (unsigned long)da, fa, rb, (unsigned long)db, fb, rc);
        if (argc >= 6 && !strcmp(argv[5], "hold")) { (void)CfRc_Reset(); (void)CfRc_Cfg(0, 0, 0, 0, 0); }
        free(x); free(y); free(z);
        CfRc_LibHold(held);
        return 0;
    }
    if (!strcmp(c, "savetest") && argc >= 3) {
        /* bench (gamecomp1, the library proofs): lib savetest SEED: while a library game plays, its save RAM (PSRAM) filled with a
           pattern from SEED (the ESP32's QSPI writes, as if the game had saved) and its copy into the library asked for now (the
           path a game's own save takes, RC_QUIET_MS after its last write); prints the pattern's crc32 to compare after a restart */
        if (!(_Src == 3 && CfRc_Playing()) || !_P.sav_size) { printf("lib: savetest e=1 (no library game with a save plays)\n"); return 0; }
        uint8_t *w = heap_caps_malloc(1024, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!w) { printf("lib: savetest e=1 (memory)\n"); return 0; }
        const uint32_t seed = num(argv[2]), len = (_P.sav_size + 1023u) & ~1023u;
        uint32_t crc = 0, x = seed * 2654435761u + 1u;
        int e = 0;
        for (uint32_t o = 0; o < len && !e; o += 1024u) {
            for (int i = 0; i < 1024; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; w[i] = (uint8_t)x; }
            e = OSD_QspiPsramWrite(CF_RC_RAM_BASE + o, w);
            crc = crc32_step(crc, w, 1024);
        }
        heap_caps_free(w);
        const int nocopy = argc >= 4 && !strcmp(argv[3], "nocopy");   /* (lib savetest SEED nocopy: the save RAM overwritten, no copy) */
        if (!nocopy) { const uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS; _P.pend = 1; _P.pend_first = now; _P.pend_at = now - RC_QUIET_MS; }
        printf("lib: savetest seed %lu: %lu bytes written e=%d crc %08lx; %s\n", (unsigned long)seed, (unsigned long)len, e, (unsigned long)crc,
               nocopy ? "no copy asked for" : "the copy asked for");
        return 0;
    }
    if (!strcmp(c, "fcheck") && argc >= 3) {
        /* bench (gamecomp1, r1831e): lib fcheck UID [N]: the game's container FCOPY'd into PSRAM scratch as a play does (lib_copy), then
           all of it compared with the library's flash (checked RREADs vs checked FREADs): the ranges that differ, and whether the
           scratch there holds the flash's bytes from 2 before ("+2": an extra word in the copy) or something else. N times */
        if (CfRc_Playing()) { printf("lib: fcheck e=1 (a game plays)\n"); return 0; }
        const uint32_t uid = num(argv[2]), times = argc >= 4 ? num(argv[3]) : 1;
        const int held = _LibHold;
        CfRc_LibHold(1);
        (void)lib_wait(30);
        const int i = _L ? cf_lib_find(_L, uid) : -1;
        if (i < 0) { CfRc_LibHold(held); printf("lib: fcheck e=1 (no game %lu)\n", (unsigned long)uid); return 0; }
        const cf_lib_ent_t e = _L->ix.e[i];
        _LoadEnt = e;
        const int osd = argc >= 5 && !strcmp(argv[4], "osd"), hold = argc >= 5 && (!strcmp(argv[4], "hold") || !strcmp(argv[4], "cfg"));
        /* lib fcheck UID N cfg MBC ROM_BANKS RAM_BANKS FLAGS: that RCFG after the copy (hold = cfg 5 128 4 2, as an MBC5 play) */
        const int cmbc = argc >= 9 && !strcmp(argv[4], "cfg") ? (int)num(argv[5]) : 5, crom = argc >= 9 && !strcmp(argv[4], "cfg") ? (int)num(argv[6]) : 128,
                  cram = argc >= 9 && !strcmp(argv[4], "cfg") ? (int)num(argv[7]) : 4, cfl = argc >= 9 && !strcmp(argv[4], "cfg") ? (int)num(argv[8]) : CF_RC_A_HOLD;   /* lib fcheck UID N osd: the LOADING page redrawn every 20 ms meanwhile */
        if (osd) { _FcOsd = 1; if (xTaskCreate(fc_osd_task, "fc_osd", 2048, NULL, 3, NULL) != pdPASS) _FcOsd = 0; }
        uint8_t *pa = malloc(258), *pb = malloc(258);
        for (uint32_t t = 0; pa && pb && t < times; t++) {
            (void)CfRc_Reset(); (void)CfRc_Cfg(0, 0, 0, 0, 0);
            const int ce = lib_to_scratch(&e);
            if (hold) { (void)CfRc_Reset(); (void)CfRc_Cfg((uint8_t)cmbc, (uint16_t)crom, (uint8_t)cram, 0, (uint8_t)cfl); }   /* (as a play: the core held, an MBC5 game set up) */
            uint32_t nd = 0, np2 = 0, nrange = 0; int cls = 0; uint32_t rs = 0;
            uint8_t prev[2] = { 0, 0 };
            int re = 0;
            for (uint32_t o = 0; o < e.stored_size && !re; o += 256u) {
                const uint32_t k = e.stored_size - o < 256u ? e.stored_size - o : 256u;
                re = lib_rd_scratch(NULL, o, pa, k) ? 1 : ent_rd(o, pb + 2, k, 0) ? 2 : 0;
                pb[0] = prev[0]; pb[1] = prev[1];
                for (uint32_t j = 0; !re && j < k; j++) {
                    const uint32_t x = o + j;
                    const int c2 = pa[j] == pb[j + 2] ? 0 : (x >= 2 && pa[j] == pb[j]) ? 1 : 2;   /* 0 same, 1 = the flash's byte 2 before, 2 other */
                    if (c2) { nd++; if (c2 == 1) np2++; }
                    if (c2 != cls) {
                        if (cls && nrange < 12) printf("lib: fcheck   +%lu..+%lu (%lu bytes) %s\n", (unsigned long)rs, (unsigned long)x, (unsigned long)(x - rs), cls == 1 ? "+2 (an extra word)" : "other");
                        if (cls) nrange++;
                        cls = c2; rs = x;
                    }
                }
                if (k >= 2) { prev[0] = pb[k]; prev[1] = pb[k + 1]; }
            }
            if (cls && !re) { if (nrange < 12) printf("lib: fcheck   +%lu..+%lu (%lu bytes) %s\n", (unsigned long)rs, (unsigned long)e.stored_size, (unsigned long)(e.stored_size - rs), cls == 1 ? "+2 (an extra word)" : "other"); nrange++; }
            printf("lib: fcheck %lu: copy e=%d, %lu bytes, %lu differ (%lu of them +2) in %lu ranges%s\n", (unsigned long)t + 1, ce, (unsigned long)e.stored_size,
                   (unsigned long)nd, (unsigned long)np2, (unsigned long)nrange, re ? (re == 1 ? "; a scratch read failed" : "; a flash read failed") : "");
        }
        free(pa); free(pb);
        if (hold) { (void)CfRc_Reset(); (void)CfRc_Cfg(0, 0, 0, 0, 0); }
        if (_FcOsd == 1) { _FcOsd = 0; for (int w = 0; w < 50 && _FcOsd != -1; w++) vTaskDelay(pdMS_TO_TICKS(20)); _FcOsd = 0; }
        CfRc_LibHold(held);
        return 0;
    }
    if (!strcmp(c, "rtest") && argc >= 4) {
        /* bench (gamecomp1, r1831e shifted windows): lib rtest N LEN [MODE]: a 4 KiB pattern into PSRAM scratch, then N checked
           RREADs (CfRc_PsramRead) of LEN bytes at scratch + 0, 2, 64, 1022 in turn; MODE 1: an FCOPY of 4 KiB of the game area into
           scratch + 64 KiB before each read; 2: a 1 KiB PSRAM write at scratch + 8 KiB before each. Counts: right, the window 2 bytes
           on (word 0 = 00 00, the bytes from word 1), wrong otherwise. The RAM cart must be off */
        if (CfRc_Playing()) { printf("lib: rtest e=1 (a game plays)\n"); return 0; }
        const uint32_t nr = num(argv[2]), len = num(argv[3]), mode = argc >= 5 ? num(argv[4]) : 0;
        uint8_t *w = heap_caps_malloc(1024, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL), *r = malloc(1024);
        if (!w || !r || !len || len > 1022 || nr == 0) { free(r); heap_caps_free(w); printf("lib: rtest e=1\n"); return 0; }
        (void)CfRc_Reset();
        for (uint32_t k = 0; k < 4; k++) {
            for (int i = 0; i < 1024; i++) w[i] = (uint8_t)(((k * 1024u + (uint32_t)i) * 37u) ^ 0xA5u ^ k);
            (void)OSD_QspiPsramWrite(RC_SCRATCH + k * 1024u, w);
        }
        #define RT_EXP(o) ((uint8_t)(((uint32_t)(o) * 37u) ^ 0xA5u ^ ((uint32_t)(o) >> 10)))
        static const uint32_t offs[4] = { 0, 2, 64, 1022 };
        uint32_t ok = 0, sh2 = 0, bad = 0, errs = 0, fce = 0;
        int shown = 0;
        uint32_t ga = 0, gn = 0;
        if (mode == 1 && CfRc_GameArea(&ga, &gn)) { printf("lib: rtest e=1 (no game area)\n"); free(r); heap_caps_free(w); return 0; }
        for (uint32_t t = 0; t < nr; t++) {
            if (mode == 1) { uint32_t crc = 0; if (CfRc_FlashCopy(ga, RC_SCRATCH + 0x10000u, 4096, &crc)) fce++; }
            if (mode == 2) { memset(w, (int)(t & 0xFF), 1024); (void)OSD_QspiPsramWrite(RC_SCRATCH + 0x2000u, w); }
            if (mode == 4) { _P.st = kNwPv_Loading; _P.pct = (int)(t % 100u); vTaskDelay(1); }   /* (the LOADING page redrawn: OSD rows on the bus) */
            const uint32_t o = mode == 3 ? 0 : offs[t & 3u];
            const uint32_t ln = mode == 3 ? ((t & 1u) ? 96u : 8u) : len;   /* (3: a library play's two header reads, psram_read_any) */
            memset(r, 0xEE, ln);
            if (mode == 3 ? psram_read_any(RC_SCRATCH + o, r, ln) : CfRc_PsramRead(RC_SCRATCH + o, r, ln)) { errs++; continue; }
            int good = 1, s2 = ln > 2 && r[0] == 0 && r[1] == 0;
            for (uint32_t i = 0; i < ln; i++) if (r[i] != RT_EXP(o + i)) { good = 0; break; }
            for (uint32_t i = 2; s2 && i < ln; i++) if (r[i] != RT_EXP(o + i - 2u)) s2 = 0;
            if (good) ok++; else if (s2) sh2++; else bad++;
            if (!good && shown < 4) { shown++; printf("lib: rtest #%lu at +%lu: %02x %02x %02x %02x %02x %02x (want %02x %02x %02x %02x)\n", (unsigned long)t, (unsigned long)o,
                                                      r[0], r[1], r[2], r[3], r[4], r[5], RT_EXP(o), RT_EXP(o + 1), RT_EXP(o + 2), RT_EXP(o + 3)); }
        }
        #undef RT_EXP
        if (mode == 4) { _P.st = kNwPv_None; _P.pct = 0; }
        printf("lib: rtest %lu x %lu B mode %lu: %lu right, %lu two bytes on, %lu wrong, %lu errors, %lu fcopy errors\n", (unsigned long)nr, (unsigned long)len,
               (unsigned long)mode, (unsigned long)ok, (unsigned long)sh2, (unsigned long)bad, (unsigned long)errs, (unsigned long)fce);
        free(r); heap_caps_free(w);
        return 0;
    }
    if (!strcmp(c, "rbench") && argc >= 3) {
        /* bench: lib rbench KIB [US]: KIB KiB of a pattern into PSRAM scratch (QSPI writes), then read back three ways:
           checked (RREAD + status), timed, timed + send-ahead; wrong KiB counted (the RAM cart must be off) */
        if (CfRc_Playing()) { printf("lib: rbench e=1 (a game plays)\n"); return 0; }
        const uint32_t kib = num(argv[2]), us = argc >= 4 ? num(argv[3]) : _FastUsRread;
        uint8_t *w = heap_caps_malloc(1024, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL), *r = malloc(1024);
        if (!w || !r || kib == 0 || kib > 3072) { free(r); heap_caps_free(w); printf("lib: rbench e=1\n"); return 0; }
        (void)CfRc_Reset();
        for (uint32_t k = 0; k < kib; k++) {
            for (int i = 0; i < 1024; i++) w[i] = (uint8_t)((k * 131u + (uint32_t)i * 7u) ^ (k >> 3));
            (void)OSD_QspiPsramWrite(RC_SCRATCH + k * 1024u, w);
        }
        const char *const nm[5] = { "checked", "timed", "timed+ahead", "tagged", "tagged+ahead" };
        if (argc >= 5 && !strcmp(argv[4], "tags")) (void)tag_probe();   /* (the tagged modes only when asked: a tagged op changes the design's window state) */
        const int keep_tag = _TagOn;
        for (int m = 0; m < 5; m++) {
            uint32_t bad = 0;
            const int keep = _FastAhead;
            if (m >= 3 && _Tag != 1) break;
            _FastAhead = m == 2 || m == 4;
            _TagOn = m >= 3;
            _TagWaits = _TagPolls = _TagLate = 0;
            memset(&_Ahead, 0, sizeof _Ahead);
            const int64_t t0 = esp_timer_get_time();
            for (uint32_t k = 0; k < kib; k++) {
                const int e = m == 0 ? CfRc_PsramRead(RC_SCRATCH + k * 1024u, r, 1024) : win_timed_lim(CF_RC_O_RREAD, RC_SCRATCH + k * 1024u, r, 1024, us, RC_SCRATCH + kib * 1024u);
                for (int i = 0; i < 1024; i++) w[i] = (uint8_t)((k * 131u + (uint32_t)i * 7u) ^ (k >> 3));
                if (e || memcmp(r, w, 1024)) bad++;
            }
            ahead_done();
            const int64_t ms = (esp_timer_get_time() - t0) / 1000;
            _FastAhead = keep;
            printf("lib: rbench %s: %lu KiB in %lld ms (%lld KiB/s), %lu wrong, margin %lu us, tag polls %lu late %lu\n", nm[m], (unsigned long)kib, (long long)ms,
                   (long long)(ms ? (int64_t)kib * 1000 / ms : 0), (unsigned long)bad, (unsigned long)us, (unsigned long)_TagPolls, (unsigned long)_TagLate);
        }
        _TagOn = keep_tag;
        free(r); heap_caps_free(w);
        return 0;
    }
    if (!strcmp(c, "heap")) {
        printf("lib: heap e=0 8-bit %lu (largest %lu) 32-bit only %lu dma %lu\n", (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT),
               (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_32BIT) - heap_caps_get_free_size(MALLOC_CAP_8BIT)),
               (unsigned long)heap_caps_get_free_size(MALLOC_CAP_DMA));
        heap_caps_print_heap_info(MALLOC_CAP_8BIT);
        return 0;
    }
    if (!strcmp(c, "fast")) {
        /* bench: lib fast [on|off] [FREAD_US RREAD_US] [fcopy|fread] */
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "on")) _FastOn = 1; else if (!strcmp(argv[i], "off")) _FastOn = 0;
            else if (!strcmp(argv[i], "fcopy")) _LibLoad = 0; else if (!strcmp(argv[i], "fread")) _LibLoad = 1;
            else if (!strcmp(argv[i], "ahead")) _FastAhead = 1; else if (!strcmp(argv[i], "noahead")) _FastAhead = 0;
            else if (!strcmp(argv[i], "tag")) _TagOn = 1; else if (!strcmp(argv[i], "notag")) _TagOn = 0;
            else if (i + 1 < argc) { _FastUsFread = num(argv[i]); _FastUsRread = num(argv[i + 1]); i++; }
        }
        printf("lib: fast e=0 %s fread %lu us rread %lu us load %s %s tag %s (design %s)\n", _FastOn ? "on" : "off", (unsigned long)_FastUsFread, (unsigned long)_FastUsRread,
               _LibLoad ? "fread" : "fcopy", _FastAhead ? "ahead" : "noahead", _TagOn ? "on" : "off", _Tag < 0 ? "not probed" : _Tag ? "tags" : "no tags");
        return 0;
    }
    printf("lib: st | ls | imp ADDR LEN keep|play NAME | imp begin LEN NAME | imp part ADDR LEN OFF | imp end CRC32 | del UID | play UID | fmt yes\n");
    return 0;
}

void CfRc_Register(void)
{
    const esp_console_cmd_t cmd = {
        .command = "rc",
        .help = "RAM cart + FPGA boot flash (1.1.0 bench): rc st | fid | fcrc | probe | load | ram | on | off | save | rtc",
        .hint = NULL,
        .func = &cmd_rc,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
    const esp_console_cmd_t lcmd = { .command = "lib", .help = "the game library on the FPGA flash: lib st | ls | imp | del | play | fmt yes", .hint = NULL, .func = &cmd_lib };
    ESP_ERROR_CHECK(esp_console_cmd_register(&lcmd));
}

#endif /* NW_CARTFLASH */
