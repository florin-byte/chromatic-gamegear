/* shl_sameboy.c: a "Chromatic" on the Mac (lane chromshlink1).
 *
 * SameBoy runs the real cartridge; reads/writes of FF58-FF5F go to the FPGA
 * model; behind it the real ESP32 window service, SHLINK ops and room client
 * (shl_dev.c) talk UDP to a Shiny Relay. A test drives it over a localhost
 * TCP line protocol, the same calls it makes on the app's ShinyGbcEmulator
 * (TestChromaticShlink.gd's RemoteGbc):
 *
 *   load PATH | reset | sram_get | sram_set HEX | keys MASK | step N
 *   read ADDR LEN | vram ADDR BANK LEN | frame        (hex answers)
 *   room host|mine|join CODE|room ID|leave | ticket JWT | status | quit
 *
 * Every answer is one line: "ok ...", or "err ...".
 *
 *   ./shl_sameboy --relay 127.0.0.1:PORT --control PORT [--client-id HEX] [--no-window]
 *                 [--ticket-cmd CMD] [ROM] [--frames N --room host --shot out.bmp]
 */
#define _DEFAULT_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <Core/gb.h>

#include "shl_dev.h"

static GB_gameboy_t *gb;
static uint32_t pixels[256 * 224];
static unsigned frames;
static char rom_path[1024];
static double slice;

static uint8_t on_read(GB_gameboy_t *g, uint16_t addr, uint8_t data)
{
    (void)g;
    if (addr >= NW_REG_ID && addr <= NW_REG_RSVD) {
        uint8_t v;
        if (nw_fpga_read(&DF, addr, &v)) return v;
        return 0xFF;
    }
    return data;
}

static bool on_write(GB_gameboy_t *g, uint16_t addr, uint8_t v)
{
    (void)g;
    nw_fpga_write(&DF, addr, v);
    return true;
}

static uint32_t rgb_encode(GB_gameboy_t *g, uint8_t r, uint8_t gg, uint8_t b)
{
    (void)g;
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)gg << 8) | b;
}
static void quiet_log(GB_gameboy_t *g, const char *s, GB_log_attributes_t a) { (void)g; (void)s; (void)a; }
static void vblank(GB_gameboy_t *g, GB_vblank_type_t type) { (void)g; (void)type; frames++; }

static int boot(const char *path)
{
    if (gb) GB_free(gb);
    gb = GB_init(GB_alloc(), GB_MODEL_CGB_E);
    char bootrom[512];
    snprintf(bootrom, sizeof bootrom, "%s/cgb_boot.bin", BOOTROM_DIR);
    if (getenv("SHL_BOOTROM")) snprintf(bootrom, sizeof bootrom, "%s", getenv("SHL_BOOTROM"));   /* e.g. the SHLINK boot-ROM test */
    if (GB_load_boot_rom(gb, bootrom) != 0) return -1;
    if (GB_load_rom(gb, path) != 0) return -1;
    GB_set_rgb_encode_callback(gb, rgb_encode);
    GB_set_pixels_output(gb, pixels);
    GB_set_vblank_callback(gb, vblank);
    GB_set_sample_rate(gb, 0);
    GB_set_log_callback(gb, quiet_log);
    GB_set_turbo_mode(gb, true, true);
    GB_set_read_memory_callback(gb, on_read);
    GB_set_write_memory_callback(gb, on_write);
    snprintf(rom_path, sizeof rom_path, "%s", path);
    nw_fpga_gb_reset(&DF);
    NwLink_Detach(&DL);
    frames = 0;
    return 0;
}

static void run_frames(unsigned n)
{
    const double ticks_per_ms = 8388.608;
    const unsigned until = frames + n;
    while (frames < until) {
        slice += GB_run(gb);
        while (slice >= ticks_per_ms) {
            slice -= ticks_per_ms;
            shl_dev_step_ms();
        }
        shl_dev_net();
    }
}

static int hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

/* ---- the control line protocol */
static int cfd = -1;
static char *out;
static size_t out_cap;

static void reply(const char *s)
{
    size_t n = strlen(s);
    (void)!write(cfd, s, n);
    (void)!write(cfd, "\n", 1);
}

static void reply_hex(const uint8_t *b, size_t n)
{
    if (out_cap < 2 * n + 8) { out_cap = 2 * n + 8; out = realloc(out, out_cap); }
    static const char *hx = "0123456789abcdef";
    memcpy(out, "ok ", 3);
    for (size_t i = 0; i < n; i++) { out[3 + 2 * i] = hx[b[i] >> 4]; out[4 + 2 * i] = hx[b[i] & 15]; }
    out[3 + 2 * n] = '\0';
    reply(out);
}

static void keys(unsigned mask)
{
    /* ShinyGbcEmulator: A 1, B 2, SELECT 4, START 8, RIGHT 16, LEFT 32, UP 64, DOWN 128 */
    static const GB_key_t map[8] = { GB_KEY_A, GB_KEY_B, GB_KEY_SELECT, GB_KEY_START,
                                     GB_KEY_RIGHT, GB_KEY_LEFT, GB_KEY_UP, GB_KEY_DOWN };
    for (int i = 0; i < 8; i++) GB_set_key_state(gb, map[i], (mask >> i) & 1);
}

static void command(char *line)
{
    char *sp = strchr(line, ' ');
    char *arg = sp ? sp + 1 : (char *)"";
    if (sp) *sp = '\0';
    char buf[4096];
    if (!strcmp(line, "load")) {
        reply(boot(arg) == 0 ? "ok" : "err load");
    } else if (!strcmp(line, "reset")) {
        if (!gb) { reply("err no rom"); return; }
        GB_reset(gb);
        nw_fpga_gb_reset(&DF);
        NwLink_Detach(&DL);
        reply("ok");
    } else if (!strcmp(line, "sram_get")) {
        const int n = gb ? GB_save_battery_size(gb) : 0;
        uint8_t *b = malloc((size_t)(n > 0 ? n : 1));
        if (n > 0) GB_save_battery_to_buffer(gb, b, (size_t)n);
        reply_hex(b, n > 0 ? (size_t)n : 0);
        free(b);
    } else if (!strcmp(line, "sram_set")) {
        const size_t n = strlen(arg) / 2;
        uint8_t *b = malloc(n ? n : 1);
        for (size_t i = 0; i < n; i++) b[i] = (uint8_t)(hexval(arg[2 * i]) << 4 | hexval(arg[2 * i + 1]));
        GB_load_battery_from_buffer(gb, b, n);
        free(b);
        reply("ok");
    } else if (!strcmp(line, "keys")) {
        keys((unsigned)strtoul(arg, NULL, 0));
        reply("ok");
    } else if (!strcmp(line, "step")) {
        if (!gb) { reply("err no rom"); return; }
        run_frames((unsigned)strtoul(arg, NULL, 0));
        snprintf(buf, sizeof buf, "ok %u", frames);
        reply(buf);
    } else if (!strcmp(line, "read")) {
        unsigned a = 0, n = 0;
        sscanf(arg, "%i %i", (int *)&a, (int *)&n);
        uint8_t *b = malloc(n ? n : 1);
        for (unsigned i = 0; i < n; i++) {
            const uint16_t ad = (uint16_t)(a + i);
            b[i] = (ad >= NW_REG_ID && ad <= NW_REG_RSVD) ? 0xFF : GB_safe_read_memory(gb, ad);
        }
        reply_hex(b, n);
        free(b);
    } else if (!strcmp(line, "vram")) {
        unsigned a = 0, bank = 0, n = 0;
        sscanf(arg, "%i %i %i", (int *)&a, (int *)&bank, (int *)&n);
        size_t size = 0;
        uint8_t *v = GB_get_direct_access(gb, GB_DIRECT_ACCESS_VRAM, &size, NULL);
        uint8_t *b = calloc(n ? n : 1, 1);
        for (unsigned i = 0; i < n; i++) {
            const size_t off = (size_t)bank * 0x2000 + (a + i - 0x8000);
            if (v && off < size) b[i] = v[off];
        }
        reply_hex(b, n);
        free(b);
    } else if (!strcmp(line, "frame")) {
        const unsigned w = GB_get_screen_width(gb), h = GB_get_screen_height(gb);
        uint8_t *b = malloc((size_t)w * h * 3);
        for (unsigned i = 0; i < w * h; i++) {
            b[3 * i] = (uint8_t)(pixels[i] >> 16);
            b[3 * i + 1] = (uint8_t)(pixels[i] >> 8);
            b[3 * i + 2] = (uint8_t)pixels[i];
        }
        reply_hex(b, (size_t)w * h * 3);
        free(b);
    } else if (!strcmp(line, "room")) {
        reply(shl_dev_room(arg) == 0 ? "ok" : "err room");
    } else if (!strcmp(line, "ticket")) {
        shl_dev_ticket(arg);
        reply("ok");
    } else if (!strcmp(line, "status")) {
        char js[3000];
        shl_dev_status(js, sizeof js);
        snprintf(buf, sizeof buf, "ok %s", js);
        reply(buf);
    } else if (!strcmp(line, "net")) {
        /* let the network side run for N ms of wall time with the GB paused */
        const uint32_t until = shl_dev_wall_ms() + (uint32_t)strtoul(arg, NULL, 0);
        while ((int32_t)(until - shl_dev_wall_ms()) > 0) { shl_dev_net(); usleep(1000); }
        reply("ok");
    } else {
        reply("err unknown");
    }
}

static int serve(int port)
{
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((uint16_t)port);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) < 0 || listen(ls, 1) < 0) { perror("control"); return 1; }
    fprintf(stderr, "shl_sameboy: control on 127.0.0.1:%d\n", port);
    cfd = accept(ls, NULL, NULL);
    static char lb[1 << 20];
    size_t ln = 0;
    for (;;) {
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(cfd, &rs);
        struct timeval tv = { 0, 2000 };
        const int r = select(cfd + 1, &rs, NULL, NULL, &tv);
        shl_dev_net();
        if (r <= 0) continue;
        const ssize_t n = read(cfd, lb + ln, sizeof lb - 1 - ln);
        if (n <= 0) break;
        ln += (size_t)n;
        char *nl;
        while ((nl = memchr(lb, '\n', ln)) != NULL) {
            *nl = '\0';
            if (nl > lb && nl[-1] == '\r') nl[-1] = '\0';
            if (!strcmp(lb, "quit")) { reply("ok"); return 0; }
            command(lb);
            const size_t used = (size_t)(nl + 1 - lb);
            memmove(lb, nl + 1, ln - used);
            ln -= used;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *relay = "127.0.0.1:5861", *rom = NULL, *room = NULL, *shot = NULL;
    int control = 0, window = 1;
    unsigned nframes = 0;
    uint64_t cid = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--relay") && i + 1 < argc) relay = argv[++i];
        else if (!strcmp(argv[i], "--control") && i + 1 < argc) control = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--client-id") && i + 1 < argc) cid = strtoull(argv[++i], NULL, 16);
        else if (!strcmp(argv[i], "--no-window")) window = 0;
        else if (!strcmp(argv[i], "--ticket-cmd") && i + 1 < argc) snprintf(dev_ticket_cmd, sizeof dev_ticket_cmd, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) nframes = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--room") && i + 1 < argc) room = argv[++i];
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
        else rom = argv[i];
    }
    if (cid == 0) cid = ((uint64_t)shl_dev_wall_ms() << 20) ^ (uint64_t)getpid();
    if (shl_dev_init(relay, cid, window) != 0) { fprintf(stderr, "shl_sameboy: relay %s?\n", relay); return 1; }
    if (control) return serve(control);
    if (!rom || boot(rom) != 0) { fprintf(stderr, "usage: see the header\n"); return 2; }
    for (unsigned f = 0; f < nframes; f += 10) {
        run_frames(10);
        if (room && DL.attached && f > 60) { shl_dev_room(room); room = NULL; }
    }
    char js[3000];
    shl_dev_status(js, sizeof js);
    printf("%s\n", js);
    (void)shot;
    return 0;
}
