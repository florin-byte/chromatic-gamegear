/* cf_cart.c: see cf_cart.h. */
#include "cf_cart.h"

#include <stdlib.h>
#include <string.h>

enum { FS_READ, FS_U1, FS_U2, FS_AUTO, FS_CFI, FS_PROG, FS_E80, FS_E1, FS_E2 };
enum { BK_PROG = 1, BK_ERASE = 2 };

static uint32_t rnd(cart_t *c)
{
    c->rng ^= c->rng << 13;
    c->rng ^= c->rng >> 17;
    c->rng ^= c->rng << 5;
    return c->rng;
}

static uint8_t sw(const cart_t *c, uint8_t v)
{
    if (!c->cfg.swap) return v;
    return (uint8_t)((v & 0xFC) | ((v & 1) << 1) | ((v >> 1) & 1));
}

const char *cart_mk_name(int mk)
{
    return mk == MK_MBC5 ? "MBC5" : mk == MK_MBC1 ? "MBC1" : mk == MK_MBC3 ? "MBC3" : "none";
}

static void cfi_word(uint8_t *cfi, int w, uint8_t v) { if (2 * w < 128) { cfi[2 * w] = v; cfi[2 * w + 1] = 0; } }

void cart_cfg_sst39vf1681(cart_cfg_t *k)
{
    memset(k, 0, sizeof *k);
    k->id16[0] = 0xBF; k->id16[1] = 0xC8;
    k->size = 0x200000;
    k->aaa = 1;
    k->reg[0].size = 0x10000; k->reg[0].count = 32; k->nreg = 1;
    k->erase_op = 0x30;                 /* block (64 KiB): the design's working assumption */
    k->small_op = 0x50; k->small_size = 0x1000;
    k->cfi_sst = 1;
    k->cfi[0x10] = 'Q'; k->cfi[0x11] = 'R'; k->cfi[0x12] = 'Y';
    k->cfi[0x27] = 21;
    k->prog_us = 7; k->erase_ms = 18; k->chip_erase_ms = 40;
}

void cart_cfg_sst39vf1682(cart_cfg_t *k)
{
    cart_cfg_sst39vf1681(k);
    k->id16[1] = 0xC9;
    k->protect_top = 0x10000;
}

void cart_cfg_is29gl032(cart_cfg_t *k)
{
    memset(k, 0, sizeof *k);
    k->id16[0] = 0x9D; k->id16[1] = 0x9D; k->id16[2] = 0x7E; k->id16[3] = 0x7E;
    k->size = 0x400000;
    k->aaa = 1;
    k->reg[0].size = 0x10000; k->reg[0].count = 64; k->nreg = 1;
    k->erase_op = 0x30;
    cfi_word(k->cfi, 0x10, 'Q'); cfi_word(k->cfi, 0x11, 'R'); cfi_word(k->cfi, 0x12, 'Y');
    cfi_word(k->cfi, 0x27, 22);
    cfi_word(k->cfi, 0x2C, 1);
    cfi_word(k->cfi, 0x2D, 63); cfi_word(k->cfi, 0x2E, 0); cfi_word(k->cfi, 0x2F, 0x00); cfi_word(k->cfi, 0x30, 0x01);
    k->prog_us = 10; k->erase_ms = 200; k->chip_erase_ms = 30000;
}

void cart_cfg_unknown(cart_cfg_t *k)
{
    cart_cfg_sst39vf1681(k);
    k->id16[0] = 0x01; k->id16[1] = 0x22;
}

void cart_init(cart_t *c, const cart_cfg_t *cfg, const uint8_t *img, uint32_t n, const uint8_t *ram)
{
    memset(c, 0, sizeof *c);
    c->cfg = *cfg;
    c->flash = malloc(cfg->size);
    memset(c->flash, 0xFF, cfg->size);
    if (img) memcpy(c->flash, img, n < cfg->size ? n : cfg->size);
    if (ram) memcpy(c->ram, ram, sizeof c->ram);
    c->rng = 0x12345678u;
    cart_power_on(c);
}

void cart_free(cart_t *c) { free(c->flash); c->flash = NULL; }

void cart_power_on(cart_t *c)
{
    c->n_power_on++;
    c->fs = FS_READ;
    c->busy = 0;
    c->rom_bank = 1;
    c->ram_bank = 0;
    c->mbc1_hi = 0;
    c->mbc1_mode = 0;
    c->ram_en = 0;
    c->mk = MK_MBC5;
    if (c->cfg.mapper == CM_HEADER) {
        const uint8_t t = c->flash[0x147];
        if (t >= 0x01 && t <= 0x03) c->mk = MK_MBC1;
        else if (t >= 0x0F && t <= 0x13) c->mk = MK_MBC3;
        else if (t == 0x00 || t == 0x08 || t == 0x09) c->mk = MK_NONE;
        else c->mk = MK_MBC5;            /* MBC5 types and anything unknown (e.g. erased FF) */
    }
}

static void unit_of(const cart_t *c, uint32_t fa, uint32_t *start, uint32_t *size)
{
    uint32_t a = 0;
    for (int r = 0; r < c->cfg.nreg; r++) {
        const uint32_t span = c->cfg.reg[r].size * (uint32_t)c->cfg.reg[r].count;
        if (fa < a + span) { *start = a + (fa - a) / c->cfg.reg[r].size * c->cfg.reg[r].size; *size = c->cfg.reg[r].size; return; }
        a += span;
    }
    *start = 0; *size = 0;
}

static void apply_stuck(cart_t *c, uint32_t a)
{
    if (c->has_stuck && a == c->stuck_addr) c->flash[a] &= (uint8_t)~c->stuck0;
}

static void complete(cart_t *c)
{
    if (c->busy_kind == BK_PROG) {
        c->flash[c->busy_addr] &= c->busy_data;
        apply_stuck(c, c->busy_addr);
    } else {
        memset(c->flash + c->busy_addr, 0xFF, c->busy_len);
        if (c->has_stuck && c->stuck_addr >= c->busy_addr && c->stuck_addr < c->busy_addr + c->busy_len) apply_stuck(c, c->stuck_addr);
    }
    c->busy = 0;
}

void cart_time(cart_t *c, double now_us)
{
    c->now_us = now_us;
    if (c->busy && now_us >= c->busy_end) complete(c);
}

int cart_busy(const cart_t *c) { return c->busy; }

void cart_power_cut(cart_t *c)
{
    c->n_power_cuts++;
    if (c->busy) {
        if (c->busy_kind == BK_PROG) {
            c->flash[c->busy_addr] &= (uint8_t)(c->busy_data | rnd(c));   /* some bits done */
        } else {
            for (uint32_t i = 0; i < c->busy_len; i++) {                   /* half-erased: anything */
                const uint32_t r = rnd(c);
                if (r & 1) c->flash[c->busy_addr + i] = 0xFF;
                else c->flash[c->busy_addr + i] &= (uint8_t)(r >> 8);
            }
        }
        c->busy = 0;
    }
    c->fs = FS_READ;
}

static uint32_t rom_fa(const cart_t *c, uint16_t addr)
{
    if (addr < 0x4000) {
        if (c->mk == MK_MBC1 && c->mbc1_mode) return ((uint32_t)(c->mbc1_hi << 5) * 0x4000u + addr) % c->cfg.size;
        return addr;
    }
    uint32_t bank;
    switch (c->mk) {
    case MK_MBC5: bank = c->rom_bank & 0x1FF; break;
    case MK_MBC1: bank = (c->rom_bank & 0x1F ? c->rom_bank & 0x1F : 1) | (uint32_t)(c->mbc1_hi << 5); break;
    case MK_MBC3: bank = c->rom_bank & 0x7F ? c->rom_bank & 0x7F : 1; break;
    default: bank = 1; break;
    }
    return (bank * 0x4000u + (addr & 0x3FFFu)) % c->cfg.size;
}

uint32_t cart_rom_offset(cart_t *c, uint16_t addr) { return rom_fa(c, addr); }
uint8_t cart_read(cart_t *c, uint16_t addr)
{
    if (addr >= 0xA000 && addr < 0xC000) {
        if (!c->ram_en) return 0xFF;
        if ((c->cfg.ram_kib && (uint32_t)(addr - 0xA000u) >= (uint32_t)c->cfg.ram_kib * 1024u) ||
            (c->cfg.ram_timeout_us > 0 && c->now_us - c->last_mbc_us > c->cfg.ram_timeout_us)) {
            c->n_ram_float_reads++;
            return (rnd(c) & 1) ? 0xED : 0xAD;      /* the floating bus seen on unit 1 */
        }
        return c->ram[(uint32_t)(c->ram_bank & 3) * 0x2000u + (addr - 0xA000u)];
    }
    if (addr >= 0x8000) return 0xFF;
    const uint32_t fa = rom_fa(c, addr);
    uint8_t v;
    if (c->busy) {
        c->toggle ^= 1;
        v = (uint8_t)((c->toggle ? 0x40 : 0) | (c->busy_kind == BK_PROG ? (~c->busy_data & 0x80) : 0));
    } else if (c->fs == FS_AUTO) {
        v = (fa & 0xFF) < 16 ? c->cfg.id16[fa & 0xFF] : 0x00;
    } else if (c->fs == FS_CFI) {
        v = c->cfg.cfi[fa & 0x7F];
    } else {
        v = c->flash[fa];
        if (c->garble_at && c->garble_at == fa + 1) v ^= 0x20;   /* a fault model only (garble_ignored): the array is unchanged */
    }
    return sw(c, v);
}

static void chip_write(cart_t *c, uint32_t fa, uint8_t d)
{
    const uint32_t mask = c->cfg.aaa ? 0xFFF : 0x7FF;
    const uint32_t u1 = c->cfg.aaa ? 0xAAA : 0x555, u2 = c->cfg.aaa ? 0x555 : 0x2AA;
    const int at1 = (fa & mask) == u1, at2 = (fa & mask) == u2;
    if (c->busy) { c->n_busy_writes++; return; }
    switch (c->fs) {
    case FS_READ:
        if (d == 0xF0) return;
        if (at1 && d == 0xAA) { c->fs = FS_U1; return; }
        if (!c->cfg.cfi_sst && d == 0x98 && (fa & 0xFF) == (c->cfg.aaa ? 0xAA : 0x55)) { c->fs = FS_CFI; c->n_cfi++; return; }
        c->n_stray++;
        return;
    case FS_AUTO:
    case FS_CFI:
        if (d == 0xF0) { c->fs = FS_READ; c->n_resets++; return; }
        if (at1 && d == 0xAA) { c->fs = FS_U1; return; }
        return;
    case FS_U1:
        c->fs = (at2 && d == 0x55) ? FS_U2 : FS_READ;
        return;
    case FS_U2:
        c->fs = FS_READ;
        if (!at1) return;
        if (d == 0x90) { c->fs = FS_AUTO; c->n_autoselect++; }
        else if (d == 0xA0) c->fs = FS_PROG;
        else if (d == 0x80) c->fs = FS_E80;
        else if (d == 0x98 && c->cfg.cfi_sst) { c->fs = FS_CFI; c->n_cfi++; }
        else if (d == 0xF0) c->n_resets++;
        return;
    case FS_PROG:
        c->fs = FS_READ;
        if (c->cfg.protect_top && fa >= c->cfg.size - c->cfg.protect_top) { c->n_prot_ignored++; return; }
        if (c->cfg.protect_bottom && fa < c->cfg.protect_bottom) { c->n_prot_ignored++; c->n_prot_bottom_ignored++; if (c->cfg.garble_ignored) c->garble_at = fa + 1; return; }
        c->busy = 1; c->busy_kind = BK_PROG; c->busy_addr = fa; c->busy_data = d;
        c->busy_end = c->now_us + c->cfg.prog_us;
        c->n_prog++;
        return;
    case FS_E80: c->fs = (at1 && d == 0xAA) ? FS_E1 : FS_READ; return;
    case FS_E1:  c->fs = (at2 && d == 0x55) ? FS_E2 : FS_READ; return;
    case FS_E2: {
        c->fs = FS_READ;
        uint32_t s0 = 0, sz = 0;
        double ms = c->cfg.erase_ms;
        if (d == c->cfg.erase_op) unit_of(c, fa, &s0, &sz);
        else if (c->cfg.small_op && d == c->cfg.small_op) { sz = c->cfg.small_size; s0 = fa / sz * sz; ms /= 4; }
        else if (d == 0x10 && at1) { s0 = 0; sz = c->cfg.size; ms = c->cfg.chip_erase_ms; c->n_chip_erase++; }
        if (!sz) return;
        if (c->cfg.protect_top && s0 + sz > c->cfg.size - c->cfg.protect_top) {
            if (sz == c->cfg.size) { sz = c->cfg.size - c->cfg.protect_top; }   /* chip erase skips the block */
            else { c->n_prot_ignored++; return; }
        }
        if (c->cfg.protect_bottom && s0 < c->cfg.protect_bottom) {
            if (sz == c->cfg.size) { s0 = c->cfg.protect_bottom; sz -= c->cfg.protect_bottom; }   /* chip erase skips the block */
            else { c->n_prot_ignored++; c->n_prot_bottom_ignored++; return; }
        }
        c->busy = 1; c->busy_kind = BK_ERASE; c->busy_addr = s0; c->busy_len = sz;
        c->busy_end = c->now_us + ms * 1000.0;
        c->n_erase++;
        return;
    }
    }
}

static void mapper_write(cart_t *c, uint16_t addr, uint8_t v);
void cart_write_engine(cart_t *c, uint16_t addr, uint8_t v)
{
    if (addr >= 0x8000) { cart_write(c, addr, v); return; }
    const uint32_t fa = rom_fa(c, addr);
    if (!c->cfg.we_gated) chip_write(c, fa, sw(c, v));
    mapper_write(c, addr, v);
}

void cart_write(cart_t *c, uint16_t addr, uint8_t v)
{
    if (addr >= 0xA000 && addr < 0xC000) {
        if (!c->ram_en) return;
        if ((c->cfg.ram_kib && (uint32_t)(addr - 0xA000u) >= (uint32_t)c->cfg.ram_kib * 1024u) ||
            (c->cfg.ram_timeout_us > 0 && c->now_us - c->last_mbc_us > c->cfg.ram_timeout_us)) return;
        c->ram[(uint32_t)(c->ram_bank & 3) * 0x2000u + (addr - 0xA000u)] = v;
        return;
    }
    if (addr >= 0x8000) return;
    c->last_mbc_us = c->now_us;
    const uint32_t fa = rom_fa(c, addr);     /* the address lines as the flash sees them in this cycle */
    if (!c->cfg.we_gated && c->cfg.gb_writes_flash) chip_write(c, fa, sw(c, v));
    else if (!c->cfg.gb_writes_flash) c->n_gb_writes_lost++;
    mapper_write(c, addr, v);
}

static void mapper_write(cart_t *c, uint16_t addr, uint8_t v)
{
    switch (c->mk) {                         /* and the mapper latches the same write */
    case MK_MBC5:
        if (addr < 0x2000) c->ram_en = (v & 0x0F) == 0x0A;
        else if (addr < 0x3000) c->rom_bank = (uint16_t)((c->rom_bank & 0x100) | v);
        else if (addr < 0x4000) c->rom_bank = (uint16_t)((c->rom_bank & 0xFF) | ((v & 1) << 8));
        else if (addr < 0x6000) c->ram_bank = v & 0x0F;
        break;
    case MK_MBC1:
        if (addr < 0x2000) c->ram_en = (v & 0x0F) == 0x0A;
        else if (addr < 0x4000) c->rom_bank = v & 0x1F;
        else if (addr < 0x6000) { c->mbc1_hi = v & 3; c->ram_bank = v & 3; }
        else c->mbc1_mode = v & 1;
        break;
    case MK_MBC3:
        if (addr < 0x2000) c->ram_en = (v & 0x0F) == 0x0A;
        else if (addr < 0x4000) c->rom_bank = v & 0x7F;
        else if (addr < 0x6000) c->ram_bank = v & 0x0F;
        break;
    default:
        break;
    }
}
