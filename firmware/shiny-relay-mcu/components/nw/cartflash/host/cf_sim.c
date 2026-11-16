/* cf_sim.c: "Send to my Chromatic" end to end on the Mac (lane cartflash1).
 *
 * SameBoy runs the TEST boot ROM (stock CGB boot + sm83/cf_netboot.inc) exactly as the
 * FPGA would; every cartridge access goes to a simulated flash cart (cf_cart.c) instead
 * of SameBoy's own MBC; FF58-FF5F go to the network window model (tools/nw/nw_fpga_model.c)
 * behind a 115200-baud UART model; behind that the REAL ESP32 window service (nw_esp.c,
 * built with NW_CARTFLASH) and the REAL cart job engine (cartflash/cf_esp.c) with
 * in-memory stand-ins for the ESP32 flash partition and NVS. GET/POST calls (the read-only
 * probe, which uses the stock v1 ops) go over HTTP to a running cart_probe_rx.py.
 *
 *   cf_sim --scenario NAME [--receiver PORT] [--loss PCT] [-v]
 * Scenarios: probe, probe_prod, install, cut_backup, cut_erase, cut_header, cut_program,
 *            verify_fail, wrong_chip, gated, unverified, roundtrip, header_mapper, loss,
 *            esp_reboot, bad_image, other_cart
 */
#define _DEFAULT_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <Core/gb.h>

#include "cf_cart.h"
#include "cf_esp.h"
#include "nw_esp.h"
#include "nw_fpga_model.h"

static int verbose;
static GB_gameboy_t *gb;
static cart_t cart;
static nw_fpga_t F;
static nw_esp_t E;
static cf_esp_t C;
static int boot_mapped = 1;
static double t_us, next_ms_us;
static uint32_t gb_ms;
static int want_cycle, netboot_sel, n_cycles, n_boot_sel;
static int game_marker = -1;
static int receiver_port;
static double loss_pct;
static uint32_t rng = 0xC0FFEEu;
static char bootrom_path[1024];
static unsigned n_cart_writes_total;
static unsigned n_boot_blocked;          /* CPU writes the mapped boot ROM kept off the cart bus */
/* a slow ESP32: nothing answers the FPGA before esp_up_ms (it is rebooting after the SRAM
   load), no Wi-Fi before wifi_up_ms (0 = never) */
static uint32_t esp_up_ms, wifi_up_ms;
static int esp_is_up = 1;
static int esp_stock_fw;          /* the ESP32 runs firmware without NW_CARTFLASH (0.17.x) */
/* the USB console: a child process (cart_probe_usb.py --stdio) */
static int usb_in = -1, usb_out = -1;
static pid_t usb_pid;
static int opt_usb_chunk;        /* --usb-chunk: 512 = the chunk size of 0.18.0-rc1 (its lines do not fit the console) */
static char usb_buf[8192];
static size_t usb_n;
static unsigned usb_lines_out, usb_cmds_in;
/* the console UART (ESP32 -> computer): con_rate bytes per ms (115200 baud = 11.52); the nw
   task blocks in printf / uart_write_bytes while more than CON_TXBUF bytes wait */
static uint8_t *con; static size_t con_n, con_head, con_cap;
static double con_rate = 11.52, con_credit;
/* --fpga-bpms X: the ESP32 <-> FPGA UART in bytes per ms (115200 baud = 11.52; wifispeed1: what a faster UART would give) */
static double fpga_bpms = 11.52;
static size_t CON_TXBUF = 4096;           /* the device's console stream buffer (cf_dev.c) */
static int usb_sync;                       /* old path: every chunk a CPX line + answer */
static double usb_answer_ms;               /* computer -> ESP32 command latency (daemon, REPL) */
typedef struct { char s[1100]; double due; } ans_t;
static ans_t ans_q[64]; static int ans_h, ans_t_;
static int opt_ram_kib; static double opt_ram_timeout_ms;
static int opt_gb_writes_flash;            /* 0: the ModRetro truth (GB-timed writes miss the flash) */
static int opt_no_engine;                  /* an FPGA without the cart bus engine */
/* 1.0.2: --chip 1681 runs a scenario's SST39VF1682 cart as an SST39VF1681 (BF C8, no protected top
   block); --wp-low holds its WP# low (the BOTTOM 64 KiB ignore program / erase) */
static int opt_chip1681, opt_wp_low, opt_wp_garble;   /* --wp-garble: the write test's read-back is neither value */
static int wp_seen_done;                   /* a write test passed in some session of this run (C is reset per session) */
static int want_marker = 0x42;             /* the marker the written game shows when it boots */

/* ---------------------------------------------------------------- the cart bus engine (model of
   fpga rtl/BSP/cart_engine.v, register-level): FF60-FF67, only while the ESP32 allows it */
static struct {
    int allow, session, autoinc, err_tmo, err_vfy, op;
    uint16_t addr;
    uint8_t val;
    double t0;
    unsigned n_ops, n_progs, n_erases;
} EG;
enum { EOP_NONE, EOP_PROG, EOP_ERASE };
/* The FPGA's patch overlay (18.22+), as far as a cart job meets it: while a session is ACTIVE the
   CPU's reads of some cart ROM bytes are answered with the patch's bytes. The engine's reads are
   the pins. A session starts at a core reset when CONFIG has patches on, and ends at one when
   it has them off. (unit 1, 2026-10-11: a USB backup under an active Tetris patch came back
   patched in 0x100-0x102, 0x3f00-0x3f66, 0x30000-0x30dc6 and more.) */
static int ovl_cfg_on;         /* the owner's setting: a patch is chosen */
static int ovl_hold;           /* the firmware holds patches off (a cart job) */
static int ovl_active;         /* the running core is served through the overlay */
static int ovl_stuck;          /* a broken FPGA / lost CONFIG: the overlay stays on whatever is sent */
static int ovl_asked, opt_legacy_no_hold;
static unsigned n_ovl_reads, n_ovl_holds, n_ovl_reads_job;
static int ovl_job;            /* a cart job is running (the reads counted in n_ovl_reads_job) */
/* (the entry jump at 0x100 is patched on the unit too; left alone here so the model's test games start) */
static int ovl_hit(uint32_t off) { return (off >= 0x3F00 && off <= 0x3F66) || (off >= 0x282F0 && off <= 0x282F7) || (off >= 0x30000 && off <= 0x30DC6); }
static void h_overlay_hold(void *u, int on) { (void)u; ovl_hold = on; n_ovl_holds++; }
static int h_overlay_state(void *u, int ask)
{
    (void)u;
    if (ask) { ovl_asked = 1; return -1; }
    if (ovl_asked) { ovl_asked = 0; return -1; }       /* the report is on its way: one more call */
    return ovl_active;
}
static void xs_session_end(void);
static void h_engine_allow(void *u, int on) { (void)u; EG.allow = on && !opt_no_engine; if (!EG.allow) { EG.session = 0; EG.op = EOP_NONE; xs_session_end(); } }
static void eg_w(uint16_t a, uint8_t v) { cart_write_engine(&cart, a, v); }
static void eg_unlock(void) { eg_w(0x0AAA, 0xAA); eg_w(0x0555, 0x55); }
static int eg_busy(void) { return EG.op != EOP_NONE; }
static void eg_step(void)
{
    if (EG.op == EOP_NONE) return;
    if (!cart_busy(&cart)) {
        if (EG.op == EOP_PROG) { if (cart_read(&cart, EG.addr) != EG.val) EG.err_vfy = 1; EG.addr++; }
        EG.op = EOP_NONE;
    } else if (t_us - EG.t0 > (EG.op == EOP_PROG ? 500.0 : 4000000.0)) { EG.err_tmo = 1; EG.op = EOP_NONE; }
}
static int eg_read(uint16_t addr, uint8_t *v)
{
    if (!EG.allow || addr < 0xFF60 || addr > 0xFF67) return 0;
    switch (addr & 7) {
    case 0: *v = (uint8_t)(0xA0 | EG.err_vfy << 3 | EG.err_tmo << 2 | eg_busy() << 1 | EG.session); break;
    case 1: *v = (uint8_t)EG.addr; break;
    case 2: *v = (uint8_t)(EG.addr >> 8); break;
    case 3: *v = cart_read(&cart, EG.addr); if (EG.autoinc) EG.addr++; break;   /* the pins: no boot ROM shadow */
    default: *v = 0xFF; break;
    }
    return 1;
}
static int eg_write(uint16_t addr, uint8_t v)
{
    if (!EG.allow || addr < 0xFF60 || addr > 0xFF67) return 0;
    const int r = addr & 7;
    if (r == 0) { EG.session = v & 1; EG.autoinc = (v >> 1) & 1; if (v & 4) EG.err_tmo = EG.err_vfy = 0; return 1; }
    if (r == 1) { EG.addr = (uint16_t)((EG.addr & 0xFF00) | v); return 1; }
    if (r == 2) { EG.addr = (uint16_t)((EG.addr & 0x00FF) | v << 8); return 1; }
    if (!EG.session || eg_busy()) { EG.err_tmo = 1; return 1; }       /* the RTL queues one op; the writer waits anyway */
    EG.n_ops++;
    EG.t0 = t_us;
    switch (r) {
    case 4: eg_w(EG.addr, v); if (EG.autoinc && (EG.addr & 0xE000) == 0xA000) EG.addr++; break;   /* RAM fills step ADDR */
    case 5: if (v == 0xFF) { EG.addr++; break; }
            eg_unlock(); eg_w(0x0AAA, 0xA0); eg_w(EG.addr, v); EG.val = v; EG.op = EOP_PROG; EG.n_progs++; break;
    case 6: eg_unlock(); eg_w(0x0AAA, v); break;
    case 7: eg_unlock(); eg_w(0x0AAA, 0x80); eg_unlock(); eg_w(v == 0x10 ? 0x0AAA : EG.addr, v); EG.op = EOP_ERASE; EG.n_erases++; break;
    }
    return 1;
}

#include "cf_xs_model.inc"

/* ---------------------------------------------------------------- ESP32 storage stand-ins */
static uint8_t *img; static uint32_t img_size;
static uint8_t *bak; static uint32_t bak_cap;
static uint8_t ram_bak[CF_RAM_SIZE];
static cf_journal_t journal; static int journal_valid;
static int power_good = 1;

static int h_img_read(void *u, uint32_t off, uint8_t *b, uint32_t n) { (void)u; if (off + n > img_size) return -1; memcpy(b, img + off, n); return 0; }
static int h_bak_write(void *u, uint32_t off, const uint8_t *b, uint32_t n) { (void)u; if (off + n > bak_cap) return -1; memcpy(bak + off, b, n); return 0; }
static int h_bak_read(void *u, uint32_t off, uint8_t *b, uint32_t n) { (void)u; if (off + n > bak_cap) return -1; memcpy(b, bak + off, n); return 0; }
static int h_ram_write(void *u, uint32_t off, const uint8_t *b, uint32_t n) { (void)u; memcpy(ram_bak + off, b, n); return 0; }
static int h_ram_read(void *u, uint32_t off, uint8_t *b, uint32_t n) { (void)u; memcpy(b, ram_bak + off, n); return 0; }
static uint8_t sav2[CF_RAM_SIZE];
static int h_sav_write(void *u, uint32_t off, const uint8_t *b, uint32_t n) { (void)u; if (off + n > sizeof sav2) return -1; memcpy(sav2 + off, b, n); return 0; }
static int stg_sav_src;        /* the staged save is the save source (usb-cart scenarios) */
static int h_stage_read(void *u, int sav, uint32_t off, uint8_t *b, uint32_t n);
static int h_sav_read(void *u, uint32_t off, uint8_t *b, uint32_t n) { (void)u; if (stg_sav_src) return h_stage_read(NULL, 1, off, b, n); if (off + n > sizeof sav2) return -1; memcpy(b, sav2 + off, n); return 0; }
static int h_jr_load(void *u, cf_journal_t *j) { (void)u; if (!journal_valid) return -1; *j = journal; return 0; }
static int h_jr_save(void *u, const cf_journal_t *j) { (void)u; journal = *j; journal_valid = 1; return 0; }
static void h_boot_select(void *u, int on) { (void)u; netboot_sel = on; n_boot_sel++; }
static void h_cart_cycle(void *u) { (void)u; want_cycle = 1; }
static int h_power_ok(void *u) { (void)u; return power_good; }
static void h_progress(void *u, const cf_status_t *s) { (void)u; if (verbose > 1) fprintf(stderr, "  [osd] %s\n", s->text); }
static void h_log(void *u, const char *m) { (void)u; if (verbose) fprintf(stderr, "  %8.3f s %s\n", t_us / 1e6, m); }
static long con_flip_at = -1;          /* corrupt the byte at this position of the console stream, once */
static unsigned long con_total;
static void con_put(const void *b0, size_t n)
{
    const uint8_t *b = b0;
    uint8_t *t = NULL;
    if (con_flip_at >= 0 && (unsigned long)con_flip_at >= con_total && (unsigned long)con_flip_at < con_total + n) {
        t = malloc(n);
        memcpy(t, b, n);
        t[(unsigned long)con_flip_at - con_total] ^= 0x20;
        con_flip_at = -1;
        b = t;
    }
    con_total += n;
    if (con_n + n > con_cap) { con_cap = (con_n + n) * 2 + 65536; con = realloc(con, con_cap); }
    memcpy(con + con_n, b, n);
    con_n += n;
    free(t);
}
static size_t con_backlog(void) { return con_n - con_head; }
static void con_drain(double bytes)
{
    if (usb_in < 0) { con_head = con_n; return; }
    size_t k = con_rate > 0 ? (size_t)bytes : con_backlog();
    if (k > con_backlog()) k = con_backlog();
    while (k) {
        const ssize_t w = write(usb_in, con + con_head, k);
        if (w <= 0) break;
        con_head += (size_t)w;
        k -= (size_t)w;
    }
    if (con_head == con_n) con_head = con_n = 0;
}
static void h_host_line(void *u, const char *line)
{
    (void)u;
    usb_lines_out++;
    con_put(line, strlen(line));
    con_put("\n", 1);
}
static void h_host_frame(void *u, const uint8_t *f, uint32_t n) { (void)u; con_put(f, n); }

/* ---------------------------------------------------------------- the image sources of a streamed
   install: the gateway (a buffer here, with a link that can drop, lie once, or forget the image)
   and the owner's computer over USB (cf_usb.c, the device's own code) */
#include "../cf_usb.h"
static cf_usb_t U;
static uint8_t *net_img; static uint32_t net_size;
static int net_down;                 /* the link is down: nothing arrives */
static int net_gone;                 /* the server no longer has the image */
static int net_lag = 3;              /* asks before a chunk is there (a ranged GET takes a while) */
static long net_flip_at = -1;        /* deliver this offset corrupted, ONCE */
static unsigned net_chunks, net_bytes, net_asks;
static uint32_t net_w_off; static int net_w_cnt;
/* the ESP32's staging area (usb-cart-1): what the computer wrote in bootloader mode */
static uint8_t stg_rom[CF_STAGE_ROM_MAX], stg_sav[CF_RAM_SIZE];
static int stg_room_bad, stg_erase_ms;
static unsigned n_stg_erases;
static int h_stage_read(void *u, int sav, uint32_t off, uint8_t *b, uint32_t n)
{
    (void)u;
    if (off + n > (sav ? sizeof stg_sav : sizeof stg_rom)) return -1;
    memcpy(b, (sav ? stg_sav : stg_rom) + off, n);
    return 0;
}
static int h_stage_room(void *u) { (void)u; return stg_room_bad; }
/* stg_online: an ONLINE unit (unit 2, 2026-10-12): the associated radio and its HTTPS sessions
   hold so much heap that a cart job is refused ("busy memory") unless the link hold was taken
   first; the hold is lifted after the job, so the link session must come back. */
static int sim_online, sim_held, n_hold_on, n_hold_off;
static void h_net_hold(void *u, int on) { (void)u; sim_held = on; if (on) n_hold_on++; else n_hold_off++; }
static int h_heap_ok(void *u) { (void)u; return !sim_online || sim_held; }
static uint32_t h_heap_free(void *u) { (void)u; return sim_online && !sim_held ? 3100u : 40000u; }
static int opt_stg_erase_fail;        /* --stg-erase-fail: the unit could not erase its copy (the tool must not call that a success) */
static void h_stage_erase(void *u) { (void)u; if (!opt_stg_erase_fail) { memset(stg_rom, 0xFF, sizeof stg_rom); memset(stg_sav, 0xFF, sizeof stg_sav); } stg_erase_ms = 3000; n_stg_erases++; }
static int h_stage_busy(void *u) { (void)u; if (stg_erase_ms > 0) stg_erase_ms--; return stg_erase_ms > 0 ? 1 : opt_stg_erase_fail ? -1 : 0; }

/* cartspeed2: the Wi-Fi image arriving while the cart is written (cf_dev.c img_avail / img_gate):
   prog_rate bytes per simulated second from prog_t0; the gate opens when all of it is there, or
   says "not the image" (prog_bad) */
static int prog_on, prog_bad;
static int no_logo(void);
static double prog_rate = 160.0 * 1024.0, prog_t0;
static unsigned prog_early_b0, prog_gate_waits;
static int prog_blank_seen;              /* the old game's block 0 was erased (from then on a logo is the new one) */
static void prog_look(uint32_t h) { if (no_logo()) prog_blank_seen = 1; else if (prog_blank_seen && h < net_size) prog_early_b0++; }
static uint32_t prog_have(void)
{
    const double h = (t_us - prog_t0) / 1e6 * prog_rate;
    return h >= (double)net_size ? net_size : (uint32_t)h;
}
static uint32_t h_img_avail(void *u)
{
    (void)u;
    if (!prog_on || C.j.strm_src != CF_SRC_WIFI_STAGED) return 0xFFFFFFFFu;
    const uint32_t h = prog_have();
    prog_look(h);                                             /* block 0 must wait for the whole image */
    return h;
}
static int h_img_gate(void *u)
{
    (void)u;
    if (!prog_on || C.j.strm_src != CF_SRC_WIFI_STAGED) return 0;
    if (prog_have() < net_size) { prog_gate_waits++; prog_look(prog_have()); return 1; }
    return prog_bad ? -1 : 0;
}
static int prog_blkcrc;              /* the download's per-block crc32 (cf_dev.c img_blk_crc), of net_img */
static int h_img_blk_crc(void *u, uint32_t off, uint32_t n, uint32_t *crc)
{
    (void)u;
    if (!prog_blkcrc || C.j.strm_src != CF_SRC_WIFI_STAGED || off >= net_size) return 0;
    const uint32_t e = off + n < net_size ? off + n : net_size;
    if (prog_on && prog_have() < e) return 0;
    *crc = cf_crc32(0, net_img + off, e - off);
    return 1;
}
static int h_chunk(void *u, uint32_t off, uint16_t n, uint8_t *dst)
{
    (void)u;
    if (C.j.job == CF_JOB_STREAM && C.j.strm_src == CF_SRC_WIFI_STAGED && prog_on && off < sizeof stg_rom && h_img_avail(NULL) < off + n) return 0;
    if (C.j.job == CF_JOB_STREAM && C.j.strm_src == 2) return h_stage_read(NULL, 0, off, dst, n) ? -1 : 1;
    if (C.j.job == CF_JOB_STREAM && C.j.strm_src == CF_SRC_WIFI_STAGED) {   /* as cf_dev.c wstage_read: FF past the staging area */
        uint32_t k = off < sizeof stg_rom ? (sizeof stg_rom - off < n ? sizeof stg_rom - off : n) : 0;
        if (k) memcpy(dst, stg_rom + off, k);
        if (k < n) memset(dst + k, 0xFF, n - k);
        return 1;
    }
    if (C.j.job == CF_JOB_USBRAM || (C.j.job == CF_JOB_STREAM && C.j.strm_src == 1)) return cf_usb_chunk(&U, off, n, dst);
    net_asks++;
    if (net_gone) return -1;
    if (net_down || !net_img || off + n > net_size) return 0;
    if (net_w_off != off) { net_w_off = off; net_w_cnt = 0; }
    if (++net_w_cnt <= net_lag) return 0;
    memcpy(dst, net_img + off, n);
    if (net_flip_at >= 0 && (uint32_t)net_flip_at >= off && (uint32_t)net_flip_at < off + n) { dst[(uint32_t)net_flip_at - off] ^= 0x10; net_flip_at = -1; }
    net_chunks++; net_bytes += n;
    net_w_off = 0xFFFFFFFFu;
    return 1;
}

static cf_host_t host(void)
{
    cf_host_t h = { 0 };
    h.img_size = img_size; h.img_read = h_img_read;
    h.bak_write = h_bak_write; h.bak_read = h_bak_read; h.ram_write = h_ram_write; h.ram_read = h_ram_read; h.sav_write = h_sav_write; h.sav_read = h_sav_read;
    h.bak_capacity = bak_cap;
    h.jr_load = h_jr_load; h.jr_save = h_jr_save;
    h.boot_select = h_boot_select; h.cart_cycle = h_cart_cycle; h.power_ok = h_power_ok;
    h.progress = h_progress; h.log = h_log; h.host_line = h_host_line;
    h.host_frame = usb_sync ? NULL : h_host_frame;
    h.engine_allow = h_engine_allow;
    h.chunk = h_chunk;
    if (!opt_legacy_no_hold) { h.overlay_hold = h_overlay_hold; h.overlay_state = h_overlay_state; }
    if (opt_xs) { h.xs_avail = h_xs_avail; h.xs_send = h_xs_send; h.xs_ring = h_xs_ring; }
    h.img_avail = h_img_avail; h.img_gate = h_img_gate; h.img_blk_crc = h_img_blk_crc;
    return h;
}

/* ---------------------------------------------------------------- the gateway (probe only) */
static void be_call(void *u, nw_esp_t *e, uint8_t id, const nw_call_t *c)
{
    (void)u;
    if ((c->op != NW_OP_GET && c->op != NW_OP_POST) || !receiver_port) { nw_esp_complete(e, id, NW_S_OFFLINE, 0, 0, NULL, 0); return; }
    char gid[33];
    for (int i = 0; i < 16; i++) snprintf(gid + 2 * i, 3, "%02x", c->game_id[i]);
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)receiver_port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (s < 0 || connect(s, (struct sockaddr *)&a, sizeof a) != 0) { if (s >= 0) close(s); nw_esp_complete(e, id, NW_S_OFFLINE, 0, 0, NULL, 0); return; }
    char hdr[512];
    const int post = c->op == NW_OP_POST;
    int hn = snprintf(hdr, sizeof hdr, "%s /gbnet/v1/games/%s/content/%s HTTP/1.0\r\nHost: 127.0.0.1\r\nX-Game-Version: %u\r\n"
                      "Authorization: Device sim-token-not-a-secret\r\nContent-Length: %u\r\n%s\r\n",
                      post ? "POST" : "GET", gid, c->path, c->game_ver, post ? c->body_len : 0u,
                      post ? "Content-Type: application/octet-stream\r\n" : "");
    (void)!write(s, hdr, (size_t)hn);
    if (post && c->body_len) (void)!write(s, c->body, c->body_len);
    static uint8_t rb[65536];
    size_t got = 0;
    for (;;) { ssize_t k = read(s, rb + got, sizeof rb - got); if (k <= 0) break; got += (size_t)k; }
    close(s);
    int code = 0;
    sscanf((const char *)rb, "HTTP/%*s %d", &code);
    const uint8_t *body = NULL;
    for (size_t i = 0; i + 3 < got; i++) if (!memcmp(rb + i, "\r\n\r\n", 4)) { body = rb + i + 4; break; }
    uint16_t blen = body ? (uint16_t)(got - (size_t)(body - rb)) : 0;
    const uint8_t st = code == 200 || code == 206 ? NW_S_OK : code == 404 ? NW_S_NOT_FOUND : code == 429 ? NW_S_RATE_LIMITED
                     : code == 403 ? NW_S_DENIED : code ? NW_S_SERVER_ERROR : NW_S_OFFLINE;
    nw_esp_complete(e, id, st, 0, blen, body, blen);
}
static void be_cancel(void *u, uint8_t id) { (void)u; (void)id; }

/* ---------------------------------------------------------------- UART model (as shl_dev.c) */
#define QMAX 4096
typedef struct { uint8_t p[260]; int n; } qpkt_t;
static qpkt_t q_e2f[QMAX];
static int e2f_head, e2f_tail;
static double e2f_credit, f2e_credit;
static uint8_t f2e_cur[260];
static int f2e_n;
static unsigned n_dropped;

static void esp_only_boot(void);
static int lose(void)
{
    if (loss_pct <= 0) return 0;
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    if ((rng % 10000u) < (unsigned)(loss_pct * 100)) { n_dropped++; return 1; }
    return 0;
}
static void esp_send(void *u, const uint8_t *p, int n)
{
    (void)u;
    if (e2f_tail - e2f_head >= QMAX) return;
    qpkt_t *q = &q_e2f[e2f_tail % QMAX];
    memcpy(q->p, p, (size_t)n);
    q->n = n;
    e2f_tail++;
}
static void step_ms(void)
{
    gb_ms++;
    if (!esp_is_up) {
        if (gb_ms < esp_up_ms) {               /* the ESP32 is booting: the FPGA hears nothing */
            f2e_n = 0;
            uint8_t junk[260];
            while (nw_fpga_to_esp(&F, junk)) {}
            nw_fpga_tick_ms(&F, 1);
            return;
        }
        esp_is_up = 1;
        esp_only_boot();
        if (verbose) fprintf(stderr, "  %8.3f s ESP32 up (window service)\n", t_us / 1e6);
    }
    if (con_rate > 0) { con_credit += con_rate; if (con_credit > 64) { con_drain(con_credit); con_credit = 0; } }
    else con_drain(0);
    if (con_backlog() > CON_TXBUF) {           /* the nw task sits in printf: nothing else runs */
        nw_fpga_tick_ms(&F, 1);
        e2f_credit += fpga_bpms;
        while (e2f_head != e2f_tail && e2f_credit >= q_e2f[e2f_head % QMAX].n + 4) {
            qpkt_t *q = &q_e2f[e2f_head % QMAX];
            e2f_credit -= q->n + 4;
            e2f_head++;
            if (!lose()) nw_fpga_from_esp(&F, q->p, q->n);
        }
        return;
    }
    if (esp_up_ms) {                           /* only on change, as nw_dev.c does */
        const int on = wifi_up_ms && gb_ms >= wifi_up_ms;
        if (on != E.online) nw_esp_set_online(&E, on);
    }
    e2f_credit += fpga_bpms;
    while (e2f_head != e2f_tail && e2f_credit >= q_e2f[e2f_head % QMAX].n + 4) {
        qpkt_t *q = &q_e2f[e2f_head % QMAX];
        e2f_credit -= q->n + 4;
        e2f_head++;
        if (!lose()) nw_fpga_from_esp(&F, q->p, q->n);
    }
    xs_line_ms(&e2f_credit, &f2e_credit);
    if (e2f_head == e2f_tail && e2f_credit > 12 && XS.in_h == XS.in_t) e2f_credit = 12;
    f2e_credit += fpga_bpms;
    for (;;) {
        if (!f2e_n) { f2e_n = nw_fpga_to_esp(&F, f2e_cur); if (!f2e_n) break; }
        if (f2e_credit < f2e_n + 4) break;
        f2e_credit -= f2e_n + 4;
        if (!lose()) nw_esp_on_packet(&E, f2e_cur, f2e_n);
        f2e_n = 0;
    }
    xs_line_ms(&e2f_credit, &f2e_credit);
    if (!f2e_n && f2e_credit > 12 && !XS.chg) f2e_credit = 12;
    nw_fpga_tick_ms(&F, 1);
    nw_esp_tick(&E, gb_ms);
    cf_esp_tick(&C, gb_ms);
    cf_usb_tick(&U, gb_ms);
}

/* ---------------------------------------------------------------- the GB side */
static uint8_t on_read(GB_gameboy_t *g, uint16_t addr, uint8_t data)
{
    (void)g;
    if (addr >= NW_REG_ID && addr <= NW_REG_RSVD) { uint8_t v; return nw_fpga_read(&F, addr, &v) ? v : 0xFF; }
    { uint8_t v; if (eg_read(addr, &v)) return v; if (addr >= 0xFF60 && addr <= 0xFF67) return 0xFF; }
    if (EG.session && EG.allow && (addr < 0x8000 || (addr >= 0xA000 && addr < 0xC000))) {
        if (!(boot_mapped && (addr < 0x100 || (addr >= 0x200 && addr < 0x900)))) return 0xFF;   /* the engine owns the bus */
    }
    if (addr < 0x8000) {
        if (boot_mapped && (addr < 0x100 || (addr >= 0x200 && addr < 0x900))) return data;   /* the boot ROM */
        if (ovl_active) {
            const uint32_t off = cart_rom_offset(&cart, addr);
            if (ovl_hit(off)) { n_ovl_reads++; if (ovl_job && netboot_sel) n_ovl_reads_job++; return (uint8_t)(cart.flash[off % cart.cfg.size] ^ 0x5A); }
        }
        return cart_read(&cart, addr);
    }
    if (addr >= 0xA000 && addr < 0xC000) return cart_read(&cart, addr);
    return data;
}
static bool on_write(GB_gameboy_t *g, uint16_t addr, uint8_t v)
{
    (void)g;
    if (eg_write(addr, v)) return true;
    if (addr < 0x8000 || (addr >= 0xA000 && addr < 0xC000)) {
        if (EG.session && EG.allow) return false;            /* the engine owns the bus: a GB write goes nowhere */
        /* gb.v: ext_bus_a15 = cpu_addr[15] | sel_boot_rom, cart_wr = cart_sel & ext_bus_wr. While the
           boot ROM is mapped, a CPU write to 0000-00FF or 0200-08FF never becomes a cart write
           (the first hardware runs lost 0A@0000, 55@0555, AA@0555/55@02AA and F0@0000 this way). */
        if (boot_mapped && (addr < 0x100 || (addr >= 0x200 && addr < 0x900))) { n_boot_blocked++; return false; }
        n_cart_writes_total++; cart_write(&cart, addr, v); return false;
    }
    if (addr == 0xFF50 && (v & 1)) boot_mapped = 0;
    if (addr == 0xFFF0 && !boot_mapped) game_marker = v;
    if (verbose > 2 && (addr == 0xFF58 || addr == 0xFF5A))
        fprintf(stderr, "  %8.3f s W %04x=%02x (unlocked %d svc %d busy %d)\n", t_us / 1e6, addr, v, F.unlocked, F.svc, F.busy);
    nw_fpga_write(&F, addr, v);
    return true;
}
static void quiet_log(GB_gameboy_t *g, const char *s, GB_log_attributes_t a) { (void)g; (void)s; (void)a; }
static uint32_t pixels[256 * 224];
static uint32_t rgb_encode(GB_gameboy_t *g, uint8_t r, uint8_t gg, uint8_t b) { (void)g; return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)gg << 8) | b; }

static void gb_reset(void)
{
    EG.session = 0; EG.op = EOP_NONE;
    xs_session_end();
    ovl_active = ovl_stuck ? ovl_active : (ovl_cfg_on && !ovl_hold);   /* a session starts / ends with the core reset */
    GB_reset(gb);
    boot_mapped = 1;
    game_marker = -1;
    nw_fpga_gb_reset(&F);
}

static void boot(void)
{
    if (gb) GB_free(gb);
    gb = GB_init(GB_alloc(), GB_MODEL_CGB_E);
    if (GB_load_boot_rom(gb, bootrom_path) != 0) { fprintf(stderr, "boot rom %s?\n", bootrom_path); exit(2); }
    static uint8_t dummy[0x8000];
    dummy[0x147] = 0x1B; dummy[0x148] = 0; dummy[0x149] = 3;
    GB_load_rom_from_buffer(gb, dummy, sizeof dummy);     /* never read: every cart access goes to cf_cart */
    GB_set_log_callback(gb, quiet_log);
    GB_set_rgb_encode_callback(gb, rgb_encode);
    GB_set_pixels_output(gb, pixels);
    GB_set_sample_rate(gb, 0);
    GB_set_turbo_mode(gb, true, true);
    GB_set_read_memory_callback(gb, on_read);
    GB_set_write_memory_callback(gb, on_write);
    boot_mapped = 1;
    game_marker = -1;
}

static void esp_boot(void)
{
    static const nw_esp_config_t cfg = { 0, 0x17, 4, 8, 10000 };
    static const nw_backend_t be = { NULL, be_call, be_cancel };
    nw_fpga_init(&F);
    F.present = 1;
    nw_esp_init(&E, &cfg, &be, esp_send, NULL);
    nw_esp_set_online(&E, 1);
    const cf_host_t h = host();
    cf_esp_init(&C, &h);
    cf_usb_init(&U, &C, h_host_line, NULL); U.chunk = (uint16_t)opt_usb_chunk; U.bin_idle_ms = 3600000u; U.stage_read = h_stage_read; U.stage_room = h_stage_room; U.stage_erase = h_stage_erase; U.stage_busy = h_stage_busy;
    U.net_hold = h_net_hold; U.heap_ok = h_heap_ok; U.heap_free = h_heap_free; U.heap_need = 6144;
    cf_usb_init(&U, &C, h_host_line, NULL); U.chunk = (uint16_t)opt_usb_chunk; U.bin_idle_ms = 3600000u; U.stage_read = h_stage_read; U.stage_room = h_stage_room; U.stage_erase = h_stage_erase; U.stage_busy = h_stage_busy;
    U.net_hold = h_net_hold; U.heap_ok = h_heap_ok; U.heap_free = h_heap_free; U.heap_need = 6144;
    E.cf_op = esp_stock_fw ? NULL : cf_esp_op;
    E.cf_user = &C;
    E.cf_probe_owns = esp_stock_fw ? NULL : cf_probe_owns;
    E.cf_probe_call = cf_probe_call;
    e2f_head = e2f_tail = 0; f2e_n = 0; e2f_credit = f2e_credit = 0;
}

/* the ESP32 alone reboots (an FPGA SRAM load resets it): window model untouched */
static void esp_only_boot(void)
{
    static const nw_esp_config_t cfg = { 0, 0x17, 4, 8, 10000 };
    static const nw_backend_t be = { NULL, be_call, be_cancel };
    nw_esp_init(&E, &cfg, &be, esp_send, NULL);
    const cf_host_t h = host();
    cf_esp_init(&C, &h);
    cf_usb_init(&U, &C, h_host_line, NULL); U.chunk = (uint16_t)opt_usb_chunk; U.bin_idle_ms = 3600000u; U.stage_read = h_stage_read; U.stage_room = h_stage_room; U.stage_erase = h_stage_erase; U.stage_busy = h_stage_busy;
    U.net_hold = h_net_hold; U.heap_ok = h_heap_ok; U.heap_free = h_heap_free; U.heap_need = 6144;
    E.cf_op = esp_stock_fw ? NULL : cf_esp_op; E.cf_user = &C;
    E.cf_probe_owns = esp_stock_fw ? NULL : cf_probe_owns; E.cf_probe_call = cf_probe_call;
    e2f_head = e2f_tail = 0; f2e_n = 0;
    const uint8_t st = NW_R_STATUS;        /* as nw_dev.c does on first sight of the window: */
    esp_send(NULL, &st, 1);                /* learn the window state (a game may have it open) */
}

/* The console's receive side as the unit has it (ESP-IDF REPL on UART0, 115200): the 128-byte
   hardware FIFO and a 256-byte ring, filled at 11.52 characters per ms and emptied at about 1.3
   (the REPL echoes each character and waits for it to be sent). What does not fit is LOST: on
   unit 1 (2026-10-11) every 700-character data line lost its middle after ~435 characters.
   --rx-repl switches the model on; --rx-loss PCT damages that share of the lines somewhere
   besides; --rx-kill OFF,N damages the first N data lines for offset OFF. */
static int opt_rx_repl; static double opt_rx_loss; static long opt_rx_kill_off = -1; static int opt_rx_kill_n;
static char rx_wire[65536]; static size_t rx_wire_n;
static char rx_q[384]; static size_t rx_q_n;
static double rx_in_credit, rx_out_credit;
static unsigned n_rx_dropped, n_rx_damaged_lines;
static uint32_t rx_rng = 0x1234567u;
static uint32_t rx_rand(void) { rx_rng = rx_rng * 1664525u + 1013904223u; return rx_rng >> 8; }
/* the tool's bytes onto the wire, with the injected damage (whole lines only reach here) */
static void rx_from_tool(const char *b, size_t n)
{
    static char line[4096]; static size_t ln;
    for (size_t i = 0; i < n; i++) {
        line[ln++] = b[i];
        if (b[i] != '\n' && ln < sizeof line - 1) continue;   /* (binary frames: any 4 KiB without a newline goes on as it is) */
        size_t a = 0, e = 0;                                 /* the span [a, e) to drop */
        line[ln] = 0;
        char pat[32];
        snprintf(pat, sizeof pat, " d %ld ", opt_rx_kill_off);
        if (opt_rx_kill_n > 0 && opt_rx_kill_off >= 0 && strstr(line, pat)) { opt_rx_kill_n--; a = ln / 3; e = a + ln / 4; }
        else if (opt_rx_loss > 0 && ln > 12 && (double)(rx_rand() % 10000) < opt_rx_loss * 100.0) {
            a = rx_rand() % (ln - 2); e = a + 1 + rx_rand() % 60; if (e > ln - 1) e = ln - 1;
        }
        if (e > a) n_rx_damaged_lines++;
        for (size_t k = 0; k < ln; k++) if ((k < a || k >= e) && rx_wire_n < sizeof rx_wire) rx_wire[rx_wire_n++] = line[k];
        ln = 0;
    }
}
static size_t rx_take(char *dst, size_t cap)                 /* what the REPL has read by now */
{
    if (!opt_rx_repl) { const size_t k = rx_wire_n < cap ? rx_wire_n : cap; memcpy(dst, rx_wire, k); memmove(rx_wire, rx_wire + k, rx_wire_n - k); rx_wire_n -= k; return k; }
    rx_in_credit += 11.52;
    size_t used = 0;
    while (rx_in_credit >= 1.0 && used < rx_wire_n) {
        rx_in_credit -= 1.0;
        if (rx_q_n < sizeof rx_q) rx_q[rx_q_n++] = rx_wire[used]; else n_rx_dropped++;
        used++;
    }
    memmove(rx_wire, rx_wire + used, rx_wire_n - used); rx_wire_n -= used;
    if (!rx_wire_n && rx_in_credit > 12) rx_in_credit = 12;
    rx_out_credit += U.bin ? 1000.0 : 1.3;                  /* binary mode: read in blocks, no echo */
    size_t k = 0;
    while (rx_out_credit >= 1.0 && k < rx_q_n && k < cap) { rx_out_credit -= 1.0; dst[k] = rx_q[k]; k++; }
    memmove(rx_q, rx_q + k, rx_q_n - k); rx_q_n -= k;
    if (!rx_q_n && rx_out_credit > 2) rx_out_credit = 2;
    return k;
}

static void usb_poll(void)
{
    while (ans_h != ans_t_ && ans_q[ans_h % 64].due <= gb_ms && con_backlog() <= CON_TXBUF) {
        cf_esp_host_answer(&C, ans_q[ans_h % 64].s);
        ans_h++;
    }
    if (usb_out < 0 || !esp_is_up) return;          /* the console answers once the ESP32 runs */
    {
        char tb[4096];
        const ssize_t k = rx_wire_n + sizeof tb < sizeof rx_wire ? read(usb_out, tb, sizeof tb) : 0;
        if (k > 0) rx_from_tool(tb, (size_t)k);
    }
    usb_n += rx_take(usb_buf + usb_n, sizeof usb_buf - 1 - usb_n);
    if (cf_usb_bin_active(&U)) {                            /* the console is with the binary reader */
        cf_usb_rx(&U, (const uint8_t *)usb_buf, (uint32_t)usb_n);
        usb_n = 0;
        return;
    }
    char *nl;
    while ((nl = memchr(usb_buf, '\n', usb_n))) {
        *nl = 0;
        usb_cmds_in++;
        if (!strncmp(usb_buf, "cart px ", 8)) {
            if (ans_t_ - ans_h < 64) { snprintf(ans_q[ans_t_ % 64].s, sizeof ans_q[0].s, "%s", usb_buf + 8); ans_q[ans_t_ % 64].due = gb_ms + usb_answer_ms; ans_t_++; }
        }
        else if (!strncmp(usb_buf, "cart usb ", 9)) {
            char *av[8]; int ac = 0;
            for (char *t = strtok(usb_buf + 9, " "); t && ac < 8; t = strtok(NULL, " ")) av[ac++] = t;
            cf_usb_cmd(&U, ac, av);
        }
        else if (!strcmp(usb_buf, "cart probe")) cf_esp_arm_probe(&C, 0);
        else if (!strcmp(usb_buf, "cart probe flashid")) cf_esp_arm_probe(&C, 1);
        const size_t used = (size_t)(nl + 1 - usb_buf);
        memmove(usb_buf, nl + 1, usb_n - used);
        usb_n -= used;
    }
}

static int usb_spawn(const char *cmd)
{
    int a[2], b[2];
    if (pipe(a) || pipe(b)) return -1;
    usb_pid = fork();
    if (usb_pid == 0) {
        dup2(a[0], 0); dup2(b[1], 1);
        close(a[1]); close(b[0]);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(a[0]); close(b[1]);
    usb_in = a[1]; usb_out = b[0];
    fcntl(usb_out, F_SETFL, O_NONBLOCK);
    signal(SIGPIPE, SIG_IGN);
    return 0;
}

/* run emulated time; stop early when done() says so. Returns 1 if done() fired. */
typedef int (*until_fn)(void);
static int (*cut_when)(void);           /* power-cut trigger, checked every ms */
static int cut_fired;

static void full_power_cut(void)
{
    if (verbose) fprintf(stderr, "  %8.3f s *** POWER CUT (cart busy=%d, erase ops so far %u) ***\n", t_us / 1e6, cart_busy(&cart), cart.n_erase);
    cart_power_cut(&cart);
    cart_power_on(&cart);
    memset(&EG, 0, sizeof EG); /* the FPGA reconfigures: no engine allow, no session */
    xs_reset_all(); XS.in_h = XS.in_t = 0;   /* ... and its express stream starts from nothing */
    esp_boot();              /* the ESP32 reboots: RAM gone, the journal (NVS) stays */
    netboot_sel = 0;
    gb_reset();
    cut_fired = 1;
}

/* --usb-wait: the simulated clock stands still while the device waits for the tool's answer to
   a `want` and the wire is empty: the tool is a real process and thinks in real time, the
   simulation does not. With it the simulated seconds are the device's and the cable's own
   (what the transfer costs on a unit), not the tool's reaction time. */
static int opt_usb_wait;
static double usb_stall_real_ms;
static int usb_waiting_for_tool(void)
{
    if (!opt_usb_wait || usb_out < 0 || usb_pid <= 0) return 0;
    if (!U.want_open || U.have || rx_wire_n || rx_q_n || usb_n || U.rx_n) return 0;
    char tb[4096];
    const ssize_t k = read(usb_out, tb, sizeof tb);
    if (k > 0) { rx_from_tool(tb, (size_t)k); usb_stall_real_ms = 0; return 0; }
    if (usb_stall_real_ms > 3000.0) return 0;                 /* the tool is not answering: let the clock run (want is repeated) */
    usleep(200);
    usb_stall_real_ms += 0.2;
    return 1;
}

static int run(double max_s, until_fn done)
{
    const double stop = t_us + max_s * 1e6;
    while (t_us < stop) {
        if (usb_waiting_for_tool()) { if (done && done()) return 1; continue; }
        const unsigned cyc = GB_run(gb);
        t_us += cyc / 8.388608;
        xs_run(t_us);
        cart_time(&cart, t_us);
        eg_step();
        if (C.wp_done) wp_seen_done = 1;
        if (cut_when && !cut_fired && cut_when()) { full_power_cut(); return 2; }
        while (t_us >= next_ms_us) {
            next_ms_us += 1000.0;
            step_ms();
            usb_poll();
        }
        if (want_cycle) {
            want_cycle = 0;
            n_cycles++;
            cart_power_on(&cart);    /* the cart is powered off ~1 s and on again */
            gb_reset();
        }
        if (done && done()) return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- images */
static const uint8_t k_logo[48] = {
    0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B, 0x03, 0x73, 0x00, 0x83, 0x00, 0x0C, 0x00, 0x0D,
    0x00, 0x08, 0x11, 0x1F, 0x88, 0x89, 0x00, 0x0E, 0xDC, 0xCC, 0x6E, 0xE6, 0xDD, 0xDD, 0xD9, 0x99,
    0xBB, 0xBB, 0x67, 0x63, 0x6E, 0x0E, 0xEC, 0xCC, 0xDD, 0xDC, 0x99, 0x9F, 0xBB, 0xB9, 0x33, 0x3E,
};

/* a bootable ROM: header + "write marker to FFF0, loop"; the rest pseudo-random data */
static uint8_t *make_rom(uint32_t size, const char *title, uint8_t type, uint8_t marker, uint32_t seed)
{
    uint8_t *r = malloc(size);
    uint32_t x = seed;
    for (uint32_t i = 0; i < size; i++) { x = x * 1103515245u + 12345u; r[i] = (uint8_t)(x >> 16); }
    for (uint32_t i = 0; i < size; i += 0x1000) if ((i / 0x1000) % 7 == 3) memset(r + i, 0xFF, 0x200);   /* some FF runs */
    static const uint8_t code[] = { 0x00, 0xC3, 0x50, 0x01 };
    memcpy(r + 0x100, code, sizeof code);
    memcpy(r + 0x104, k_logo, sizeof k_logo);
    memset(r + 0x134, 0, 0x4C - 0x34 + 1);
    memcpy(r + 0x134, title, strlen(title) < 15 ? strlen(title) : 15);
    r[0x143] = 0x80;                                     /* CGB */
    r[0x147] = type;
    int sb = 0; while ((0x8000u << sb) < size) sb++;
    r[0x148] = (uint8_t)sb;
    r[0x149] = 0x03;
    uint8_t h = 0;
    for (int i = 0x134; i <= 0x14C; i++) h = (uint8_t)(h - r[i] - 1);
    r[0x14D] = h;
    const uint8_t main_[] = { 0x3E, marker, 0xE0, 0xF0, 0x18, 0xFE };   /* ld a,m; ldh [$F0],a; jr @ */
    memcpy(r + 0x150, main_, sizeof main_);
    uint16_t g = 0;
    for (uint32_t i = 0; i < size; i++) if (i != 0x14E && i != 0x14F) g = (uint16_t)(g + r[i]);
    r[0x14E] = (uint8_t)(g >> 8); r[0x14F] = (uint8_t)g;
    return r;
}

/* ---------------------------------------------------------------- checks */
static int fails;
static void check(int ok, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("  %s ", ok ? "ok  " : "FAIL");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    if (!ok) fails++;
}

static uint8_t *orig; static uint32_t orig_used = 0x40000;   /* "Tetris": 256 KiB on a 2 MiB chip */
static uint8_t orig_ram[CF_RAM_SIZE];
static uint8_t *orig_chip;                                    /* the whole original chip */

static void setup(const cart_cfg_t *k0, int with_recovery)
{
    cart_cfg_t kk = *k0;
    if (opt_chip1681 && kk.id16[0] == 0xBF && kk.id16[1] == 0xC9) {
        kk.id16[1] = 0xC8;
        kk.protect_top = 0;
    }
    if (kk.id16[0] == 0xBF && kk.id16[1] == 0xC8) { kk.protect_bottom = opt_wp_low ? 0x10000u : 0; kk.garble_ignored = opt_wp_garble; }
    if (opt_ram_kib) kk.ram_kib = opt_ram_kib;
    if (opt_ram_timeout_ms > 0) kk.ram_timeout_us = opt_ram_timeout_ms * 1000.0;
    kk.gb_writes_flash = opt_gb_writes_flash;
    memset(&EG, 0, sizeof EG);
    const cart_cfg_t *k = &kk;
    orig = make_rom(orig_used, "SIM TETRIS", 0x1B, 0x77, 1);
    orig_chip = malloc(k->size);
    memset(orig_chip, 0xFF, k->size);
    memcpy(orig_chip, orig, orig_used);
    if (with_recovery) {                                      /* a "recovery sector" near the end (ModRetro CLI hint) */
        for (uint32_t i = 0; i < 0x1000; i++) orig_chip[k->size - 0x10000 + i] = (uint8_t)(i * 7 + 3);
    }
    for (uint32_t i = 0; i < CF_RAM_SIZE; i++) orig_ram[i] = (uint8_t)((i * 13 + 5) ^ ((i >> 13) * 0x55) ^ (i >> 8));   /* differs per bank and per page */
    cart_init(&cart, k, orig_chip, k->size, orig_ram);
    bak_cap = k->size;
    bak = calloc(1, bak_cap);
    memset(ram_bak, 0, sizeof ram_bak);
    journal_valid = 0;
    img = make_rom(0x20000, "SHINYGEN GAME", 0x1B, 0x42, 99);   /* a Shiny Gen game: 128 KiB MBC5+RAM+BATT */
    img_size = 0x20000;
    esp_boot();
    boot();
    t_us = 0; next_ms_us = 1000; gb_ms = 0;
}

static int job_over(void) { return !C.armed && !C.done_pending && C.st.result != 0xEE && n_cycles > 0 && game_marker >= 0; }
static int job_stopped(void) { return !C.armed && !C.done_pending && C.ph == CF_PH_DONE; }

static int arm(uint8_t job)
{
    cf_job_t j = { .job = job, .allow_unverified = 1, .ram_clear = 1, .max_retries = 2 };
    C.st.result = 0xEE;
    n_cycles = 0;
    return cf_esp_arm(&C, &j);
}

static int flash_eq(const uint8_t *want, uint32_t off, uint32_t n) { return memcmp(cart.flash + off, want, n) == 0; }

static void report_stats(void)
{
    printf("  stats: %.1f s emulated, writer cmds %u, erases %u, programmed %u bytes, read %u bytes, retries %u, dup answers %u,"
           " chip: prog %u erase %u busy-writes %u stray %u autoselect %u, UART drops %u\n",
           t_us / 1e6, C.n_cmds, C.n_erase, C.n_program_bytes, C.n_read_bytes, C.n_retries, C.n_dup,
           cart.n_prog, cart.n_erase, cart.n_busy_writes, cart.n_stray, cart.n_autoselect, n_dropped);
    if (opt_xs) printf("  express: route %s, fpga ops %u, posts %u (refused %u), status %u (lost %u), fw restarts %u, blocks verified %u, programmed %u bytes\n",
                       C.xs && C.xs->on ? "express" : "classic", XS.n_ops, XS.n_posts, XS.n_naks, XS.n_status, XS.n_lost, C.xs ? (unsigned)C.xs->n_resync : 0u, C.xs ? (unsigned)C.xs->verified : 0u, XS.n_prog_bytes);
}

/* install O -> A and check everything */
static void expect_installed(void)
{
    check(C.st.result == CF_D_OK, "job result OK (got %u)", C.st.result);
    check(flash_eq(img, 0, img_size), "flash[0..128K) == the new game");
    check(flash_eq(orig_chip + 0x20000, 0x20000, cart.cfg.size - 0x20000), "flash past the game untouched (old data + recovery sector)");
    check(journal.bak_ok && !memcmp(bak, orig_chip, cart.cfg.size), "backup == the whole original chip");
    uint32_t want_map = 0;
    for (int k = 0; k < 32; k++) if (!cart.cfg.ram_kib || (k % 8) < cart.cfg.ram_kib) want_map |= 1u << k;
    int bak_ok = 1, zero = 1;
    for (unsigned i = 0; i < CF_RAM_SIZE; i++) {
        const int present = (want_map >> (i / 1024)) & 1;
        if (present && ram_bak[i] != orig_ram[i]) bak_ok = 0;
        if (!present && ram_bak[i] != 0xFF) bak_ok = 0;      /* a floating KiB is never a backup */
        if (present && cart.ram[i]) zero = 0;
    }
    check(journal.ram_present == want_map, "save RAM present map %08x (expected %08x)", journal.ram_present, want_map);
    check(bak_ok, "save RAM backup == the original on every present KiB, FF (not stored) elsewhere");
    check(zero, "save RAM cleared for the new game (present KiBs)");
    check(game_marker == 0x42, "after the cart power cycle the console boots the NEW game (marker %02x)", game_marker & 0xFF);
    check(cart.n_busy_writes == 0, "no bus write while the chip was busy");
    check(journal.phase == CF_PH_IDLE && journal.job == CF_JOB_NONE, "journal: idle, nothing pending");
    check(!netboot_sel, "boot image select back to stock");
}

/* ---------------------------------------------------------------- scenarios */
static int sc_install(void)
{
    cart_cfg_t k; cart_cfg_sst39vf1682(&k);
    setup(&k, 1);
    check(arm(CF_JOB_INSTALL) == 0, "armed INSTALL");
    run(900, job_over);
    expect_installed();
    report_stats();
    return 0;
}

static int cut_target_n;
static int cut_backup(void) { return C.ph == CF_PH_BAK_ROM && C.unit >= 0x30000; }
static int cut_erase(void) { return cart_busy(&cart) && cart.busy_kind == 2 && (int)cart.n_erase == cut_target_n; }
static int cut_header(void) { return cart_busy(&cart) && cart.busy_kind == 2 && cart.busy_addr == 0; }
static int cut_program(void) { return cart_busy(&cart) && cart.busy_kind == 1 && (int)cart.n_prog == cut_target_n; }

static int sc_cut(int (*when)(void), const char *what)
{
    cart_cfg_t k; cart_cfg_sst39vf1682(&k);
    setup(&k, 1);
    check(arm(CF_JOB_INSTALL) == 0, "armed INSTALL");
    cut_when = when; cut_fired = 0;
    run(900, job_over);
    check(cut_fired, "power cut injected %s", what);
    cut_when = NULL;
    uint8_t job = 0, ph = 0;
    const int pend = cf_esp_pending(&C, &job, &ph);
    printf("  after the cut: journal job %u phase %u pending %d\n", job, ph, pend);
    run(5, NULL);            /* the console comes back; the stub finds no job; whatever is on the cart boots (or not) */
    /* the user is asked on the OSD and says "resume" */
    check(arm(CF_JOB_INSTALL) == 0, "re-armed INSTALL after the cut");
    run(900, job_over);
    expect_installed();
    report_stats();
    return 0;
}

static int sc_verify_fail(void)
{
    cart_cfg_t k; cart_cfg_sst39vf1682(&k);
    setup(&k, 0);
    /* a bit that is 1 in the new game but stuck at 0, and 0 in the original (so restore works) */
    uint32_t a = 0x14000;
    while (!((img[a] & ~orig_chip[a]) & 0xFF)) a++;
    const uint8_t bit = (uint8_t)(img[a] & ~orig_chip[a] & -(img[a] & ~orig_chip[a]));
    cart.has_stuck = 1; cart.stuck_addr = a; cart.stuck0 = bit;
    printf("  stuck-at-0 bit %02x at %06x\n", bit, a);
    check(arm(CF_JOB_INSTALL) == 0, "armed INSTALL");
    run(900, job_stopped);
    check(C.st.result == CF_D_FAILED, "job FAILED (got %u)", C.st.result);
    check(journal.phase == CF_PH_FAILED && journal.job == CF_JOB_INSTALL, "journal: FAILED, the OSD offers retry / restore");
    check(journal.bak_ok && !memcmp(bak, orig_chip, k.size), "backup intact");
    check(C.n_retries >= 2, "retried the sector before giving up (%u retries)", C.n_retries);
    run(3, NULL);
    check(arm(CF_JOB_RESTORE) == 0, "armed RESTORE");
    run(900, job_over);
    check(C.st.result == CF_D_OK, "restore OK");
    check(flash_eq(orig_chip, 0, k.size), "flash == the original chip again (all 2 MiB)");
    check(!memcmp(cart.ram, orig_ram, CF_RAM_SIZE), "FRAM == the original save");
    check(game_marker == 0x77, "the original game boots (marker %02x)", game_marker & 0xFF);
    report_stats();
    return 0;
}

static int sc_refused(const cart_cfg_t *k, int allow, const char *why, uint8_t want)
{
    setup(k, 0);
    cf_job_t j = { .job = CF_JOB_INSTALL, .allow_unverified = (uint8_t)allow, .ram_clear = 1 };
    C.st.result = 0xEE; n_cycles = 0;
    check(cf_esp_arm(&C, &j) == 0, "armed INSTALL (%s)", why);
    run(300, job_stopped);
    check(C.st.result == want, "refused with code %u (got %u)", want, C.st.result);
    check(cart.n_erase == 0 && cart.n_prog == 0 && cart.n_chip_erase == 0, "no erase / program command reached the chip");
    check(flash_eq(orig_chip, 0, k->size), "flash unchanged");
    check(!memcmp(cart.ram, orig_ram, CF_RAM_SIZE), "FRAM unchanged");
    check(!journal.bak_ok, "no backup taken (nothing to protect)");
    check(journal.phase == CF_PH_IDLE, "journal idle: the cart is consistent");
    run(10, NULL);
    check(game_marker == 0x77, "the original game still boots (marker %02x)", game_marker & 0xFF);
    report_stats();
    return 0;
}

static int sc_roundtrip(void)
{
    cart_cfg_t k; cart_cfg_is29gl032(&k);       /* the 4 MiB chip, geometry from CFI */
    setup(&k, 1);
    check(arm(CF_JOB_INSTALL) == 0, "armed INSTALL (IS29GL032, CFI geometry)");
    run(1800, job_over);
    expect_installed();
    run(3, NULL);
    check(arm(CF_JOB_RESTORE) == 0, "armed RESTORE");
    run(1800, job_over);
    check(C.st.result == CF_D_OK, "restore OK");
    check(flash_eq(orig_chip, 0, k.size), "flash == the original chip (all 4 MiB)");
    check(!memcmp(cart.ram, orig_ram, CF_RAM_SIZE), "FRAM == the original save");
    check(game_marker == 0x77, "the original game boots again (marker %02x)", game_marker & 0xFF);
    check(journal.bak_ok, "the backup is kept after a restore");
    report_stats();
    return 0;
}

static int sc_header_mapper(void)
{
    /* hypothesis variant: the cart FPGA picks the mapper from 0x147 at power-on; the
       original is MBC5 (as the research says ModRetro carts behave); the power cut lands in
       the header sector's erase, so the next power-on sees 0x147 = FF (model: MBC5). */
    cart_cfg_t k; cart_cfg_sst39vf1682(&k);
    k.mapper = CM_HEADER;
    setup(&k, 0);
    check(arm(CF_JOB_INSTALL) == 0, "armed INSTALL (header-chosen mapper)");
    cut_when = cut_header; cut_fired = 0;
    run(900, job_over);
    cut_when = NULL;
    check(cut_fired, "power cut during the header sector erase");
    run(5, NULL);
    printf("  mapper after the cut: %s (0x147 = %02x)\n", cart_mk_name(cart.mk), cart.flash[0x147]);
    check(arm(CF_JOB_INSTALL) == 0, "re-armed");
    run(900, job_over);
    expect_installed();
    check(cart.mk == MK_MBC5, "mapper after the final power cycle: %s", cart_mk_name(cart.mk));
    report_stats();
    return 0;
}

static int sc_esp_reboot(void)
{
    /* the ESP32 crashes mid-write; the GB keeps running the writer */
    cart_cfg_t k; cart_cfg_sst39vf1682(&k);
    setup(&k, 0);
    check(arm(CF_JOB_INSTALL) == 0, "armed INSTALL");
    while (!(C.ph == CF_PH_WRITE && C.oi >= 1)) if (!run(1, NULL) && t_us > 900e6) break;
    printf("  %8.3f s ESP32 reboot (writer keeps running)\n", t_us / 1e6);
    esp_boot();
    run(20, NULL);
    check(C.ph == CF_PH_DONE || !C.armed, "unarmed ESP32 answers the writer with DONE(NOJOB): it halts, nothing written");
    check(cf_esp_pending(&C, NULL, NULL), "journal says a write is pending");
    check(arm(CF_JOB_INSTALL) == 0, "re-armed (OSD: resume)");
    run(900, job_over);
    expect_installed();
    report_stats();
    return 0;
}

static int sc_ram_partial(void)
{
    /* only 1 KiB of each 8 KiB RAM bank is memory (one reading of unit 1's probe) */
    cart_cfg_t k; cart_cfg_sst39vf1682(&k);
    opt_ram_kib = 1;
    setup(&k, 0);
    check(arm(CF_JOB_INSTALL) == 0, "armed INSTALL (1 KiB of save RAM per bank)");
    run(900, job_over);
    expect_installed();
    run(3, NULL);
    check(arm(CF_JOB_RESTORE) == 0, "armed RESTORE");
    run(900, job_over);
    check(C.st.result == CF_D_OK, "restore OK (no RAMWRITE into floating KiBs, which would never verify)");
    check(flash_eq(orig_chip, 0, k.size), "flash == the original");
    check(!memcmp(cart.ram, orig_ram, CF_RAM_SIZE), "save RAM == the original");
    check(game_marker == 0x77, "the original boots (marker %02x)", game_marker & 0xFF);
    report_stats();
    return 0;
}

/* ---- the engine path (fpga cartflash1-engine) on unit 1's chip, as the hardware steps will run */
static int arm_job(uint8_t job, int allow_unverified)
{
    cf_job_t j = { .job = job, .allow_unverified = (uint8_t)allow_unverified, .ram_clear = 0, .max_retries = 2 };
    C.st.result = 0xEE; n_cycles = 0;
    return cf_esp_arm(&C, &j);
}
static int cut_rt_header(void) { return cart_busy(&cart) && cart.busy_kind == 2 && cart.busy_addr == 0 && journal.job == CF_JOB_ROUNDTRIP; }
static int sc_engine_job(const char *which)
{
    cart_cfg_t k; cart_cfg_sst39vf1682(&k);
    setup(&k, 1);                                /* data in the protected top block too */
    const int seq = !strcmp(which, "hw_sequence");
    if (!strcmp(which, "selftest") || seq) {
        check(arm_job(CF_JOB_SELFTEST, 0) == 0, "armed SELFTEST (SST39VF1682 verified: no allow_unverified)");
        run(900, job_over);
        check(C.st.result == CF_D_OK, "selftest OK (got %u)", C.st.result);
        check(EG.n_progs >= 4000 && EG.n_erases == 1, "engine: %u byte programs, %u erase (the 4 KiB pattern, one block back to FF)", EG.n_progs, EG.n_erases);
        check(flash_eq(orig_chip, 0, k.size), "flash == the original chip after the selftest (the unit is blank again)");
        check(journal.bak_ok && !memcmp(bak, orig_chip, k.size), "backup of the whole chip taken first");
        check(game_marker == 0x77, "Tetris boots after the selftest (marker %02x)", game_marker & 0xFF);
        run(3, NULL);
    }
    if (!strcmp(which, "rt_tetris") || seq) {
        const unsigned e0 = cart.n_erase;
        check(arm_job(CF_JOB_ROUNDTRIP, 0) == 0, "armed ROUNDTRIP");
        if (seq) { cut_when = cut_rt_header; cut_fired = 0; }
        run(900, job_over);
        if (seq) {
            cut_when = NULL;
            check(cut_fired, "power cut during the round trip's header block erase");
            run(5, NULL);
            check(cf_esp_pending(&C, NULL, NULL), "journal: write pending after the cut");
            check(arm_job(CF_JOB_ROUNDTRIP, 0) == 0, "re-armed ROUNDTRIP (resume)");
            run(900, job_over);
        }
        check(C.st.result == CF_D_OK, "round trip OK (got %u)", C.st.result);
        check(cart.n_erase - e0 >= 4, "the 4 non-blank 64 KiB blocks of the game erased and reprogrammed (%u erases)", cart.n_erase - e0);
        check(flash_eq(orig_chip, 0, k.size), "flash == the original chip, byte for byte");
        check(!memcmp(cart.ram, orig_ram, CF_RAM_SIZE), "save RAM untouched by the round trip");
        check(game_marker == 0x77, "Tetris boots after the round trip (marker %02x)", game_marker & 0xFF);
    }
    check(cart.n_prot_ignored == 0, "the protected top block was never targeted");
    check(cart.n_busy_writes == 0, "no bus write while the chip was busy");
    check(!EG.allow && !EG.session, "engine allow off after the job");
    report_stats();
    return 0;
}

/* ---- cart RAM through the engine (2026-10-11): all four banks, a cut in the RAM phase, and the
   boot ROM's write blocking that hid the truth on hardware */
static int cut_ram_phase(void) { return C.ph == CF_PH_RAM && C.pos >= 0x3000 && journal.job == CF_JOB_RESTORE; }
static int sc_ram_engine(int with_cut)
{
    cart_cfg_t k; cart_cfg_sst39vf1682(&k);
    setup(&k, 0);
    const unsigned b0 = n_boot_blocked;
    check(arm(CF_JOB_INSTALL) == 0, "armed INSTALL (RAM backup + clear through the engine)");
    run(900, job_over);
    expect_installed();
    int banks = 1;
    for (int b = 0; b < 4; b++) if (memcmp(ram_bak + b * 0x2000, orig_ram + b * 0x2000, 0x2000)) banks = 0;
    check(banks, "RAM backup: each of the 4 banks holds its own (bank-distinct) pattern");
    /* the new game saves: a few bytes in bank 2, as a G5 slot would */
    memcpy(cart.ram + 0x4000, "SGSV\x3c\x00\x45\xa4", 8);
    run(3, NULL);
    check(arm(CF_JOB_RESTORE) == 0, "armed RESTORE");
    if (with_cut) { cut_when = cut_ram_phase; cut_fired = 0; }
    run(900, job_over);
    if (with_cut) {
        cut_when = NULL;
        check(cut_fired, "power cut in the middle of the RAM restore (bank 1)");
        int part = 0;
        for (unsigned i = 0; i < CF_RAM_SIZE; i++) if (cart.ram[i] != orig_ram[i]) part++;
        check(part > 0, "after the cut the RAM is part old, part new (%d bytes still differ)", part);
        run(5, NULL);
        check(arm(CF_JOB_RESTORE) == 0, "re-armed RESTORE after the cut");
        run(900, job_over);
    }
    check(C.st.result == CF_D_OK, "restore OK (got %u)", C.st.result);
    check(flash_eq(orig_chip, 0, k.size), "flash == the original chip");
    check(!memcmp(cart.ram, orig_ram, CF_RAM_SIZE), "FRAM == the original save, all 4 banks, byte for byte");
    check(!cart.ram_en, "RAM disabled after the job");
    check(game_marker == 0x77, "the original game boots (marker %02x)", game_marker & 0xFF);
    check(n_boot_blocked == b0, "no cart write of the whole run was aimed under the mapped boot ROM (%u blocked)", n_boot_blocked - b0);
    report_stats();
    return 0;
}

/* a game's save on its own: take it, clear the cart's RAM, put it back; and the RAM pattern test */
static int cut_save_put(void) { return C.ph == CF_PH_SAVE && journal.job == CF_JOB_SAVE_PUT && C.pos >= 0x5000; }
static int sc_save_rt(void)
{
    cart_cfg_t k; cart_cfg_sst39vf1682(&k);
    setup(&k, 0);
    memset(sav2, 0xEE, sizeof sav2);
    check(arm_job(CF_JOB_RAMTEST, 0) == 0, "armed RAMTEST");
    run(300, job_over);
    check(C.st.result == CF_D_OK && C.ramtest_ok, "ramtest PASS (result %u)", C.st.result);
    check(!memcmp(cart.ram, orig_ram, CF_RAM_SIZE), "RAM == the original after the test (the KiB got its bytes back)");
    run(3, NULL);
    check(arm_job(CF_JOB_SAVE_GET, 0) == 0, "armed SAVE_GET");
    run(300, job_over);
    check(C.st.result == CF_D_OK && !memcmp(sav2, orig_ram, CF_RAM_SIZE), "the save store == the cart's RAM, all 4 banks");
    check(C.save_crc == cf_crc32(0, orig_ram, CF_RAM_SIZE), "its crc32 is reported");
    check(!journal.bak_ok, "the cart backup was not touched (none stored)");
    run(3, NULL);
    check(arm_job(CF_JOB_SAVE_CLEAR, 0) == 0, "armed SAVE_CLEAR");
    run(300, job_over);
    int z = 1; for (unsigned i = 0; i < CF_RAM_SIZE; i++) if (cart.ram[i]) z = 0;
    check(C.st.result == CF_D_OK && z, "the cart's RAM is all 00");
    run(3, NULL);
    check(arm_job(CF_JOB_SAVE_PUT, 0) == 0, "armed SAVE_PUT");
    cut_when = cut_save_put; cut_fired = 0;
    run(300, job_over);
    cut_when = NULL;
    check(cut_fired, "power cut in the middle of SAVE_PUT (bank 2)");
    run(5, NULL);
    check(!cf_esp_pending(&C, NULL, NULL), "a cut save job leaves no pending flash write in the journal");
    check(arm_job(CF_JOB_SAVE_PUT, 0) == 0, "armed SAVE_PUT again");
    run(300, job_over);
    check(C.st.result == CF_D_OK && !memcmp(cart.ram, orig_ram, CF_RAM_SIZE), "the cart's RAM == the save, byte for byte");
    check(flash_eq(orig_chip, 0, k.size), "the flash was never written");
    check(game_marker == 0x77, "the game boots (marker %02x)", game_marker & 0xFF);
    report_stats();
    return 0;
}

/* ---------------------------------------------------------------- the streamed install (2026-10-11):
   straight from the source to the cart, nothing staged or backed up on the ESP32 */
#include "../cf_sha256.h"
static int arm_stream(const char *id, int src)
{
    cf_job_t j;
    memset(&j, 0, sizeof j);
    j.job = CF_JOB_STREAM; j.ram_clear = 1; j.max_retries = 2;
    j.strm_src = (uint8_t)src; j.strm_chunk = (src && src != CF_SRC_WIFI_STAGED) ? CF_USB_CHUNK : CF_PROG_CHUNK;
    j.fast_verify = src == CF_SRC_WIFI_STAGED;      /* wifispeed1: the Wi-Fi image downloaded + hashed into the staging area */
    j.xs_timing = (uint8_t)opt_xs_timing;
    j.strm_size = net_size;
    j.strm_crc32 = cf_crc32(0, net_img, net_size);
    cf_sha256_t x; cf_sha256_init(&x); cf_sha256_update(&x, net_img, net_size); cf_sha256_final(&x, j.strm_sha);
    j.strm_have_sha = 1;
    snprintf(j.strm_id, sizeof j.strm_id, "%s", id);
    cf_title_from_header(net_img + 0x100, j.strm_title);
    C.st.result = 0xEE; n_cycles = 0;
    return cf_esp_arm(&C, &j);
}
static int is1681(void) { return cart.cfg.id16[0] == 0xBF && cart.cfg.id16[1] == 0xC8; }
static int cart_is_image(uint32_t chip_size)
{
    const uint32_t w = chip_size - (is1681() ? 0u : 0x10000u), e = net_size < w ? net_size : w;
    if (memcmp(cart.flash, net_img, e)) { for (uint32_t i = 0; i < e; i++) if (cart.flash[i] != net_img[i]) { printf("  (first difference at %06x: cart %02x image %02x)\n", (unsigned)i, cart.flash[i], net_img[i]); break; } return 0; }
    for (uint32_t i = e; i < w; i++) if (cart.flash[i] != 0xFF) return 0;       /* nothing of the old game is left */
    return 1;
}
static int no_logo(void) { for (int i = 0x104; i < 0x134; i++) if (cart.flash[i] != 0xFF) return 0; return 1; }
static void stream_setup(cart_cfg_t *k, uint32_t size, uint8_t type, int with_top)
{
    cart_cfg_sst39vf1682(k);
    setup(k, with_top);
    net_img = make_rom(size <= 0x40000 ? 0x40000 : size <= 0x100000 ? 0x100000 : 0x200000, "STREAM TEST", type, 0x42, 4242);
    net_size = size;
    net_down = net_gone = 0; net_flip_at = -1; net_chunks = net_bytes = net_asks = 0; net_lag = 3; net_w_off = 0xFFFFFFFFu;
}
static void stream_expect_ok(const cart_cfg_t *k)
{
    check(C.st.result == CF_D_OK, "streamed install OK (result %u)", C.st.result);
    check(cart_is_image(k->size), "the cart holds the image, and FF from its end to the writable end");
    if (!is1681()) {
        check(!memcmp(cart.flash + k->size - 0x10000, orig_chip + k->size - 0x10000, 0x10000), "the protected top block untouched");
        check(C.wp_scan == 0 && C.wp_step == 0 && cart.n_prot_bottom_ignored == 0, "SST39VF1682: no write test (it has no protected bottom block)");
    } else {
        check(cart.n_prot_bottom_ignored == 0, "SST39VF1681, WP# high: no write was ignored");
    }
    int z = 1; for (unsigned i = 0; i < CF_RAM_SIZE; i++) if (cart.ram[i]) z = 0;
    check(z, "the save RAM is cleared (the game and its save are replaced)");
    check(game_marker == want_marker, "the new game boots (marker %02x)", game_marker & 0xFF);
    check(journal.phase == CF_PH_IDLE && journal.job == CF_JOB_NONE && !journal.bak_ok, "journal idle; no backup was ever taken on the ESP32");
    check(cart.n_busy_writes == 0, "no bus write while the chip was busy");
}
static int strm_cut_erase(void) { return cart_busy(&cart) && cart.busy_kind == 2 && cart.n_erase == 3; }
static int strm_cut_blk0(void) { return C.ph == CF_PH_STREAM && C.sp == 7 /* SP_BLK0 */ && C.pos >= 0x4000 && cart_busy(&cart) && cart.busy_kind == 1; }
static int strm_at_blk2(void) { return C.ph == CF_PH_STREAM && C.sp == 2 /* SP_BLOCKS */ && C.s_blk == 2 && C.pos > 0x24000; }
static int xs_cut_prog(void) { return C.xs && C.xs->on && C.xs->verified >= 5 && cart_busy(&cart) && cart.busy_kind == 1; }   /* mid-program, block 6 */
static int strm_in_hash(void) { return C.ph == CF_PH_STREAM && C.sp == 4 /* SP_HASH0 */ && C.pos >= 0x2000; }

static int sc_stream(const char *which)
{
    cart_cfg_t k;
    if (!strcmp(which, "strm_ok")) {
        stream_setup(&k, 0x30000u + 123u, 0x13, 1);         /* a foreign ROM, 196,731 bytes, data in the top block */
        const unsigned b0 = n_boot_blocked;
        check(arm_stream("0123456789abcdef0123456789abcdef", 0) == 0, "armed STREAM (gateway), 196,731 bytes");
        run(1800, job_over);
        stream_expect_ok(&k);
        check(net_bytes >= net_size + 0x10000u - 0x10000u && net_bytes <= net_size + 0x10000u + 4096u, "the source gave the image once + its first block again for the hash (%u bytes for %u)", net_bytes, net_size);
        check(n_boot_blocked == b0, "no write aimed under the mapped boot ROM");
    } else if (!strcmp(which, "strm_drop")) {
        stream_setup(&k, 0x80000, 0x1B, 0);
        check(arm_stream("11111111111111111111111111111111", 0) == 0, "armed STREAM, 512 KiB");
        run(600, strm_at_blk2); net_down = 1; check(1, "link down in the middle of block 2");
        run(40, NULL); check(C.armed && C.st.result == 0xEE, "the job waits (40 s without data)");
        net_down = 0; run(600, strm_in_hash); net_down = 1; check(1, "link down again in the hash pass");
        run(25, NULL); net_down = 0;
        run(1800, job_over);
        stream_expect_ok(&k);
        check(net_bytes <= net_size + 0x10000u + 4096u, "no byte was fetched twice because of the drops (%u for %u)", net_bytes, net_size);
    } else if (!strcmp(which, "strm_pause")) {
        stream_setup(&k, 0x80000, 0x1B, 0);
        check(arm_stream("22222222222222222222222222222222", 0) == 0, "armed STREAM, 512 KiB");
        run(600, strm_at_blk2); net_down = 1;
        run(400, job_stopped);
        check(C.st.result == CF_D_PAUSED, "5 minutes without data: paused (result %u)", C.st.result);
        check(cf_esp_stream_pending(&C, NULL, NULL, NULL) && no_logo(), "unfinished in the journal; the cart has no logo (the console will not start it)");
        game_marker = -1;
        run(8, NULL);
        check(game_marker != 0x42 && game_marker != 0x77, "and indeed neither the old nor the new game starts from it (the CPU runs FF bytes: rst 38 forever)");
        check(arm_stream("ffffffffffffffffffffffffffffffff", 0) != 0, "another image is refused while this one is unfinished");
        const unsigned c0 = net_bytes;
        net_down = 0;
        check(arm_stream("22222222222222222222222222222222", 0) == 0, "the same image re-arms (resume)");
        run(1800, job_over);
        stream_expect_ok(&k);
        check(net_bytes - c0 <= net_size - 0x10000u + 0x10000u + 4096u, "the resume asked only for what was missing (%u more bytes)", net_bytes - c0);
    } else if (!strcmp(which, "strm_cut_erase") || !strcmp(which, "strm_cut_blk0")) {
        stream_setup(&k, 0x80000, 0x1B, 0);
        check(arm_stream("33333333333333333333333333333333", 0) == 0, "armed STREAM, 512 KiB");
        cut_when = !strcmp(which, "strm_cut_erase") ? strm_cut_erase : strm_cut_blk0; cut_fired = 0;
        run(1800, job_over);
        cut_when = NULL;
        check(cut_fired, !strcmp(which, "strm_cut_erase") ? "power cut in the middle of a block erase" : "power cut while the first block was being written");
        game_marker = -1;
        run(8, NULL);
        check(cf_esp_stream_pending(&C, NULL, NULL, NULL), "after the restart: unfinished in the journal");
        check(no_logo() && game_marker != 0x42 && game_marker != 0x77, "the half-written cart has no logo; neither the old nor the new game starts from it");
        check(arm_stream("33333333333333333333333333333333", 0) == 0, "re-armed (resume)");
        run(1800, job_over);
        stream_expect_ok(&k);
    } else if (!strcmp(which, "strm_verify")) {
        stream_setup(&k, 0x80000, 0x1B, 0);
        net_flip_at = 0x2ABCD;                                /* one byte wrong in transit, block 2 */
        check(arm_stream("44444444444444444444444444444444", 0) == 0, "armed STREAM, 512 KiB, one corrupted byte on the way");
        run(1800, job_stopped);
        check(C.st.result == CF_D_VERIFY, "the read-back does not match: VERIFY (result %u)", C.st.result);
        check(no_logo(), "the first block was never written: the cart does not start");
        check(cf_esp_stream_pending(&C, NULL, NULL, NULL) && journal.strm_tries == 1, "unfinished, 1 failed check on record");
        run(5, NULL);
        check(arm_stream("44444444444444444444444444444444", 0) == 0, "re-armed");
        run(1800, job_over);
        stream_expect_ok(&k);
        /* and a wrong byte in the FIRST block, delivered after the hash pass */
        run(3, NULL);
        net_img[0x20000] ^= 1;                                /* a new image (so not "the same again") */
        net_flip_at = 0x3000;
        net_lag = 3;
        C.s_have = 0;
        check(arm_stream("55555555555555555555555555555555", 0) == 0, "armed another image; its first block will arrive corrupted once (after the hash)");
        { int n = 0; for (;;) { if (run(1800, job_stopped)) break; if (++n > 2) break; } }
        check(C.st.result == CF_D_OK || C.st.result == CF_D_VERIFY, "ended OK or VERIFY (result %u)", C.st.result);
        if (C.st.result == CF_D_VERIFY) check(no_logo(), "VERIFY: the first block was erased again, the cart does not start");
        else check(cart_is_image(k.size), "OK: the flip landed in the hash pass instead, where it cannot be written");
    } else if (!strcmp(which, "strm_gone")) {
        stream_setup(&k, 0x80000, 0x1B, 0);
        check(arm_stream("66666666666666666666666666666666", 0) == 0, "armed STREAM");
        run(600, strm_at_blk2); net_gone = 1;
        run(60, job_stopped);
        check(C.st.result == CF_D_GONE && cf_esp_stream_pending(&C, NULL, NULL, NULL), "the server forgot the image: GONE, unfinished");
        cf_esp_stream_cancel(&C);
        check(!cf_esp_stream_pending(&C, NULL, NULL, NULL) && !cf_esp_pending(&C, NULL, NULL), "cancelled: the journal is idle");
        check(no_logo(), "the cart stays without a first block (the owner restores it over USB)");
        net_gone = 0;
        net_img[0x1000] ^= 0x55;
        check(arm_stream("77777777777777777777777777777777", 0) == 0, "a new image can be sent now");
        run(1800, job_over);
        stream_expect_ok(&k);
    } else if (!strcmp(which, "strm_xs_shrink")) {
        /* cartspeed1: a 256 KiB image on a cart full of a 2 MiB one: the tail blocks are read, found
           not blank, erased once each and read back blank */
        stream_setup(&k, 0x40000, 0x1B, 0);
        for (uint32_t i = 0x40000; i < 0x1F0000; i++) cart.flash[i] = (uint8_t)(i * 5 + 1);
        memset(stg_rom, 0xFF, sizeof stg_rom); memcpy(stg_rom, net_img, 0x40000);
        net_down = 1;
        check(arm_stream("efefefefefefefefefefefefefefefef", CF_SRC_WIFI_STAGED) == 0, "armed STREAM (staged), 256 KiB over a full cart");
        run(1800, job_over);
        stream_expect_ok(&k);
        check(C.n_erase == 31, "one erase per block: 4 for the image, 27 for the tail (%u)", (unsigned)C.n_erase);
    } else if (!strcmp(which, "strm_xs_ffruns")) {
        /* cartspeed1: an image with long FF runs inside its blocks (as Tetris): the PROG ops skip them,
           so the ring carries only the bytes that are programmed; the cart is the image byte for byte */
        stream_setup(&k, 0x80000, 0x1B, 0);
        uint32_t nff = 0;
        for (uint32_t i = 0x4000; i < 0x80000; i++) if ((i % 0x2800) >= 0xC00 && (i % 0x2800) < 0x2400) { net_img[i] = 0xFF; nff++; }
        memset(stg_rom, 0xFF, sizeof stg_rom); memcpy(stg_rom, net_img, 0x80000);
        net_down = 1;
        check(arm_stream("abcdabcdabcdabcdabcdabcdabcdabcd", CF_SRC_WIFI_STAGED) == 0, "armed STREAM (staged), 512 KiB with FF runs of 6 KiB every 10 KiB");
        run(1800, job_over);
        stream_expect_ok(&k);
        check(XS.n_posts < (0x80000u - nff) / 1024u + 120u, "the ring carried about the non-FF bytes only (%u posts for %u non-FF bytes)", XS.n_posts, 0x80000u - nff);
    } else if (!strcmp(which, "strm_xs_stuck")) {
        /* cartspeed1: a cell that will not program (bit 3 stuck at 0 in block 5): the FPGA stops with
           VERIFY at it, the block is done again from a fresh start, then the job ends unfinished */
        stream_setup(&k, 0x80000, 0x1B, 0);
        memset(stg_rom, 0xFF, sizeof stg_rom); memcpy(stg_rom, net_img, 0x80000);
        uint32_t a = 0x50123; while (!(net_img[a] & 0x08)) a++;
        cart.has_stuck = 1; cart.stuck_addr = a; cart.stuck0 = 0x08;
        net_down = 1;
        check(arm_stream("cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd", CF_SRC_WIFI_STAGED) == 0, "armed STREAM (staged), 512 KiB, a stuck bit in block 5");
        run(1800, job_stopped);
        check(C.st.result == CF_D_FAILED, "ends unfinished after its restarts (result %u)", C.st.result);
        check(C.xs && C.xs->n_resync >= 2, "the block was tried again (%u restarts)", C.xs ? (unsigned)C.xs->n_resync : 0u);
        { uint8_t src = 0; check(cf_esp_stream_pending(&C, NULL, NULL, &src) && src == CF_SRC_WIFI_STAGED, "the journal keeps it unfinished (resume or restore)"); }
        check(no_logo(), "the cart has no first block: it never starts half written");
        check(cart.n_busy_writes == 0, "no bus write while the chip was busy");
    } else if (!strcmp(which, "strm_wstg") || !strcmp(which, "strm_wstg_cut") || !strcmp(which, "strm_xs_cut_prog")) {
        /* wifispeed1: the Wi-Fi image already downloaded and hashed into the staging area (source 3):
           written from there, every bank checked by CRC16 on the Game Boy side (no full read-back) */
        const int cut = !strcmp(which, "strm_wstg_cut") || !strcmp(which, "strm_xs_cut_prog");
        stream_setup(&k, 0x200000, 0x1B, 0);
        memset(net_img + 0x1F0000, 0xFF, 0x10000);
        memset(stg_rom, 0xFF, sizeof stg_rom);
        memcpy(stg_rom, net_img, 0x1F0000);
        net_down = 1;                                          /* the network is gone: nothing may come from it */
        check(arm_stream("abababababababababababababababab", CF_SRC_WIFI_STAGED) == 0, "armed STREAM from the staged download, 2 MiB");
        if (cut) {
            cut_when = !strcmp(which, "strm_xs_cut_prog") ? xs_cut_prog : strm_cut_erase; cut_fired = 0;
            run(3600, job_over);
            cut_when = NULL;
            check(cut_fired, !strcmp(which, "strm_xs_cut_prog") ? "power cut while the FPGA programs a byte (express, block 6)" : "power cut in the middle of a block erase");
            check(no_logo(), "after the cut the cart has no first block");
            game_marker = -1;
            run(8, NULL);
            { uint8_t src = 0; check(cf_esp_stream_pending(&C, NULL, NULL, &src) && src == CF_SRC_WIFI_STAGED, "after the restart: unfinished, from the staged download"); }
            check(arm_stream("abababababababababababababababab", CF_SRC_WIFI_STAGED) == 0, "re-armed (resume) from the staged bytes");
        }
        run(3600, job_over);
        stream_expect_ok(&k);
        check(net_bytes == 0 && net_asks == 0, "not one byte asked from the network (%u)", net_bytes);
    } else if (!strcmp(which, "strm_wprog_stale")) {
        /* cartspeed2: the staging area does not hold what the download received (a stale read, a bad
           flash write): the block's crc32 differs from the stream's: void before block 0, resume ok */
        stream_setup(&k, 0x200000, 0x1B, 0);
        memset(net_img + 0x1F0000, 0xFF, 0x10000);
        memset(stg_rom, 0xFF, sizeof stg_rom);
        memcpy(stg_rom, net_img, 0x1F0000);
        stg_rom[0x54321] ^= 0x08;
        net_down = 1;
        prog_on = 1; prog_bad = 0; prog_blkcrc = 1; prog_t0 = t_us; prog_early_b0 = 0; prog_gate_waits = 0; prog_blank_seen = 0;
        check(arm_stream("efefefefefefefefefefefefefefefef", CF_SRC_WIFI_STAGED) == 0, "armed STREAM while the image downloads (express)");
        run(3600, job_stopped);
        check(C.st.result == CF_D_VERIFY, "the staged block 5 is not what came: stopped (result %u)", C.st.result);
        check(no_logo(), "the cart has no first block");
        stg_rom[0x54321] ^= 0x08;
        prog_t0 = t_us; game_marker = -1;
        run(8, NULL);
        check(arm_stream("efefefefefefefefefefefefefefefef", CF_SRC_WIFI_STAGED) == 0, "re-armed (resume) with the image downloading again");
        run(3600, job_over);
        stream_expect_ok(&k);
        check(prog_early_b0 == 0, "block 0 was never written before the whole image was there (%u)", prog_early_b0);
        prog_on = 0; prog_blkcrc = 0;
    } else if (!strncmp(which, "strm_wprog", 10)) {
        /* cartspeed2: the cart written WHILE the image downloads (express stream, block by block behind
           it); block 0 and the save RAM only after the whole image's check. _bad: the check fails at
           the end (the blocks are void, a resume writes them all); _cut: a power cut mid-way, the
           download starts over, the resume reads the blocks done before it back (_diff: one of
           them was written from a wrong staged byte: written again) */
        const int bad = !strcmp(which, "strm_wprog_bad"), cutp = !strncmp(which, "strm_wprog_cut", 14), diff = !strcmp(which, "strm_wprog_cut_diff");
        stream_setup(&k, 0x200000, 0x1B, 0);
        memset(net_img + 0x1F0000, 0xFF, 0x10000);
        memset(stg_rom, 0xFF, sizeof stg_rom);
        memcpy(stg_rom, net_img, 0x1F0000);
        if (diff) stg_rom[0x23456] ^= 0x40;                     /* block 2, wrong in the staging area before the cut */
        net_down = 1;
        prog_on = 1; prog_bad = bad; prog_t0 = t_us; prog_early_b0 = 0; prog_gate_waits = 0; prog_blank_seen = 0;
        check(arm_stream("cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd", CF_SRC_WIFI_STAGED) == 0, "armed STREAM while the image downloads (express)");
        if (bad) {
            run(3600, job_stopped);
            check(C.st.result == CF_D_VERIFY, "the image did not check out at the end: stopped (result %u)", C.st.result);
            check(no_logo(), "the cart has no first block");
            { uint8_t src = 0; check(cf_esp_stream_pending(&C, NULL, NULL, &src) && src == CF_SRC_WIFI_STAGED, "unfinished (resumable)"); }
            int any = 0; for (unsigned i = 0; i < sizeof journal.done; i++) if (journal.done[i]) any = 1;
            check(!any, "no block counts as done any more");
            prog_bad = 0; prog_t0 = t_us;                        /* downloaded again, right this time */
            game_marker = -1;
            run(8, NULL);
            check(arm_stream("cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd", CF_SRC_WIFI_STAGED) == 0, "re-armed (resume) with the image downloading again");
        }
        if (cutp) {
            cut_when = xs_cut_prog; cut_fired = 0;
            run(3600, job_over);
            cut_when = NULL;
            check(cut_fired, "power cut while the FPGA programs a byte (block 6), the image still downloading");
            check(no_logo(), "after the cut the cart has no first block");
            if (diff) stg_rom[0x23456] ^= 0x40;                 /* the new download has the right byte */
            prog_t0 = t_us;                                      /* the download starts over after the restart */
            game_marker = -1;
            run(8, NULL);
            check(arm_stream("cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd", CF_SRC_WIFI_STAGED) == 0, "re-armed (resume) while the image downloads again");
        }
        run(3600, job_over);
        stream_expect_ok(&k);
        check(prog_early_b0 == 0, "block 0 was never written before the whole image was there (%u)", prog_early_b0);
        check(prog_gate_waits > 0, "the cart write waited for the image's check (%u asks)", prog_gate_waits);
        if (cutp && C.xs) check(C.xs->n_rechk + C.xs->n_rebad >= 4, "the resume read the blocks done before the cut back (%u ok, %u written again)", C.xs->n_rechk, C.xs->n_rebad);
        if (diff && C.xs) check(C.xs->n_rebad == 1, "exactly the block written from the wrong byte was written again (%u)", C.xs->n_rebad);
        prog_on = 0;
    } else if (!strcmp(which, "strm_2mib")) {
        stream_setup(&k, 0x200000, 0x1B, 0);                 /* 2 MiB whose last 64 KiB are FF, the cart's top block blank */
        memset(net_img + 0x1F0000, 0xFF, 0x10000);
        check(arm_stream("88888888888888888888888888888888", 0) == 0, "armed STREAM, 2,097,152 bytes with an FF tail");
        run(3600, job_over);
        stream_expect_ok(&k);
        check(cart.n_prot_ignored == 0, "the protected block was never written to");
    } else if (!strcmp(which, "strm_2mib_top")) {
        stream_setup(&k, 0x200000, 0x1B, 1);                 /* the cart's protected top block holds data */
        memset(net_img + 0x1F0000, 0xFF, 0x10000);
        check(arm_stream("99999999999999999999999999999999", 0) == 0, "armed STREAM, 2 MiB, on a cart whose top block is not blank");
        run(600, job_stopped);
        check(C.st.result == CF_D_BAD_IMAGE, "refused (result %u)", C.st.result);
        check(flash_eq(orig_chip, 0, k.size) && cart.n_erase == 0, "nothing was erased or written");
        check(!cf_esp_stream_pending(&C, NULL, NULL, NULL), "and nothing is left unfinished");
        game_marker = -1;
        run(8, NULL);
        check(game_marker == 0x77, "the old game still boots (marker %02x)", game_marker & 0xFF);
    }
    report_stats();
    return 0;
}

/* ---------------------------------------------------------------- the owner's USB backup / restore
   (protocol CBU1): host/cart_usb.py, the reference implementation, against cf_usb.c */
#include <sys/wait.h>
static const char *usb_tool_cmd;
static int tool_status = -1;
static int tool_exited(void)
{
    int st;
    if (tool_status >= 0) return 1;
    if (usb_pid > 0 && waitpid(usb_pid, &st, WNOHANG) == usb_pid) { tool_status = WIFEXITED(st) ? WEXITSTATUS(st) : 99; return 1; }
    return 0;
}
static uint8_t *slurp(const char *path, long *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) { *n = -1; return NULL; }
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)*n + 1);
    if (fread(b, 1, (size_t)*n, f) != (size_t)*n) *n = -1;
    fclose(f);
    return b;
}
static int sc_usb(const char *which)
{
    cart_cfg_t k;
    if (!usb_tool_cmd) { printf("  (needs --usb-tool CMD)\n"); return 1; }
    con_rate = 0;
    if (!strncmp(which, "usb_backup", 10)) {
        cart_cfg_sst39vf1682(&k);
        setup(&k, 1);
        if (!strcmp(which, "usb_backup_noise")) con_flip_at = 300000;        /* one byte wrong on the wire */
        check(usb_spawn(usb_tool_cmd) == 0, "the reference tool started: backup");
        run(3600, tool_exited);
        check(tool_status == 0, "the tool finished and is satisfied (exit %d)", tool_status);
        long rn, sn;
        uint8_t *rom = slurp("out.gb", &rn), *sav = slurp("out.sav", &sn);
        check(rn >= 0x8000 && rn <= (long)k.size && !memcmp(rom, cart.flash, (size_t)rn), "the .gb file == the cart's flash (%ld bytes)", rn);
        int ff = 1; for (uint32_t i = (uint32_t)(rn > 0 ? rn : 0); i < k.size; i++) if (cart.flash[i] != 0xFF) ff = 0;
        check(ff, "and nothing but FF was left out of it");
        check(sn == CF_RAM_SIZE && !memcmp(sav, cart.ram, CF_RAM_SIZE), "the .sav file == the cart's save RAM (32,768 bytes)");
        check(flash_eq(orig_chip, 0, k.size) && !memcmp(cart.ram, orig_ram, CF_RAM_SIZE), "the cart was only read");
        check(!journal.bak_ok && !cf_esp_pending(&C, NULL, NULL), "nothing kept on the ESP32");
        if (!strcmp(which, "usb_backup_noise")) check(con_flip_at < 0, "a byte was corrupted on the wire, and the tool asked for that range again");
    } else {
        stream_setup(&k, 0x60000u + 777u, 0x1B, 0);          /* the owner's own file: 393,993 bytes */
        uint8_t savf[CF_RAM_SIZE];
        for (unsigned i = 0; i < CF_RAM_SIZE; i++) savf[i] = (uint8_t)(i * 7 + (i >> 9) + 3);
        FILE *f = fopen("restore.gb", "wb"); fwrite(net_img, 1, net_size, f); fclose(f);
        f = fopen("restore.sav", "wb"); fwrite(savf, 1, sizeof savf, f); fclose(f);
        const int stg = !strncmp(which, "stg_", 4);
        if (!strcmp(which, "stg_patch")) {
            /* the cart page's patch job: the cart's own game with its header (block 0) and a little
               more changed, written over the game it came from (as a PARTY patch) */
            free(net_img);
            net_img = malloc(orig_used);
            memcpy(net_img, orig, orig_used);
            memcpy(net_img + 0x134, "SIM TETRIS PTY", 14);
            uint8_t h = 0; for (int i = 0x134; i <= 0x14C; i++) h = (uint8_t)(h - net_img[i] - 1);
            net_img[0x14D] = h;
            for (uint32_t i = 0x2400; i < 0x2440; i++) net_img[i] ^= 0x5A;     /* code in bank 0 */
            for (uint32_t i = 0x24100; i < 0x24180; i++) net_img[i] ^= 0x3C;   /* and in bank 9 */
            net_size = orig_used;
            want_marker = 0x77;
            f = fopen("restore.gb", "wb"); fwrite(net_img, 1, net_size, f); fclose(f);
        }
        if (stg) {
            /* usb-cart-1: the computer has written the ROM and the save into the ESP32's flash */
            memset(stg_rom, 0xFF, sizeof stg_rom);
            memcpy(stg_rom, net_img, net_size);
            memcpy(stg_sav, savf, sizeof stg_sav);
            stg_sav_src = 1;
            if (!strcmp(which, "stg_bad")) stg_rom[0x23456] ^= 0x04;          /* one bit wrong in the staged copy */
            if (!strcmp(which, "stg_big")) stg_room_bad = 1;                  /* a firmware image that reaches the staging area */
            if (!strcmp(which, "stg_online")) { sim_online = 1; sim_held = 0; }
        }
        if (!strcmp(which, "usb_restore_blank") || !strcmp(which, "stg_blank")) {
            /* another firmware's unfinished write: no first block, other data after it, and
               nothing about it in this ESP32's journal */
            memset(cart.flash, 0xFF, 0x10000);
            for (uint32_t i = 0x10000; i < 0x28000; i++) cart.flash[i] = (uint8_t)(i * 13 + 5);
            want_cycle = 1; run(3, NULL);
            check(no_logo(), "the cart has no first block (no header, no logo, no entry point)");
        }
        uint8_t *before = malloc(k.size);
        memcpy(before, cart.flash, k.size);
        check(usb_spawn(usb_tool_cmd) == 0, "the reference tool started: %s", !strcmp(which, "usb_savonly") ? "the save only" : "restore");
        if (!strcmp(which, "usb_restore_cut") || !strcmp(which, "stg_cut")) { cut_when = strm_cut_erase; cut_fired = 0; }
        const unsigned e0 = cart.n_erase, p0 = cart.n_prog;
        for (int i = 0; i < 6 && !tool_exited(); i++) run(7200, tool_exited);   /* a cut returns early: carry on */
        cut_when = NULL;
        if (!strcmp(which, "usb_restore_cut") || !strcmp(which, "stg_cut")) check(cut_fired, "power cut in the middle of a block erase; the tool sent the same command again");
        if (opt_wp_low && strcmp(which, "usb_savonly")) {
            /* 1.0.2: an SST39VF1681 whose WP# holds its bottom block: refused before any erase */
            check(tool_status != 0, "the tool was told no (exit %d)", tool_status);
            const uint8_t want_code = opt_wp_garble ? CF_D_WPTEST : CF_D_LOCKED;
            check(C.st.result == want_code, "the job ended with code %u (%s, got %u)", want_code, opt_wp_garble ? "CF_D_WPTEST" : "CF_D_LOCKED", C.st.result);
            check(!memcmp(cart.flash, before, k.size), "the cart's flash is byte for byte as before (all %u bytes)", (unsigned)k.size);
            check(!memcmp(cart.ram, orig_ram, CF_RAM_SIZE), "the save RAM is as before");
            check(cart.n_erase == 0 && cart.n_prog == 0 && cart.n_prot_bottom_ignored == 1, "no erase or program took; the one test write was ignored by the chip (%u)", cart.n_prot_bottom_ignored);
            check(journal.phase == CF_PH_IDLE && !cf_esp_pending(&C, NULL, NULL) && !cf_esp_stream_pending(&C, NULL, NULL, NULL), "journal idle: nothing unfinished");
            cart.garble_at = 0;
            game_marker = -1; want_cycle = 1; run(8, NULL);
            check(game_marker == 0x77, "the cart's own game still boots (marker %02x)", game_marker & 0xFF);
            report_stats();
            return 0;
        }
        if (!strcmp(which, "usb_savonly")) {
            check(tool_status == 0, "the tool finished and is satisfied (exit %d)", tool_status);
            check(!memcmp(cart.flash, before, k.size), "the game (flash) is untouched");
            check(!memcmp(cart.ram, savf, CF_RAM_SIZE), "the save RAM == the .sav file");
            check(C.wp_scan == 0 && C.wp_step == 0 && cart.n_prog == 0 && cart.n_erase == 0, "no write test, no flash command: the save is in FRAM");
            report_stats();
            return 0;
        }
        if (!strcmp(which, "stg_bad") || !strcmp(which, "stg_big")) {
            check(tool_status != 0, "the tool was told no (exit %d)", tool_status);
            check(cart.n_erase == e0 && cart.n_prog == p0 && flash_eq(orig_chip, 0, k.size), "nothing on the cart was erased or written");
            check(n_cycles == 0 || !C.armed, "no cart job ran");
            report_stats();
            return 0;
        }
        check(tool_status == 0, "the tool finished and is satisfied (exit %d)", tool_status);
        run(8, NULL);
        check(cart_is_image(k.size), "the cart holds the file, FF after it");
        check(!memcmp(cart.ram, savf, CF_RAM_SIZE), "the save RAM == the .sav file");
        check(journal.phase == CF_PH_IDLE && !journal.bak_ok, "journal idle, nothing kept on the ESP32");
        if (stg) {
            run(8, NULL);
            int ff = 1;
            for (uint32_t i = 0; i < sizeof stg_rom && ff; i++) if (stg_rom[i] != 0xFF) ff = 0;
            for (uint32_t i = 0; i < sizeof stg_sav && ff; i++) if (stg_sav[i] != 0xFF) ff = 0;
            check(ff && n_stg_erases >= 1, "the staged ROM and save were erased by the firmware after the install");
        }
        if (sim_online) {
            check(n_hold_on >= 1, "the link hold was taken before the job (%u times)", n_hold_on);
            run(40, NULL);
            check(!sim_held && n_hold_off >= 1, "the link hold was lifted after the job: the link comes back (on %u, off %u)", n_hold_on, n_hold_off);
        }
        check(game_marker == want_marker, "the restored game boots (marker %02x)", game_marker & 0xFF);
        if (is1681()) check(cart.n_prot_bottom_ignored == 0 && wp_seen_done, "SST39VF1681, WP# high: the write test passed before the first erase; no write was ignored");
        if (!opt_rx_repl && opt_rx_loss <= 0 && opt_rx_kill_n == 0 && opt_rx_kill_off < 0) check(U.n_bad_chunks == 0, "every chunk arrived whole (%u chunks)", U.n_chunks);
        else printf("  console RX model: %u characters lost to a full buffer, %u lines damaged on purpose, %u data lines refused and asked again, %u other lines while busy, %u chunks taken\n",
                    n_rx_dropped, n_rx_damaged_lines, U.n_bad_chunks, U.n_garbage, U.n_chunks);
        if (opt_rx_repl && !opt_usb_chunk) check(n_rx_dropped == 0, "no character was lost to a full receive buffer (lines fit it whole)");
    }
    report_stats();
    return 0;
}

/* Cart jobs on a unit whose game runs with a patch ACTIVE (FPGA 18.22+ overlay). With the hold
   (cf_esp overlay_hold / overlay_state) every job sees the cart's own bytes and the game is
   patched again afterwards; with --legacy-no-hold (the firmware before it) these fail. */
static void ovl_begin(void)
{
    ovl_cfg_on = 1;
    want_cycle = 1; run(3, NULL);                       /* the game is running, served through the overlay */
    check(ovl_active == 1, "a patch is ACTIVE on the running game");
}
static void ovl_after(int expect_reads_zero)
{
    run(8, NULL);
    if (expect_reads_zero) check(n_ovl_reads_job == 0, "no cart byte the job read came from the overlay (%u did)", n_ovl_reads_job);
    check(ovl_hold == 0, "the hold is released after the job");
    check(ovl_active == 1, "and the game is patched again (the owner's setting was not touched)");
}
static int sc_overlay(const char *which)
{
    cart_cfg_t k;
    if (!strcmp(which, "ovl_backup")) {
        if (!usb_tool_cmd) { printf("  (needs --usb-tool CMD)\n"); return 1; }
        con_rate = 0;
        cart_cfg_sst39vf1682(&k);
        setup(&k, 1);
        ovl_begin();
        ovl_job = 1; n_ovl_reads_job = 0;
        check(usb_spawn(usb_tool_cmd) == 0, "the reference tool started: backup");
        run(3600, tool_exited);
        ovl_job = 0;
        check(tool_status == 0, "the tool finished and is satisfied (exit %d)", tool_status);
        long rn, sn;
        uint8_t *rom = slurp("out.gb", &rn), *sav = slurp("out.sav", &sn);
        check(rom && rn >= 0x8000 && !memcmp(rom, cart.flash, (size_t)rn), "the .gb file == the cart's OWN flash, not the patched view (%ld bytes)", rn);
        (void)sav;
        ovl_after(1);
    } else if (!strcmp(which, "ovl_restore")) {
        if (!usb_tool_cmd) { printf("  (needs --usb-tool CMD)\n"); return 1; }
        con_rate = 0;
        stream_setup(&k, 0x60000u + 777u, 0x1B, 0);
        uint8_t savf[CF_RAM_SIZE];
        for (unsigned i = 0; i < CF_RAM_SIZE; i++) savf[i] = (uint8_t)(i * 7 + (i >> 9) + 3);
        FILE *f = fopen("restore.gb", "wb"); fwrite(net_img, 1, net_size, f); fclose(f);
        f = fopen("restore.sav", "wb"); fwrite(savf, 1, sizeof savf, f); fclose(f);
        ovl_begin();
        ovl_job = 1; n_ovl_reads_job = 0;
        check(usb_spawn(usb_tool_cmd) == 0, "the reference tool started: restore");
        for (int i = 0; i < 6 && !tool_exited(); i++) run(7200, tool_exited);
        ovl_job = 0;
        check(tool_status == 0, "the tool finished and is satisfied (exit %d)", tool_status);
        run(8, NULL);
        check(cart_is_image(k.size), "the cart holds the file, FF after it");
        check(!memcmp(cart.ram, savf, CF_RAM_SIZE), "the save RAM == the .sav file");
        ovl_after(1);
    } else if (!strcmp(which, "ovl_stream")) {
        stream_setup(&k, 0x80000, 0x1B, 0);
        ovl_begin();
        ovl_job = 1; n_ovl_reads_job = 0;
        check(arm_stream("33333333333333333333333333333333", 0) == 0, "armed STREAM (gateway), 512 KiB");
        run(1800, job_over);
        ovl_job = 0;
        check(C.st.result == CF_D_OK, "installed: every blank check, compare and the crc32 + sha256 read-back saw the cart's own bytes (result %u)", C.st.result);
        check(cart_is_image(k.size), "the cart holds the image");
        ovl_after(1);
    } else {                                            /* ovl_stuck_usb / ovl_stuck_stream: the FPGA keeps the overlay on */
        const int usb = !strcmp(which, "ovl_stuck_usb");
        stream_setup(&k, 0x60000u + 777u, 0x1B, 0);
        ovl_begin();
        ovl_stuck = 1;
        const unsigned e0 = cart.n_erase, p0 = cart.n_prog;
        check(arm_stream("44444444444444444444444444444444", usb) == 0, "armed STREAM (%s) while the FPGA keeps its overlay on", usb ? "USB" : "gateway");
        run(60, job_stopped);
        check(C.st.result == CF_D_OVERLAY, "refused: the patch overlay is active (result %u)", C.st.result);
        check(cart.n_erase == e0 && cart.n_prog == p0 && flash_eq(orig_chip, 0, k.size), "nothing was erased or written");
        check(!cf_esp_pending(&C, NULL, NULL), "and nothing is left pending");
        run(8, NULL);
        check(ovl_hold == 0, "the hold is released");
    }
    report_stats();
    return 0;
}

/* the model itself: what gb.v does to CPU writes while the boot ROM is mapped */
static int sc_bootblock(void)
{
    cart_cfg_t k; cart_cfg_sst39vf1682(&k);
    opt_gb_writes_flash = 1;
    setup(&k, 0);
    boot_mapped = 1;
    const unsigned b0 = n_boot_blocked;
    on_write(gb, 0x0000, 0x0A);
    check(!cart.ram_en && n_boot_blocked == b0 + 1, "boot ROM mapped: 0A@0000 never reaches the cart (RAM stays disabled)");
    on_write(gb, 0x1000, 0x0A);
    check(cart.ram_en, "0A@1000 does (same MBC register)");
    on_write(gb, 0x0AAA, 0xAA); on_write(gb, 0x0555, 0x55); on_write(gb, 0x0AAA, 0x90);
    check(cart_read(&cart, 0x4000 + 0) != 0xBF || cart.rom_bank != 0, "AA@0AAA 55@0555 90@0AAA: 55@0555 is blocked, no ID mode");
    on_write(gb, 0x1000, 0xF0);
    on_write(gb, 0x2000, 0x00); on_write(gb, 0x3000, 0x00);
    on_write(gb, 0x1AAA, 0xAA); on_write(gb, 0x1555, 0x55); on_write(gb, 0x1AAA, 0x90);
    check(cart_read(&cart, 0x4000) == 0xBF && cart_read(&cart, 0x4001) == 0xC9, "AA@1AAA 55@1555 90@1AAA: ID BF C9 at 4000 (bank 0), on a cart that takes GB-timed writes");
    on_write(gb, 0x1000, 0xF0);
    check(cart_read(&cart, 0x4000) == cart.flash[0], "F0@1000: read mode again");
    on_write(gb, 0x0555, 0x55); on_write(gb, 0x02AA, 0x55); on_write(gb, 0x00FF, 1); on_write(gb, 0x0200, 1); on_write(gb, 0x08FF, 1);
    check(n_boot_blocked == b0 + 2 + 5, "0000-00FF and 0200-08FF blocked, 0100-01FF and 0900+ pass (%u)", n_boot_blocked - b0);
    boot_mapped = 0;
    on_write(gb, 0x0000, 0x0A);
    check(cart.ram_en, "boot ROM unmapped (a running game): 0A@0000 reaches the cart");
    printf("  stats: 0.0 s emulated\n");
    return 0;
}

static int sc_gbtest(int gbw)
{
    cart_cfg_t k; cart_cfg_sst39vf1682(&k);
    opt_gb_writes_flash = gbw;
    setup(&k, 0);
    const unsigned b0 = n_boot_blocked;
    check(arm_job(CF_JOB_GBTEST, 0) == 0, "armed GBTEST");
    run(300, job_over);
    check(C.st.result == CF_D_OK && C.gbtest[0], "gbtest ran (result %u)", C.st.result);
    check(C.gbtest[1] && C.gbtest[2], "GB-timed RAM reads == the engine's, with the engine's enable and with 0A@1000");
    if (gbw) check(C.gbtest[7] && C.gbtest[3] == 0xBF && C.gbtest[4] == 0xC9, "GB-timed flash ID at 1AAA/1555: BF C9 (a cart that takes GB-timed flash writes)");
    else check(!C.gbtest[7], "a cart whose flash ignores GB-timed writes: no ID (%02x %02x)", C.gbtest[3], C.gbtest[4]);
    check(C.gbtest[5] == cart.flash[0] && C.gbtest[6] == cart.flash[1], "read mode after the exit");
    check(!memcmp(cart.ram, orig_ram, CF_RAM_SIZE) && flash_eq(orig_chip, 0, k.size), "RAM and flash untouched");
    check(n_boot_blocked == b0, "no write aimed under the mapped boot ROM");
    check(game_marker == 0x77, "the game boots after (marker %02x)", game_marker & 0xFF);
    report_stats();
    return 0;
}

/* the GB-timed route (writer v4 F_GBT, job option gb_timed): no engine in the bitstream */
static int sc_gbt(int diy)
{
    cart_cfg_t k; cart_cfg_sst39vf1682(&k);
    opt_gb_writes_flash = diy;           /* 1: a cart whose flash takes CPU writes; 0: the ModRetro truth */
    opt_no_engine = 1;
    setup(&k, 0);
    const unsigned b0 = n_boot_blocked;
    cf_job_t j = { .job = CF_JOB_INSTALL, .allow_unverified = 0, .ram_clear = 1, .max_retries = 2, .gb_timed = 1 };
    C.st.result = 0xEE; n_cycles = 0;
    check(cf_esp_arm(&C, &j) == 0, "armed INSTALL, GB-timed route, FPGA without the engine");
    run(900, job_over);
    check(n_boot_blocked == b0, "no GB-timed write aimed under the mapped boot ROM (%u blocked)", n_boot_blocked - b0);
    check(EG.n_ops == 0, "the engine was never used (%u ops)", EG.n_ops);
    if (diy) {
        expect_installed();
    } else {
        check(C.st.result != CF_D_OK, "a cart whose flash ignores CPU writes: the job does not report success (result %u)", C.st.result);
        check(flash_eq(orig_chip, 0, k.size), "and the flash is unchanged (nothing erased, nothing programmed)");
    }
    report_stats();
    return 0;
}

/* any ROM may be installed (owner 2026-10-11): the device judges the length only */
static int sc_bad_image(void)
{
    cart_cfg_t k; cart_cfg_sst39vf1682(&k);
    setup(&k, 0);
    /* larger than what this chip lets us write: 2 MiB minus the protected top 64 KiB */
    free(img);
    img_size = k.size - 0x10000u + 0x1000u;
    img = make_rom(0x200000, "TOO BIG", 0x1B, 0x42, 5);
    C.h.img_size = img_size;
    check(arm(CF_JOB_INSTALL) == 0, "armed INSTALL with an image past the writable flash (%u bytes)", (unsigned)img_size);
    run(600, job_stopped);
    check(C.st.result == CF_D_BAD_IMAGE, "refused BAD_IMAGE (got %u)", C.st.result);
    check(cart.n_erase == 0 && cart.n_prog == 0, "nothing erased");
    check(journal.bak_ok, "the backup was still taken (it was needed first and is kept)");
    check(flash_eq(orig_chip, 0, k.size), "flash unchanged");
    report_stats();
    return 0;
}

/* a foreign ROM: not MBC5, no Shiny Gen identity, a length that is not a power of two */
static int sc_foreign(void)
{
    cart_cfg_t k; cart_cfg_sst39vf1682(&k);
    setup(&k, 0);
    free(img);
    img = make_rom(0x40000, "FOREIGN ROM", 0x13, 0x42, 77);      /* header says MBC3 + 256 KiB */
    img_size = 0x30000u + 123u;                                  /* but 196,731 bytes arrive */
    C.h.img_size = img_size;
    memset(sav2, 0xEE, sizeof sav2);
    check(arm(CF_JOB_INSTALL) == 0, "armed INSTALL with a foreign ROM (MBC3 header, 196,731 bytes)");
    run(900, job_over);
    check(C.st.result == CF_D_OK, "installed (result %u)", C.st.result);
    check(flash_eq(img, 0, img_size), "flash[0..196,731) == the image");
    int ff = 1; for (uint32_t i = img_size; i < 0x40000u; i++) if (cart.flash[i] != 0xFF) ff = 0;
    check(ff, "the rest of its last 64 KiB block is FF");
    check(flash_eq(orig_chip + 0x40000, 0x40000, k.size - 0x40000), "flash past that block untouched");
    check(journal.bak_ok && !memcmp(bak, orig_chip, k.size) && !memcmp(ram_bak, orig_ram, CF_RAM_SIZE), "the original cart (ROM + save) is in the backup");
    check(!memcmp(sav2, orig_ram, CF_RAM_SIZE), "the replaced game's save is kept in the save store");
    int z = 1; for (unsigned i = 0; i < CF_RAM_SIZE; i++) if (cart.ram[i]) z = 0;
    check(z, "the cart's save RAM is cleared for the new game");
    check(game_marker == 0x42, "the foreign ROM boots (marker %02x)", game_marker & 0xFF);
    run(3, NULL);
    /* a cart with a header the console would not start can still be put back */
    cart.flash[0x104] ^= 0xFF;
    memcpy(journal.tgt_hdr, cart.flash + 0x100, 80);
    C.j = journal;
    check(arm(CF_JOB_RESTORE) == 0, "armed RESTORE on a cart whose header is now invalid");
    run(1800, job_over);
    check(C.st.result == CF_D_OK && flash_eq(orig_chip, 0, k.size) && !memcmp(cart.ram, orig_ram, CF_RAM_SIZE), "the original cart is back, ROM and save (result %u)", C.st.result);
    report_stats();
    return 0;
}

/* ---------------------------------------------------------------- 1.0.2: the SST39VF1681's write test
   (cf_esp.c "WP TEST"): before the first erase of a write job, one bit of one byte of the bottom
   64 KiB block (WP#-protected when WP# is low) is programmed and read back */
static uint32_t wp_cut_at; static uint8_t wp_cut_v, wp_cut_nv;
static int wp_cut_after_test(void)
{
    if (C.ph == CF_PH_STREAM && C.sp == 9 /* SP_WPTEST */ && C.wp_step == 3 && cart.n_prog == 1 && !cart_busy(&cart)) {
        wp_cut_at = C.wp_at; wp_cut_v = C.wp_v; wp_cut_nv = C.wp_nv;
        return 1;
    }
    return 0;
}
static int wp_cut_in_erase0(void) { return cart_busy(&cart) && cart.busy_kind == 2 && cart.busy_addr == 0 && C.wp_done; }
static int wp_diff(const uint8_t *a, const uint8_t *b, uint32_t n, uint32_t *at)
{
    int d = 0;
    for (uint32_t i = 0; i < n; i++) if (a[i] != b[i]) { d++; *at = i; }
    return d;
}
static int arm_install_1681(void)
{
    cf_job_t j = { .job = CF_JOB_INSTALL, .allow_unverified = 0, .ram_clear = 1, .max_retries = 2 };
    C.st.result = 0xEE; n_cycles = 0;
    return cf_esp_arm(&C, &j);
}
static void wp_expect_locked(const uint8_t *before, uint32_t size, unsigned want_ignored)
{
    check(C.st.result == CF_D_LOCKED, "refused with code %u (CF_D_LOCKED; got %u)", CF_D_LOCKED, C.st.result);
    check(!memcmp(cart.flash, before, size), "the cart's flash is byte for byte as before (all %u bytes)", (unsigned)size);
    check(!memcmp(cart.ram, orig_ram, CF_RAM_SIZE), "the save RAM is as before");
    check(cart.n_erase == 0 && cart.n_prog == 0 && cart.n_prot_bottom_ignored == want_ignored,
          "no erase or program took; %u test write(s) reached the chip and were ignored (%u)", want_ignored, cart.n_prot_bottom_ignored);
    check(journal.phase == CF_PH_IDLE && !cf_esp_pending(&C, NULL, NULL) && !cf_esp_stream_pending(&C, NULL, NULL, NULL),
          "journal idle: nothing unfinished, no retry / restore offered");
}
static int sc_wp(const char *which)
{
    cart_cfg_t k;
    opt_chip1681 = 1;
    if (strstr(which, "_locked")) opt_wp_low = 1;
    if (strstr(which, "_neither")) { opt_wp_low = 1; opt_wp_garble = 1; }
    if (!strncmp(which, "wp_install", 10)) {
        /* the ESP32's own backup + write plan (OSD / Wi-Fi install): the test after the backup gate */
        cart_cfg_sst39vf1682(&k);
        setup(&k, 1);
        uint8_t *before = malloc(k.size); memcpy(before, cart.flash, k.size);
        check(arm_install_1681() == 0, "armed INSTALL on an SST39VF1681 (allow_unverified 0)%s", opt_wp_low ? ", WP# low" : "");
        if (opt_wp_low) {
            run(900, job_stopped);
            wp_expect_locked(before, k.size, 1);
            check(journal.bak_ok && !memcmp(bak, orig_chip, k.size), "the backup was complete before the test (the verified-backup gate came first)");
            game_marker = -1; run(8, NULL);
            check(game_marker == 0x77, "the cart's own game still boots (marker %02x)", game_marker & 0xFF);
        } else {
            run(900, job_over);
            expect_installed();
            check(C.wp_done && cart.n_prot_bottom_ignored == 0, "the write test passed (%06x: %02x -> %02x) and block 0 was then rewritten", C.wp_at, C.wp_v, C.wp_nv);
        }
    } else if (!strcmp(which, "wp_selftest_locked")) {
        cart_cfg_sst39vf1682(&k);
        setup(&k, 0);
        check(arm_job(CF_JOB_SELFTEST, 0) == 0, "armed SELFTEST on an SST39VF1681 with WP# low");
        run(900, job_over);
        check(C.st.result == CF_D_OK, "the selftest passes in a block of the upper half (result %u)", C.st.result);
        check(C.wp_scan == 0 && cart.n_prot_bottom_ignored == 0, "no write test: the selftest never touches the bottom block");
    } else if (!strncmp(which, "wp_stream", 9) || !strncmp(which, "wp_noff", 7) || !strcmp(which, "wp_zero") || !strcmp(which, "wp_hdr")) {
        stream_setup(&k, 0x60000u + 321u, 0x1B, 1);
        if (!strncmp(which, "wp_noff", 7)) for (uint32_t i = 0; i < 0x10000; i++) if (cart.flash[i] == 0xFF) cart.flash[i] = 0xFE;
        if (!strcmp(which, "wp_zero")) memset(cart.flash, 0, 0x10000);
        if (!strcmp(which, "wp_hdr")) { for (uint32_t i = 0; i < 0x10000; i++) if (i < 0x100 || i >= 0x150) cart.flash[i] = 0; }
        uint8_t *before = malloc(k.size); memcpy(before, cart.flash, k.size);
        check(arm_stream("abababababababababababababababab", 0) == 0, "armed STREAM (gateway) on an SST39VF1681%s (%s)", opt_wp_low ? ", WP# low" : "",
              !strncmp(which, "wp_noff", 7) ? "no FF byte in its first block" : !strcmp(which, "wp_zero") ? "first block all 00" :
              !strcmp(which, "wp_hdr") ? "first block 00 but the header" : "a game");
        if (opt_wp_low || !strcmp(which, "wp_zero")) {
            run(900, job_stopped);
            wp_expect_locked(before, k.size, !strcmp(which, "wp_zero") ? 0 : 1);
            check(C.n_erase == 0, "the writer was never told to erase");
            if (strcmp(which, "wp_zero")) { game_marker = -1; run(8, NULL); check(game_marker == 0x77, "the cart's own game still boots (marker %02x)", game_marker & 0xFF); }
        } else {
            run(1800, job_over);
            stream_expect_ok(&k);
            uint32_t want = 0;
            if (!strncmp(which, "wp_noff", 7)) { for (uint32_t i = 0xFFFF; i > 0; i--) if (!(i >= 0x100 && i < 0x150) && before[i]) { want = i; break; } }
            else if (!strcmp(which, "wp_hdr")) { for (uint32_t i = 0x14F; i >= 0x100; i--) if (before[i]) { want = i; break; } }
            else { for (uint32_t i = 0xFFFF; i >= 15; i--) { int run16 = 1; for (int q = 0; q < 16; q++) if (before[i - q] != 0xFF || (i - q >= 0x100 && i - q < 0x150)) run16 = 0; if (run16) { want = i; break; } } }
            check(C.wp_done && C.wp_at == want && C.wp_v == before[want] && C.wp_nv == (uint8_t)(before[want] & (before[want] - 1)),
                  "the test byte is %06x (%02x -> %02x): %s", C.wp_at, C.wp_v, C.wp_nv,
                  !strncmp(which, "wp_noff", 7) ? "the highest byte with a 1 bit outside the header" : !strcmp(which, "wp_hdr") ? "a header byte (nothing else has a 1 bit)" : "the top of the highest run of 16 FF bytes");
        }
    } else if (!strncmp(which, "wp_neither", 10)) {
        /* the write test reads back NEITHER the old nor the new value (a fault: the chip ignored the
           program, and that byte then reads with a bit flipped): code 14, nothing erased, never code 4 */
        const int inst = !strcmp(which, "wp_neither_install");
        if (inst) { cart_cfg_sst39vf1682(&k); setup(&k, 1); } else stream_setup(&k, 0x60000u + 321u, 0x1B, 1);
        uint8_t *before = malloc(k.size); memcpy(before, cart.flash, k.size);
        check((inst ? arm_install_1681() : arm_stream("a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1", 0)) == 0, "armed %s on an SST39VF1681 whose test byte reads back wrong", inst ? "INSTALL" : "STREAM");
        run(900, job_stopped);
        check(C.st.result == CF_D_WPTEST, "stopped with code %u (CF_D_WPTEST; got %u), not %u (CF_D_FAILED)", CF_D_WPTEST, C.st.result, CF_D_FAILED);
        check(C.wp_step == 3 && C.wp_v == 0xFF && C.wp_nv == 0xFE, "the read-back was neither %02x nor %02x", C.wp_v, C.wp_nv);
        check(!memcmp(cart.flash, before, k.size), "the cart's flash is byte for byte as before (all %u bytes)", (unsigned)k.size);
        check(!memcmp(cart.ram, orig_ram, CF_RAM_SIZE), "the save RAM is as before");
        check(cart.n_erase == 0 && cart.n_prog == 0 && C.n_erase == 0, "no erase was ever sent; no program took");
        check(journal.phase == CF_PH_IDLE && !cf_esp_pending(&C, NULL, NULL) && !cf_esp_stream_pending(&C, NULL, NULL, NULL), "journal idle: nothing unfinished");
        cart.garble_at = 0;
        game_marker = -1; run(8, NULL);
        check(game_marker == 0x77, "the cart's own game still boots (marker %02x)", game_marker & 0xFF);
    } else if (!strcmp(which, "wp_cut_test")) {
        stream_setup(&k, 0x60000u + 321u, 0x1B, 1);
        check(arm_stream("cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd", 0) == 0, "armed STREAM on an SST39VF1681");
        cut_when = wp_cut_after_test; cut_fired = 0;
        run(900, job_over);
        cut_when = NULL;
        check(cut_fired, "power cut after the test write (%06x: %02x -> %02x), before the erase of block 0", wp_cut_at, wp_cut_v, wp_cut_nv);
        uint32_t at = 0;
        const int d = wp_diff(cart.flash, orig_chip, k.size, &at);
        check(d == 1 && at == wp_cut_at && cart.flash[at] == wp_cut_nv && orig_chip[at] == wp_cut_v, "the cart differs from before in that one byte only (%d bytes differ)", d);
        check(journal.phase == CF_PH_IDLE && !cf_esp_stream_pending(&C, NULL, NULL, NULL), "the journal was never \"in flux\": nothing unfinished");
        game_marker = -1; want_cycle = 1; run(8, NULL);
        check(game_marker == 0x77, "the cart's own game still boots (the byte is FF padding; marker %02x)", game_marker & 0xFF);
        check(arm_stream("cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd", 0) == 0, "the same job again");
        run(1800, job_over);
        stream_expect_ok(&k);
        check(C.wp_done && C.wp_at != wp_cut_at, "the test ran again, on another byte (%06x), and passed", C.wp_at);
    } else if (!strcmp(which, "wp_cut_erase0")) {
        stream_setup(&k, 0x60000u + 321u, 0x1B, 1);
        check(arm_stream("efefefefefefefefefefefefefefefef", 0) == 0, "armed STREAM on an SST39VF1681");
        cut_when = wp_cut_in_erase0; cut_fired = 0;
        run(900, job_over);
        cut_when = NULL;
        check(cut_fired, "power cut during the erase of block 0, after the test passed");
        check(cf_esp_stream_pending(&C, NULL, NULL, NULL), "unfinished in the journal (in flux from just before that erase)");
        run(5, NULL);
        check(arm_stream("efefefefefefefefefefefefefefefef", 0) == 0, "resumed");
        run(1800, job_over);
        stream_expect_ok(&k);
        check(C.wp_scan == 0 && C.wp_step == 0, "the resume skipped the test (its first session passed it)");
    } else {
        printf("  unknown wp scenario\n");
        fails++;
    }
    report_stats();
    return 0;
}

static int sc_other_cart(void)
{
    cart_cfg_t k; cart_cfg_sst39vf1682(&k);
    setup(&k, 0);
    check(arm(CF_JOB_BACKUP) == 0, "armed BACKUP of cart 1");
    run(600, job_stopped);
    check(C.st.result == CF_D_OK && journal.bak_ok, "cart 1 backed up");
    /* the user swaps in another ModRetro cart */
    uint8_t *other = make_rom(0x40000, "OTHER GAME", 0x1B, 0x55, 7);
    memcpy(cart.flash, other, 0x40000);
    run(3, NULL);
    check(arm(CF_JOB_INSTALL) == 0, "armed INSTALL with cart 2 inserted");
    run(600, job_stopped);
    check(C.st.result == CF_D_BAD_HEADER, "refused: not the cart the backup is from (got %u)", C.st.result);
    check(cart.n_erase == 0, "nothing erased");
    check(!memcmp(bak, orig_chip, k.size), "cart 1's backup untouched");
    report_stats();
    return 0;
}

static int booted(void) { return game_marker >= 0; }
static int usb_done(void) { return C.probe_lines > 0 && !C.probe_armed && game_marker >= 0; }
static int sc_probe(int prod)   /* 0 read-only, 1 receiver not local, 2 flash-ID build */
{
    if (prod != 3 && !receiver_port) { printf("  (needs --receiver PORT)\n"); return 1; }
    if (prod == 3 && usb_out < 0) { printf("  (needs --usb-tool CMD)\n"); return 1; }
    cart_cfg_t k; cart_cfg_sst39vf1682(&k);
    esp_stock_fw = prod != 3;          /* the Wi-Fi path is for stock 0.17.x firmware */
    setup(&k, 1);
    const unsigned w0 = n_cart_writes_total;
    if (prod == 3) {
        /* the stub may give up on the first boot (nothing armed, no Wi-Fi); the tool's
           `cart probe` power-cycles the cart and the probe runs; wait for its end */
        run(1800, usb_done);
        run(30, booted);
        check(C.probe_lines + C.probe_frames > 10, "probe data went over the USB console (%u control lines, %u binary frames, %u console commands, %u sum errors)",
              C.probe_lines, C.probe_frames, usb_cmds_in, C.probe_bad_sums);
        printf("  console: %.1f KB/s average over the run, RAM reads that floated: %u\n",
               (double)(C.probe_frames * 1035u) / (t_us / 1e3), cart.n_ram_float_reads);
        check(E.online == 0, "no Wi-Fi was used (the ESP32 stayed offline)");
        printf("  window: local ops %u, requests %u, svc %d, unlocked %d, cycles %d\n", E.n_local, E.n_requests, F.svc, F.unlocked, n_cycles);
    } else
        run(prod == 1 ? 30 : 1500, booted);   /* the stub finds no job (CF_LOAD DENIED), GETs cp/probe.bin, runs it */
    if (prod == 1) {
        check(n_cart_writes_total == w0, "receiver not local: the probe never ran, no cart write at all");
        check(game_marker == 0x77, "the cart booted normally (marker %02x)", game_marker & 0xFF);
    } else {
        check(game_marker == 0x77, "after the probe the cart booted normally (marker %02x)", game_marker & 0xFF);
        if (prod == 2 && !cart.cfg.gb_writes_flash) {
            check(cart.n_autoselect == 0 && cart.n_gb_writes_lost > 0 && cart.n_prog == 0 && cart.n_erase == 0,
                  "reproduces unit 1: the GB-timed ID cycles (%u writes) never reach a ModRetro cart's flash", cart.n_gb_writes_lost);
            check(cart.fs == 0, "the chip is in read mode");
        } else if (prod == 2) {
            check(cart.n_autoselect >= 1 && cart.n_prog == 0 && cart.n_erase == 0 && cart.n_chip_erase == 0,
                  "flash-ID build: autoselect %u, CFI %u, resets %u; no program / erase", cart.n_autoselect, cart.n_cfi, cart.n_resets);
            check(cart.fs == 0, "the chip is back in read mode");
        } else
            check(cart.n_autoselect == 0 && cart.n_prog == 0 && cart.n_erase == 0,
                  "read-only: no flash command reached the chip (autoselect %u; %u plain MBC register writes seen by the chip)",
                  cart.n_autoselect, cart.n_stray);
        check(flash_eq(orig_chip, 0, k.size), "flash unchanged");
        check(!memcmp(cart.ram, orig_ram, CF_RAM_SIZE), "FRAM unchanged");
        FILE *f = fopen("orig_chip.bin", "wb"); fwrite(orig_chip, 1, k.size, f); fclose(f);
        f = fopen("orig_ram.bin", "wb"); fwrite(orig_ram, 1, CF_RAM_SIZE, f); fclose(f);
        printf("  wrote orig_chip.bin / orig_ram.bin for the receiver-side comparison\n");
    }
    printf("  stats: %.1f s emulated, cart writes %u, window requests %u\n", t_us / 1e6, n_cart_writes_total - w0, E.n_requests);
    return 0;
}

int main(int argc, char **argv)
{
    const char *sc = "install";
    const char *usb_tool = NULL;
    snprintf(bootrom_path, sizeof bootrom_path, "%s", "../sm83/build/netboot_sameboy.bin");
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--scenario") && i + 1 < argc) sc = argv[++i];
        else if (!strcmp(argv[i], "--receiver") && i + 1 < argc) receiver_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--loss") && i + 1 < argc) loss_pct = atof(argv[++i]);
        else if (!strcmp(argv[i], "--fpga-bpms") && i + 1 < argc) fpga_bpms = atof(argv[++i]);
        else if (!strcmp(argv[i], "--boot") && i + 1 < argc) snprintf(bootrom_path, sizeof bootrom_path, "%s", argv[++i]);
        else if (!strcmp(argv[i], "-v")) verbose++;
        else if (!strcmp(argv[i], "--esp-up") && i + 1 < argc) esp_up_ms = (uint32_t)(atof(argv[++i]) * 1000);
        else if (!strcmp(argv[i], "--wifi-up") && i + 1 < argc) wifi_up_ms = (uint32_t)(atof(argv[++i]) * 1000);
        else if (!strcmp(argv[i], "--usb-tool") && i + 1 < argc) usb_tool = argv[++i];
        else if (!strcmp(argv[i], "--usb-sync")) { usb_sync = 1; CON_TXBUF = 256; }   /* old: printf, no TX buffer */
        else if (!strcmp(argv[i], "--con-txbuf") && i + 1 < argc) CON_TXBUF = (size_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--usb-answer-ms") && i + 1 < argc) usb_answer_ms = atof(argv[++i]);
        else if (!strcmp(argv[i], "--con-rate") && i + 1 < argc) con_rate = atof(argv[++i]);
        else if (!strcmp(argv[i], "--ram-kib") && i + 1 < argc) opt_ram_kib = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--gb-writes-flash")) opt_gb_writes_flash = 1;
        else if (!strcmp(argv[i], "--no-engine")) opt_no_engine = 1;
        else if (!strcmp(argv[i], "--usb-wait")) opt_usb_wait = 1;
        else if (!strcmp(argv[i], "--stg-erase-fail")) opt_stg_erase_fail = 1;
        else if (!strcmp(argv[i], "--rx-repl")) opt_rx_repl = 1;
        else if (!strcmp(argv[i], "--rx-loss") && i + 1 < argc) opt_rx_loss = atof(argv[++i]);
        else if (!strcmp(argv[i], "--rx-kill") && i + 1 < argc) { opt_rx_kill_off = atol(argv[++i]); char *cm = strchr(argv[i], ','); opt_rx_kill_n = cm ? atoi(cm + 1) : 3; }
        else if (!strcmp(argv[i], "--usb-chunk") && i + 1 < argc) opt_usb_chunk = atoi(argv[++i]);   /* 512: the chunk size of 0.18.0-rc1 */
        else if (!strcmp(argv[i], "--legacy-no-hold")) opt_legacy_no_hold = 1;   /* the firmware before the overlay hold */
        else if (!strcmp(argv[i], "--ram-timeout-ms") && i + 1 < argc) opt_ram_timeout_ms = atof(argv[++i]);
        else if (!strcmp(argv[i], "--chip") && i + 1 < argc) { i++; opt_chip1681 = !strcmp(argv[i], "1681"); }
        else if (!strcmp(argv[i], "--wp-low")) opt_wp_low = 1;
        else if (!strcmp(argv[i], "--xs")) opt_xs = 1;                       /* cartspeed1: an 18.26 FPGA (express stream) */
        else if (!strcmp(argv[i], "--xs-lose") && i + 1 < argc) opt_xs_lose = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--xs-flip") && i + 1 < argc) opt_xs_flip = atol(argv[++i]);
        else if (!strcmp(argv[i], "--xs-timing") && i + 1 < argc) opt_xs_timing = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--xs-postloss") && i + 1 < argc) opt_xs_postloss = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--wp-garble")) { opt_wp_low = 1; opt_wp_garble = 1; }
    }
    xs_reset_all();
    printf("scenario %s%s%s%s\n", sc, opt_chip1681 ? " (SST39VF1681)" : "", opt_wp_low ? " (WP# low)" : "", opt_xs ? " (FPGA 18.26: express stream)" : "");
    if (esp_up_ms) { esp_is_up = 0; printf("  slow ESP32: window service from %.1f s, Wi-Fi %s\n", esp_up_ms / 1000.0,
                                             wifi_up_ms ? "later" : "never (USB only)"); }
    usb_tool_cmd = usb_tool;
    if (usb_tool && strncmp(sc, "usb_", 4) && strncmp(sc, "stg_", 4) && strncmp(sc, "ovl_", 4) && usb_spawn(usb_tool)) { fprintf(stderr, "spawn failed\n"); return 2; }
    cart_cfg_t k;
    if (!strcmp(sc, "install")) sc_install();
    else if (!strcmp(sc, "loss")) { if (loss_pct <= 0) loss_pct = 3; sc_install(); check(n_dropped > 0, "%u UART packets dropped and recovered", n_dropped); }
    else if (!strcmp(sc, "ram_engine")) sc_ram_engine(0);
    else if (!strcmp(sc, "ram_cut")) sc_ram_engine(1);
    else if (!strcmp(sc, "bootblock")) sc_bootblock();
    else if (!strncmp(sc, "ovl_", 4)) sc_overlay(sc);
    else if (!strcmp(sc, "save_rt")) sc_save_rt();
    else if (!strcmp(sc, "foreign")) sc_foreign();
    else if (!strncmp(sc, "strm_", 5)) sc_stream(sc);
    else if (!strncmp(sc, "usb_", 4) || !strncmp(sc, "stg_", 4)) sc_usb(sc);
    else if (!strcmp(sc, "gbt_diy")) sc_gbt(1);
    else if (!strcmp(sc, "gbt_modretro")) sc_gbt(0);
    else if (!strcmp(sc, "gbtest")) sc_gbtest(1);
    else if (!strcmp(sc, "gbtest_miss")) sc_gbtest(0);
    else if (!strcmp(sc, "cut_backup")) sc_cut(cut_backup, "during the backup (before any erase)");
    else if (!strcmp(sc, "cut_erase")) { cut_target_n = 1; sc_cut(cut_erase, "during the 1st sector erase (sector 1)"); }
    else if (!strcmp(sc, "cut_header")) sc_cut(cut_header, "during the header sector erase (0x0000)");
    else if (!strcmp(sc, "cut_program")) { cut_target_n = 20000; sc_cut(cut_program, "mid-program (byte 20000)"); }
    else if (!strcmp(sc, "verify_fail")) sc_verify_fail();
    else if (!strcmp(sc, "wrong_chip")) { cart_cfg_unknown(&k); sc_refused(&k, 1, "unknown chip ID 01 22", CF_D_WRONG_CHIP); }
    else if (!strcmp(sc, "gated")) { cart_cfg_sst39vf1682(&k); k.we_gated = 1; sc_refused(&k, 1, "/WE gated by the mapper: autoselect never engages", CF_D_WRONG_CHIP); }
    else if (!strcmp(sc, "unverified")) { cart_cfg_is29gl032(&k); sc_refused(&k, 0, "chip known but not verified on hardware (IS29GL032; the SST39VF1681 is allowed since 1.0.2)", CF_D_WRONG_CHIP); }
    else if (!strncmp(sc, "wp_", 3)) sc_wp(sc);
    else if (!strcmp(sc, "roundtrip")) sc_roundtrip();
    else if (!strcmp(sc, "header_mapper")) sc_header_mapper();
    else if (!strcmp(sc, "esp_reboot")) sc_esp_reboot();
    else if (!strcmp(sc, "bad_image")) sc_bad_image();
    else if (!strcmp(sc, "ram_partial")) sc_ram_partial();
    else if (!strcmp(sc, "selftest") || !strcmp(sc, "rt_tetris") || !strcmp(sc, "hw_sequence")) sc_engine_job(sc);
    else if (!strcmp(sc, "erase_timeout")) {
        cart_cfg_sst39vf1682(&k);
        k.erase_ms = 6000;                     /* longer than the engine's ~4 s erase timeout */
        setup(&k, 0);
        check(arm_job(CF_JOB_ROUNDTRIP, 0) == 0, "armed ROUNDTRIP on a chip whose erase outlasts the engine timeout");
        run(900, job_stopped);
        check(C.st.result == CF_D_FAILED, "job FAILED at the timeout (got %u)", C.st.result);
        check(cart.n_busy_writes == 0, "no write into the busy chip after the timeout (B0 would suspend the erase)");
        check(journal.bak_ok && journal.phase == CF_PH_FAILED, "backup kept, journal FAILED: restore offered");
        report_stats();
    }
    else if (!strcmp(sc, "no_engine")) { opt_no_engine = 1; cart_cfg_sst39vf1682(&k); sc_refused(&k, 0, "FPGA without the cart bus engine", CF_D_NO_ENGINE); }
    else if (!strcmp(sc, "other_cart")) sc_other_cart();
    else if (!strcmp(sc, "probe")) sc_probe(0);
    else if (!strcmp(sc, "probe_prod")) sc_probe(1);
    else if (!strcmp(sc, "probe_flashid")) sc_probe(2);
    else if (!strcmp(sc, "probe_usb")) sc_probe(3);
    else { fprintf(stderr, "unknown scenario %s\n", sc); return 2; }
    printf("%s %s (%d failed checks)\n", fails ? "FAILED" : "PASSED", sc, fails);
    return fails ? 1 : 0;
}
