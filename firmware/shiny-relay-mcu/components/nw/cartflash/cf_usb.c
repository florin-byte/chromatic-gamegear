/* cf_usb.c: see cf_usb.h (lane cartflash1, protocol CBU1). */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cf_usb.h"
#include "cf_sha256.h"
#if defined(ESP_PLATFORM)
#include "mbedtls/sha256.h"
#endif

static void say(cf_usb_t *u, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void say(cf_usb_t *u, const char *fmt, ...)
{
    char b[220];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (u->line) u->line(u->user, b);
}

/* the three data buffers of a write over the console: made when one starts, gone when it ends */
static int bufs(cf_usb_t *u)
{
    if (u->mem) return 1;
    u->mem = (uint8_t *)malloc(3u * CF_USB_CHUNK_MAX + 16u);
    if (!u->mem) return 0;
    u->buf = u->mem; u->nbuf = u->mem + CF_USB_CHUNK_MAX; u->rx = u->mem + 2u * CF_USB_CHUNK_MAX;
    return 1;
}
static void bufs_free(cf_usb_t *u)
{
    free(u->mem);
    u->mem = u->buf = u->nbuf = u->rx = NULL;
    u->have = 0; u->next_have = 0; u->rx_n = 0;
}

void cf_usb_init(cf_usb_t *u, cf_esp_t *esp, void (*line)(void *, const char *), void *user)
{
    memset(u, 0, sizeof *u);
    u->esp = esp;
    u->line = line;
    u->user = user;
}

static int b64v(int ch)
{
    return ch >= 'A' && ch <= 'Z' ? ch - 'A' : ch >= 'a' && ch <= 'z' ? ch - 'a' + 26 : ch >= '0' && ch <= '9' ? ch - '0' + 52 :
           ch == '+' ? 62 : ch == '/' ? 63 : -1;
}
static int b64dec(const char *s, uint8_t *o, int cap)
{
    int n = 0, bits = 0;
    uint32_t acc = 0;
    for (; *s && *s != '='; s++) {
        const int v = b64v(*s);
        if (v < 0) return -1;
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) { bits -= 8; if (n >= cap) return -1; o[n++] = (uint8_t)(acc >> bits); }
    }
    return n;
}

static void want_say(cf_usb_t *u, uint32_t off, uint16_t n) { u->n_wants++; say(u, "CBU1 want %u %u%s", (unsigned)off, (unsigned)n, u->bin ? " bin" : ""); }

/* Binary mode: the chunk that will most likely be asked for next (the one right behind, the
   same length, inside the same 16 KiB bank and the image) is asked for now. A wrong guess costs
   one frame that is dropped. */
static void ask_ahead(cf_usb_t *u, uint32_t off, uint16_t n)
{
    const uint32_t nx = off + n;
    u->next_open = 0; u->next_have = 0;
    if (!u->bin || n != CF_USB_BIN_CHUNK || (nx & 0x3FFFu) == 0 || nx + n > u->limit) return;
    u->next_off = nx; u->next_n = n; u->next_open = 1;
    u->want_t = u->now; u->bin_t = u->now;
    want_say(u, nx, n);
}

int cf_usb_chunk(cf_usb_t *u, uint32_t off, uint16_t n, uint8_t *dst)
{
    if (u->have && u->want_off == off && u->want_n == n) {
        memcpy(dst, u->buf, n);
        u->have = 0;
        u->want_open = 0;
        ask_ahead(u, off, n);
        return 1;
    }
    if (u->next_open && u->next_off == off && u->next_n == n) {
        /* the chunk asked for ahead is the one wanted now */
        if (u->next_have) {
            memcpy(dst, u->nbuf, n);
            u->next_have = 0; u->next_open = 0;
            ask_ahead(u, off, n);
            return 1;
        }
        /* still on its way: it becomes the open want (no second `want` line: one is out) */
        u->want_open = 1; u->have = 0; u->want_off = off; u->want_n = n;
        u->next_open = 0;
        return 0;
    }
    if (n > CF_USB_CHUNK_MAX) return -1;
    if (!u->want_open || u->want_off != off || u->want_n != n || u->now - u->want_t >= CF_USB_WANT_MS) {
        if (!u->want_open || u->want_off != off) u->bin_t = u->now;      /* a new chunk is asked for: its 30 s start now */
        u->want_open = 1; u->have = 0;
        u->next_open = 0; u->next_have = 0;                               /* the guess, if any, was wrong */
        u->want_off = off; u->want_n = n; u->want_t = u->now;
        want_say(u, off, n);
    }
    return 0;
}

static void want_again(cf_usb_t *u)
{
    if (u->want_open && !u->have) { u->want_t = u->now; want_say(u, u->want_off, u->want_n); }
    else if (u->next_open && !u->next_have) { u->want_t = u->now; want_say(u, u->next_off, u->next_n); }
}

void cf_usb_rx(cf_usb_t *u, const uint8_t *b, uint32_t n)
{
    static const uint8_t magic[4] = { 0xA5, 'C', 'B', 'D' };
    if (!bufs(u)) return;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t ch = b[i];
        if (u->rx_n < 4) {                                    /* hunting for the magic */
            if (ch == magic[u->rx_n]) u->rx_n++;
            else u->rx_n = ch == magic[0] ? 1 : 0;
            if (u->rx_n == 4) memcpy(u->rx, magic, 4);
            continue;
        }
        u->rx[u->rx_n++] = ch;
        if (u->rx_n < 10) continue;
        const uint32_t len = (uint32_t)(u->rx[8] | u->rx[9] << 8);
        if (len == 0 || len > CF_USB_CHUNK_MAX) { u->rx_n = 0; u->n_bad_frames++; want_again(u); continue; }
        if (u->rx_n < 14u + len) continue;
        const uint32_t off = (uint32_t)u->rx[4] | (uint32_t)u->rx[5] << 8 | (uint32_t)u->rx[6] << 16 | (uint32_t)u->rx[7] << 24;
        const uint8_t *e = u->rx + 10 + len;
        const uint32_t crc = (uint32_t)e[0] | (uint32_t)e[1] << 8 | (uint32_t)e[2] << 16 | (uint32_t)e[3] << 24;
        u->rx_n = 0;
        if (cf_crc32(0, u->rx + 4, 6u + len) != crc) {
            u->n_bad_frames++;
            want_again(u);                                    /* damaged on the way: the same again, at once */
            continue;
        }
        if (u->next_open && !u->next_have && off == u->next_off && len == u->next_n) {
            memcpy(u->nbuf, u->rx + 10, len);                 /* the chunk asked for ahead */
            u->next_have = 1;
            u->n_chunks++; u->n_frames++;
            u->bin_t = u->now;
            continue;
        }
        if (!u->want_open || off != u->want_off || len != u->want_n) {
            /* a whole frame, but not the one asked for now: a second copy of one already taken
               (the `want` was repeated while its answer was on the way). Dropped WITHOUT asking
               again: asking would bring the next duplicate, and so on for the whole file. */
            u->n_dup_frames++;
            continue;
        }
        memcpy(u->buf, u->rx + 10, len);
        u->have = 1;
        u->n_chunks++; u->n_frames++;
        u->bin_t = u->now;
    }
}

int cf_usb_bin_active(cf_usb_t *u)
{
    if (!u->bin) return 0;
    if (!u->job) { u->bin = 0; return 0; }                    /* the job ended (its done / fail and `ready` are out) */
    /* only a chunk that was asked for and did not come counts (an erase or the read-back of the
       cart can take minutes with nothing to send) */
    if (u->want_open && u->now - u->bin_t > (u->bin_idle_ms ? u->bin_idle_ms : CF_USB_BIN_IDLE_MS)) { u->bin = 0; u->rx_n = 0; say(u, "CBU1 text"); return 0; }
    return 1;
}

void cf_usb_hold(cf_usb_t *u)
{
    if (u->held || !u->net_hold) return;
    u->held = 1;
    u->n_holds++;
    u->net_hold(u->user, 1);
}

/* the words of every heap refusal: what there is and what is needed (the page shows them) */
static void busy_mem(cf_usb_t *u, uint32_t need)
{
    say(u, "CBU1 fail busy memory (free %u, needs %u)", (unsigned)(u->heap_free ? u->heap_free(u->user) : 0), (unsigned)need);
}

static int arm(cf_usb_t *u, cf_job_t *j)
{
    cf_usb_hold(u);                                   /* the radio's heap first (unit 2 ONLINE, usb-cart-5) */
    if (u->heap_ok && !u->heap_ok(u->user)) { busy_mem(u, u->heap_need); return 1; }
    j->max_retries = 2;
    j->gb_timed = (uint8_t)u->gb_timed;
    u->esp->st.result = 0xEE;
    const int r = cf_esp_arm(u->esp, j);
    if (r) { say(u, "CBU1 fail refused %d", r); return 1; }
    u->job = j->job;
    u->want_open = 0; u->have = 0;
    return 0;
}

int cf_usb_cmd(cf_usb_t *u, int argc, char **argv)
{
    cf_esp_t *c = u->esp;
    const char *w = argc > 0 ? argv[0] : "";
    const int writing = u->job == CF_JOB_STREAM || u->job == CF_JOB_USBRAM;
    int bin = 0;
    if (u->mem && !u->job && !c->armed && !c->done_pending) bufs_free(u);     /* a console write is over */
    if (!strcmp(w, "write") && argc >= 3 && !strcmp(argv[argc - 1], "bin")) { bin = 1; argc--; }
    if (!strcmp(w, "d")) {
        /* One chunk: offset, base64, crc32 of the bytes. ANY line that begins with "d" is a
           data line: if it is not exactly the chunk asked for (short, damaged on the wire, wrong
           offset, wrong crc) it is refused and the same chunk asked for again. It is never
           taken for a command (0.18.0-rc1 let a damaged one fall through: "fail busy"). */
        /* NO buffer on this stack: this runs in the console task (4 KB of stack in the release
           build; usb-cart-1 had two 1 KiB buffers here and overflowed it on every command). A
           text line is decoded into nbuf, which only binary mode uses otherwise. */
        if (!bufs(u)) { busy_mem(u, 3u * CF_USB_CHUNK_MAX + 16u); return 1; }
        uint8_t *const tmp = u->nbuf;
        char *end = NULL;
        const int shaped = argc == 4 && !u->bin && strlen(argv[3]) == 8;
        const int n = shaped ? b64dec(argv[2], tmp, (int)CF_USB_CHUNK_MAX) : -1;
        const uint32_t off = shaped ? (uint32_t)strtoul(argv[1], &end, 10) : 0;
        const int off_ok = shaped && end && *end == 0;
        const uint32_t crc = shaped ? (uint32_t)strtoul(argv[3], &end, 16) : 0;
        const int whole = off_ok && n > 0 && end && *end == 0 && cf_crc32(0, tmp, (uint32_t)n) == crc;
        if (whole && u->want_open && off == u->want_off && n == (int)u->want_n) {
            memcpy(u->buf, tmp, (size_t)n);
            u->have = 1;
            u->n_chunks++;
            return 0;
        }
        /* a whole, undamaged line for another offset is a second copy of one already taken:
           dropped without asking again (see cf_usb_rx). Anything else: the same chunk again. */
        if (whole && (!u->want_open || off != u->want_off)) { u->n_dup_frames++; return 0; }
        u->n_bad_chunks++;
        want_again(u);
        return 0;
    }
    u->last_cmd_t = u->now;
    if (!strcmp(w, "baud") && argc >= 2) {
        const uint32_t b = (uint32_t)strtoul(argv[1], NULL, 10);
        /* Removed (0.18.0-rc1): on a unit nothing got through after the switch (2026-10-11), and
           the console is not the limit: every cart byte also crosses the FPGA-to-ESP32 link at
           115200. The FPGA's USB bridge knows 115200, 460800 and 921600 only. */
        (void)b;
        say(u, "CBU1 fail baud");
        return 0;
    }
    if (!strcmp(w, "status")) {
        char id[33], t[17];
        uint8_t src;
        /* 1.0.3: fpga=MAJ.MIN of the loaded FPGA image (when known), so the cart page can skip its
           own engine load on a unit whose flash image is the page's engine (18.28) */
        char fv[24] = "";
        const unsigned v = c->h.fpga_ver ? c->h.fpga_ver(c->h.user) : 0u;
        if (v) snprintf(fv, sizeof fv, " fpga=%u.%u", (v >> 8) & 0xFFu, v & 0xFFu);
        if (c->armed) say(u, "CBU1 status busy job=%u%s", c->j.job, fv);
        else if (cf_esp_stream_pending(c, id, t, &src)) say(u, "CBU1 status unfinished id=%s src=%s size=%u crc32=%08x%s title=\"%s\"", id, src ? "usb" : "wifi",
                                                             (unsigned)c->j.strm_size, (unsigned)c->j.strm_crc32, fv, t);
        else say(u, "CBU1 status idle%s", fv);
        return 0;
    }
    if (!strcmp(w, "cancel")) {
        if (c->armed) { say(u, "CBU1 busy job=%u", c->j.job); return 0; }
        {
            const int had = cf_esp_stream_pending(c, NULL, NULL, NULL);
            cf_esp_stream_cancel(c);
            /* an unfinished write has no first block: say so in plain words (the page shows them) */
            if (had) say(u, "CBU1 note The cartridge has no game on it now. Send or restore one.");
        }
        say(u, "CBU1 status idle");
        return 0;
    }
    if (c->armed || c->done_pending) {
        /* The same write again while it is still running (the page was reloaded, the tool
           started again): carry on with it. */
        if (!strcmp(w, "write") && argc >= 4 && !strcmp(argv[1], "rom") && c->j.job == CF_JOB_STREAM && c->j.strm_src == 1 &&
            (uint32_t)strtoul(argv[2], NULL, 0) == c->j.strm_size && (uint32_t)strtoul(argv[3], NULL, 16) == c->j.strm_crc32) {
            u->job = CF_JOB_STREAM;
            if (!bufs(u)) { busy_mem(u, 3u * CF_USB_CHUNK_MAX + 16u); return 1; }
            /* the chunk size of a running job does not change: binary only if it was armed so */
            u->bin = bin && c->opt.strm_chunk == CF_USB_BIN_CHUNK; u->bin_t = u->now; u->rx_n = 0;
            u->next_open = 0; u->next_have = 0;
            u->limit = c->j.job == CF_JOB_USBRAM ? CF_RAM_SIZE : c->j.strm_size;
            if (u->want_open) want_again(u);
            else say(u, "CBU1 progress rom rejoined %u %u", (unsigned)c->pos, (unsigned)c->j.strm_size);
            return 0;
        }
        if (!strcmp(w, "write") && argc == 2 && !strcmp(argv[1], "sav") && c->j.job == CF_JOB_USBRAM) {
            u->job = CF_JOB_USBRAM;
            if (!bufs(u)) { busy_mem(u, 3u * CF_USB_CHUNK_MAX + 16u); return 1; }
            u->bin = bin && c->opt.strm_chunk == CF_USB_BIN_CHUNK; u->bin_t = u->now; u->rx_n = 0;
            u->next_open = 0; u->next_have = 0;
            u->limit = c->j.job == CF_JOB_USBRAM ? CF_RAM_SIZE : c->j.strm_size;
            want_again(u);
            return 0;
        }
        /* Anything else while a job runs is not an error to stop on: wait and ask again. During
           a write it is most likely a damaged data line: the chunk is asked for again. */
        u->n_garbage++;
        if (writing && u->want_open) { u->want_t = u->now; say(u, "CBU1 want %u %u%s", (unsigned)u->want_off, (unsigned)u->want_n, u->bin ? " bin" : ""); }
        else say(u, "CBU1 busy job=%u", c->j.job);
        return 0;
    }
    cf_job_t j;
    memset(&j, 0, sizeof j);
    if (!strcmp(w, "info")) {
        j.job = CF_JOB_USBREAD; j.rd_what = 2;
        return arm(u, &j);
    }
    if (!strcmp(w, "read") && argc >= 2) {
        j.job = CF_JOB_USBREAD;
        j.rd_what = !strcmp(argv[1], "sav") ? 1 : 0;
        if (argc >= 3 && !strcmp(argv[argc - 1], "skipblank")) { j.rd_skip = 1; argc--; }
        if (argc >= 3) j.rd_off = (uint32_t)strtoul(argv[2], NULL, 0);
        if (argc >= 4) j.rd_len = (uint32_t)strtoul(argv[3], NULL, 0);
        return arm(u, &j);
    }
    if (!strcmp(w, "write") && argc >= 2 && !strcmp(argv[1], "sav")) {
        j.job = CF_JOB_USBRAM;
        j.strm_src = 1; j.strm_chunk = bin ? CF_USB_BIN_CHUNK : u->chunk ? u->chunk : CF_USB_RAM_CHUNK;
        if (!bufs(u)) { busy_mem(u, 3u * CF_USB_CHUNK_MAX + 16u); return 1; }
        if (arm(u, &j)) { bufs_free(u); return 1; }
        u->bin = bin; u->bin_t = u->now; u->rx_n = 0;
        u->next_open = 0; u->next_have = 0;
        u->limit = j.job == CF_JOB_USBRAM ? CF_RAM_SIZE : j.strm_size;
        return 0;
    }
    if (!strcmp(w, "write") && argc >= 4 && !strcmp(argv[1], "rom")) {
        j.job = CF_JOB_STREAM;
        j.strm_src = 1; j.strm_chunk = bin ? CF_USB_BIN_CHUNK : u->chunk ? u->chunk : CF_USB_CHUNK;
        j.ram_clear = 0;                                  /* a restore: the save is the owner's own `write sav` */
        j.strm_size = (uint32_t)strtoul(argv[2], NULL, 0);
        j.strm_crc32 = (uint32_t)strtoul(argv[3], NULL, 16);
        if (argc >= 5 && cf_hex32(argv[4], j.strm_sha) == 0) j.strm_have_sha = 1;
        snprintf(j.strm_id, sizeof j.strm_id, "usb-%08x-%u", (unsigned)j.strm_crc32, (unsigned)j.strm_size);
        snprintf(j.strm_title, sizeof j.strm_title, "USB RESTORE");
        if (!bufs(u)) { busy_mem(u, 3u * CF_USB_CHUNK_MAX + 16u); return 1; }
        if (arm(u, &j)) { bufs_free(u); return 1; }
        u->bin = bin; u->bin_t = u->now; u->rx_n = 0;
        u->next_open = 0; u->next_have = 0;
        u->limit = j.job == CF_JOB_USBRAM ? CF_RAM_SIZE : j.strm_size;
        return 0;
    }
    if (!strcmp(w, "stage") && argc >= 2 && u->stage_read) {
        /* nothing on the cart is touched here: this is the proof that the data is right before any erase */
        const int sav = !strcmp(argv[1], "sav");
        u->st_cmds++;
        if (u->stage_room && u->stage_room(u->user)) { say(u, "CBU1 fail stage code=31 (this firmware is too big for the staging area)"); return 0; }
        if (u->st_clearing || (u->stage_busy && u->stage_busy(u->user) > 0)) { say(u, "CBU1 busy clearing"); return 0; }
        uint32_t size = sav ? CF_RAM_SIZE : (argc >= 3 ? (uint32_t)strtoul(argv[2], NULL, 10) : 0);
        const char *crc_s = sav ? (argc >= 3 ? argv[2] : "") : (argc >= 4 ? argv[3] : "");
        uint8_t sha[32];
        const int have_sha = !sav && argc >= 5 && cf_hex32(argv[4], sha) == 0;
        if (strlen(crc_s) != 8 || (!sav && !have_sha) || (strcmp(argv[1], "rom") && !sav)) { say(u, "CBU1 fail stage code=32 (usage)"); return 0; }
        if (!sav && (size < 0x8000u || size > CF_STAGE_ROM_MAX)) { say(u, "CBU1 fail stage code=33 (size: 32768 to 2031616)"); return 0; }
        const uint32_t want = (uint32_t)strtoul(crc_s, NULL, 16);
        uint32_t crc = 0;
        /* cartspeed2: 4 KiB reads, the table crc32 and (ESP32) the SHA hardware: 2 MiB in about a second (was 5.4 s) */
#if defined(ESP_PLATFORM)
        mbedtls_sha256_context h;
        mbedtls_sha256_init(&h);
        (void)mbedtls_sha256_starts(&h, 0);
#else
        cf_sha256_t h;
        cf_sha256_init(&h);
#endif
        enum { NB = 4096 };
        uint8_t *b = (uint8_t *)malloc(NB);             /* not static (heap for good), not on the console task's stack */
        uint32_t nb = NB;
        if (!b) { b = (uint8_t *)malloc(512); nb = 512; }
        if (!b) {
#if defined(ESP_PLATFORM)
            mbedtls_sha256_free(&h);
#endif
            busy_mem(u, 512); return 0;
        }
        int rd_bad = 0;
        for (uint32_t o = 0; o < size; o += nb) {
            const uint32_t k = size - o < nb ? size - o : nb;
            if (u->stage_read(u->user, sav, o, b, k)) { rd_bad = 1; break; }
            crc = cf_crc32(crc, b, k);
#if defined(ESP_PLATFORM)
            if (!sav) (void)mbedtls_sha256_update(&h, b, k);
#else
            if (!sav) cf_sha256_update(&h, b, k);
#endif
        }
        free(b);
        uint8_t d[32];
#if defined(ESP_PLATFORM)
        if (!sav && !rd_bad) (void)mbedtls_sha256_finish(&h, d);
        mbedtls_sha256_free(&h);
#else
        if (!sav && !rd_bad) cf_sha256_final(&h, d);
#endif
        if (rd_bad) { say(u, "CBU1 fail stage code=34 (flash read)"); return 0; }
        if (crc != want || (!sav && memcmp(d, sha, 32))) {
            if (sav) u->st_sav = 0; else u->st_rom = 0;
            say(u, "CBU1 fail stage code=35 (%s: the staged data is not the file: crc32 %08x)", sav ? "sav" : "rom", (unsigned)crc);
            return 0;
        }
        if (sav) { u->st_sav = 1; say(u, "CBU1 staged sav %08x", (unsigned)crc); }
        else { u->st_rom = 1; u->st_size = size; u->st_crc = crc; memcpy(u->st_sha, sha, 32); say(u, "CBU1 staged rom %u %08x", (unsigned)size, (unsigned)crc); }
        return 0;
    }
    if (!strcmp(w, "install") && u->stage_read) {
        int fast = 0, nosav = 0, classic = 0, tm = 2;   /* cartspeed1: express bus timing 2 (any verify error: 0) */
        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "fast")) fast = 1; else if (!strcmp(argv[i], "nosav")) nosav = 1;
            else if (!strcmp(argv[i], "classic")) classic = 1;                 /* cartspeed1: never the express stream */
            else if (!strncmp(argv[i], "timing=", 7)) tm = atoi(argv[i] + 7) & 3;   /* cartspeed1: express bus timing (bench) */
        }
        u->st_cmds++;
        if (u->st_clearing || (u->stage_busy && u->stage_busy(u->user) > 0)) { say(u, "CBU1 busy clearing"); return 0; }
        if (!u->st_rom) { say(u, "CBU1 fail install code=36 (stage rom first)"); return 0; }
        j.job = CF_JOB_STREAM;
        j.strm_src = 2; j.strm_chunk = CF_PROG_CHUNK;
        j.ram_clear = (u->st_sav && !nosav) ? 2 : 0;       /* the staged save, or the cart's save left alone */
        j.fast_verify = (uint8_t)fast;
        j.xs_off = (uint8_t)classic; j.xs_timing = (uint8_t)tm;
        j.strm_size = u->st_size; j.strm_crc32 = u->st_crc;
        memcpy(j.strm_sha, u->st_sha, 32); j.strm_have_sha = 1;
        snprintf(j.strm_id, sizeof j.strm_id, "stg-%08x-%u", (unsigned)j.strm_crc32, (unsigned)j.strm_size);
        snprintf(j.strm_title, sizeof j.strm_title, "USB INSTALL");
        if (arm(u, &j)) return 1;
        u->st_fast = fast;
        u->tp_sp = 0;
        say(u, "CBU1 installing rom %u %08x verify=%s save=%s", (unsigned)j.strm_size, (unsigned)j.strm_crc32, fast ? "fast" : "full", j.ram_clear ? "staged" : "kept");
        return 0;
    }
    if (!strcmp(w, "clear") && u->stage_erase) {
        u->st_cmds++;
        if (u->st_clearing || (u->stage_busy && u->stage_busy(u->user) > 0)) { say(u, "CBU1 busy clearing"); return 0; }
        {
            uint8_t src = 0;
            if (cf_esp_stream_pending(c, NULL, NULL, &src)) {
                cf_esp_stream_cancel(c);
                say(u, "CBU1 note The cartridge has no game on it now. Send or restore one.");
            }
        }
        u->st_rom = 0; u->st_sav = 0;
        u->stage_erase(u->user);
        u->st_clearing = 1; u->st_ready_after = 1;
        return 0;
    }
    say(u, "CBU1 fail usage");
    return 1;
}

static const char *const k_sp[] = { "check", "erase", "write", "tidy", "hash", "verify", "save", "header", "check-header" };

/* `CBU1 time rom <phase> <ms>` when a phase of the install ends: where the time goes on a unit */
static void phase_time(cf_usb_t *u, int sp1)
{
    if (sp1 == u->tp_sp) return;
    if (u->tp_sp) say(u, "CBU1 time rom %s %u", k_sp[u->tp_sp - 1], (unsigned)(u->now - u->tp_t));
    else u->tp_t0 = u->now;
    u->tp_sp = sp1; u->tp_t = u->now;
}

void cf_usb_tick(cf_usb_t *u, uint32_t now_ms)
{
    u->now = now_ms;
    cf_esp_t *c = u->esp;
    if (u->job && (c->armed || c->done_pending)) {
        /* once a second while a job runs: something for the page to show, and proof of life
           during the stretches with no data either way (erase, the read-back of the cart) */
        if (u->job == CF_JOB_STREAM && c->ph == CF_PH_STREAM && c->armed) phase_time(u, 1 + (c->sp >= 0 && c->sp <= 8 ? c->sp : 0));
        if (now_ms - u->prog_t >= 1000u) {
            u->prog_t = now_ms;
            if (c->ph == CF_PH_STREAM) {
                /* phase, position, size, and one number for a progress bar: 0-99 over the whole job */
                static const uint8_t k_lo[] = { 0, 1, 3, 68, 70, 72, 95, 97, 99 }, k_hi[] = { 1, 3, 68, 70, 72, 95, 97, 99, 99 };
                const int sp = c->sp >= 0 && c->sp <= 8 ? c->sp : 0;
                const uint32_t sz = c->j.strm_size ? c->j.strm_size : 1;
                uint32_t frac = sp == 6 ? c->pos * 100u / CF_RAM_SIZE : (c->pos > sz ? 100u : (uint32_t)((uint64_t)c->pos * 100u / sz));
                if (sp == 0 || sp == 1 || sp == 3 || sp == 7 || sp == 8) frac = 50;
                const unsigned pct = k_lo[sp] + (k_hi[sp] - k_lo[sp]) * frac / 100u;
                say(u, "CBU1 progress rom %s %u %u %u", k_sp[sp], (unsigned)c->pos, (unsigned)c->j.strm_size, pct);
            }
            else say(u, "CBU1 progress job=%u %u", u->job, (unsigned)c->pos);
        }
        return;
    }
    if (u->st_clearing) {
        /* the staged ROM and save are being erased (about 20 s for the whole area) */
        const int busy = u->stage_busy ? u->stage_busy(u->user) : 0;
        if (busy > 0) {
            if (now_ms - u->prog_t >= 1000u) { u->prog_t = now_ms; say(u, "CBU1 progress clear"); }
            return;
        }
        u->st_clearing = 0;
        if (busy < 0) say(u, "CBU1 fail clear code=37 (the copy of the ROM and save is STILL in the Chromatic's flash: send cart usb clear)");
        else say(u, "CBU1 cleared");
        if (u->st_ready_after) { u->st_ready_after = 0; say(u, "CBU1 ready"); }
        return;
    }
    if (!u->job && u->held && !c->armed && !c->done_pending && !u->st_clearing && now_ms - u->last_cmd_t > 30000u) {
        /* the cart is done with the heap: the link comes back by itself (Wi-Fi, relay, nearby) */
        u->held = 0;
        u->net_hold(u->user, 0);
    }
    if (!u->job) {
        /* a faster console speed never outlives the page that asked for it */
        if (u->baud && u->baud != 115200u && u->set_baud && now_ms - u->last_cmd_t > 10000u) {
            u->baud = 115200u;
            u->set_baud(u->user, 115200u);
        }
        return;
    }
    const uint8_t r = c->st.result;
    if (u->job == CF_JOB_STREAM) {
        if (u->tp_sp) {
            phase_time(u, 0);
            say(u, "CBU1 time rom total %u erases=%u retries=%u", (unsigned)(u->now - u->tp_t0), (unsigned)c->n_erase, (unsigned)c->n_retries);
        }
        if (r == CF_D_OK && c->opt.ram_clear == 2 && c->n_sav_fail)
            say(u, "CBU1 note The game is installed, but the save could not be written to the cartridge (%u of 32 pieces).", (unsigned)c->n_sav_fail);
        if (r == CF_D_OK) say(u, "CBU1 done rom %u %08x", (unsigned)c->j.strm_size, (unsigned)c->j.strm_crc32);
        else say(u, "CBU1 fail rom code=%u%s", r, r == 0xEE ? " (no cart writer answered: this FPGA cannot reach the cart)" :
                                                  r == CF_D_OVERLAY ? " (a game patch is active on the Chromatic: nothing was written; turn PATCHES off and try again)" :
                                                  r == CF_D_LOCKED ? " (the cartridge's first block is write-protected: nothing was erased or written)" :
                                                  r == CF_D_WPTEST ? " (the cartridge's write test did not read back cleanly: nothing was erased)" :
                                                  cf_esp_stream_pending(c, NULL, NULL, NULL) ? " unfinished (send the same command again to resume)" : "");
    } else if (r != CF_D_OK) {
        say(u, "CBU1 fail job=%u code=%u%s", u->job, r, r == 0xEE ? " (no cart writer answered: this FPGA cannot reach the cart)" :
                                                  r == CF_D_OVERLAY ? " (a game patch is active on the Chromatic: nothing was read or written; turn PATCHES off and try again)" : "");
    }
    const int staged_ok = u->job == CF_JOB_STREAM && c->opt.strm_src == 2 && r == CF_D_OK && u->stage_erase;
    u->job = 0;
    u->want_open = 0;
    u->bin = 0;
    /* (the data buffers are freed by the next `cart usb` command, in the console task that
       also fills them: freeing them here, in the cart task, could pull them from under it) */
    if (staged_ok) {
        /* installed and verified: nothing of the ROM or the save stays on the Chromatic */
        u->st_rom = 0; u->st_sav = 0;
        u->stage_erase(u->user);
        u->st_clearing = 1; u->st_ready_after = 1;
        return;
    }
    say(u, "CBU1 ready");               /* the cart is back with the console: the next command may come */
}
