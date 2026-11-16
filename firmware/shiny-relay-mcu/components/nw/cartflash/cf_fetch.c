/* cf_fetch.c: see cf_fetch.h (lane cartflash1, DRAFT). */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cf_fetch.h"
#include "cf_esp.h"      /* cf_crc32, cf_image_policy */

static void logf_(const cf_fetch_host_t *h, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void logf_(const cf_fetch_host_t *h, const char *fmt, ...)
{
    if (!h->log) return;
    char b[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    h->log(h->user, b);
}

static int size_ok(uint32_t size, uint32_t cap)
{
    if (size < 0x8000u || size > cap) return 0;
    return 1;                                        /* any length (owner 2026-10-11: any ROM); the tail of its last block stays FF */
}

/* crc32 of region [0, n); -1 on a read error */
static int region_crc(const cf_fetch_host_t *h, uint32_t n, uint32_t *crc)
{
    uint8_t b[256];
    uint32_t c = 0;
    for (uint32_t o = 0; o < n; o += sizeof b) {
        const uint32_t k = n - o < sizeof b ? n - o : sizeof b;
        if (h->read(h->user, o, b, k)) return -1;
        c = cf_crc32(c, b, k);
    }
    *crc = c;
    return 0;
}

static int jr_get(const cf_fetch_host_t *h, cf_fetch_jr_t *j)
{
    memset(j, 0, sizeof *j);
    if (!h->jr_load || h->jr_load(h->user, j, sizeof *j) || j->magic != CF_FETCH_MAGIC) { memset(j, 0, sizeof *j); return -1; }
    j->rel[sizeof j->rel - 1] = 0;
    return 0;
}

uint32_t cf_fetch_staged(const cf_fetch_host_t *h, uint32_t *crc32)
{
    cf_fetch_jr_t j;
    if (jr_get(h, &j) || !j.ok || !size_ok(j.size, h->cap)) return 0;
    uint32_t c = 0;
    if (region_crc(h, j.size, &c) || c != j.crc32) return 0;
    if (crc32) *crc32 = c;
    return j.size;
}

int cf_fetch(const cf_fetch_host_t *h, const char *rel, uint32_t size, uint32_t crc32, int max_retries)
{
    if (!rel || !rel[0] || strlen(rel) >= sizeof ((cf_fetch_jr_t *)0)->rel || !size_ok(size, h->cap)) return CF_FE_ARG;
    cf_fetch_jr_t j;
    uint32_t crc = 0, off = 0;
    const int have = jr_get(h, &j) == 0 && j.size == size && j.crc32 == crc32 && !strcmp(j.rel, rel);
    if (have && j.ok) {
        if (cf_fetch_staged(h, NULL) == size) { logf_(h, "cf: fetch: this image is already staged and verified"); return CF_FE_OK; }
    } else if (have && j.done && j.done <= size && j.done % CF_FETCH_BLK == 0) {
        if (region_crc(h, j.done, &crc) == 0) {      /* the prefix is whatever the flash holds: its crc carries on */
            off = j.done;
            logf_(h, "cf: fetch: resuming at %u of %u bytes", (unsigned)off, (unsigned)size);
        } else crc = 0;
    }
    memset(&j, 0, sizeof j);
    j.magic = CF_FETCH_MAGIC; j.size = size; j.crc32 = crc32; j.done = off; j.ok = 0;
    snprintf(j.rel, sizeof j.rel, "%s", rel);
    if (h->jr_save && h->jr_save(h->user, &j, sizeof j)) return CF_FE_FLASH;   /* "not staged" is on record before the first erase */

    int fails = 0;
    while (off < size) {
        if (off % CF_FETCH_BLK == 0 && h->erase(h->user, off, CF_FETCH_BLK)) return CF_FE_FLASH;
        const uint32_t to_blk = CF_FETCH_BLK - off % CF_FETCH_BLK;          /* never write past the erased block */
        uint16_t want = (uint16_t)(to_blk < CF_FETCH_CHUNK ? to_blk : CF_FETCH_CHUNK);
        if (size - off < want) want = (uint16_t)(size - off);
        const uint8_t *d = NULL;
        uint16_t len = 0;
        uint32_t total = 0;
        const int r = h->range(h->user, rel, off, want, &d, &len, &total);
        if (r == 2) { logf_(h, "cf: fetch: the server refused %s", rel); return CF_FE_GONE; }
        if (r == 0 && total != size) { logf_(h, "cf: fetch: the server's size is %u, not %u", (unsigned)total, (unsigned)size); return CF_FE_SIZE; }
        if (r != 0 || len == 0 || len > want || !d) {
            if (++fails > max_retries) {
                logf_(h, "cf: fetch: stopped at %u of %u bytes after %d tries in a row (run it again to resume)", (unsigned)off, (unsigned)size, fails);
                return CF_FE_NET;
            }
            if (h->sleep_ms) h->sleep_ms(h->user, 500u << (fails > 5 ? 5 : fails));
            if (off % CF_FETCH_BLK == 0) continue;                           /* the block is erased again: harmless */
            /* mid-block: go on from here without erasing (the loop only erases at a block start) */
            continue;
        }
        fails = 0;
        if (h->write(h->user, off, d, len)) return CF_FE_FLASH;
        crc = cf_crc32(crc, d, len);
        off += len;
        if (off % CF_FETCH_JR_STEP == 0 && off < size) {
            j.done = off;
            if (h->jr_save && h->jr_save(h->user, &j, sizeof j)) return CF_FE_FLASH;
            logf_(h, "cf: fetch: %u of %u KiB", (unsigned)(off / 1024), (unsigned)(size / 1024));
        }
    }
    /* what counts is the flash: read it all back */
    uint32_t fc = 0;
    if (region_crc(h, size, &fc)) return CF_FE_FLASH;
    if (crc != crc32 || fc != crc32) {
        logf_(h, "cf: fetch: crc32 %08x (flash %08x), wanted %08x: dropped", (unsigned)crc, (unsigned)fc, (unsigned)crc32);
        j.done = 0;
        if (h->jr_save) (void)h->jr_save(h->user, &j, sizeof j);
        return CF_FE_CRC;
    }
                                       /* any ROM: the header is not judged (cf_image_compat warns later) */
    j.done = size; j.ok = 1;
    if (h->jr_save && h->jr_save(h->user, &j, sizeof j)) return CF_FE_FLASH;
    logf_(h, "cf: fetch: %u bytes, crc32 %08x, verified on the flash: staged", (unsigned)size, (unsigned)crc32);
    return CF_FE_OK;
}
