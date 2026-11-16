/* cf_fetch.h: download a cart image into the ESP32's image region (lane cartflash1, DRAFT).
 *
 * "Send to my Chromatic", the Wi-Fi half: the image comes over the gateway's HTTPS in ranged
 * GETs, goes straight into the "cart" partition's image region, and is only staged for
 * `cart install` after its size, crc32 and cart header check out ON THE FLASH (read back).
 * Portable C: the device (cf_dev.c) and the host test (host/cf_fetch_test.c) give the I/O.
 *
 * Resume: progress is journalled every CF_FETCH_JR_STEP bytes (block aligned). A fetch of the
 * same image after a reboot or a dropped link re-checks the stored prefix by crc and goes on
 * from there; anything else starts again from 0.
 */
#ifndef CF_FETCH_H
#define CF_FETCH_H
#include <stdint.h>

#define CF_FETCH_BLK      4096u            /* the region is erased one block ahead of the writes */
#define CF_FETCH_CHUNK    2048u            /* one ranged GET (the gateway budgets 30/s; 512 GETs per MiB) */
#define CF_FETCH_JR_STEP  65536u
#define CF_FETCH_MAGIC    0x31464643u      /* "CFF1" */

enum {
    CF_FE_OK = 0,
    CF_FE_ARG,          /* size below 32 KiB or larger than the region, or no path */
    CF_FE_NET,          /* gave up after max_retries in a row: run it again to resume */
    CF_FE_GONE,         /* the server says no (not found / denied): not retried */
    CF_FE_SIZE,         /* the server's total is not the size asked for */
    CF_FE_FLASH,        /* erase / write / read of the region failed */
    CF_FE_CRC,          /* the whole image does not have the crc32 asked for: progress dropped */
    CF_FE_HEADER,       /* unused since 2026-10-11 (any ROM is taken); kept so the codes do not move */
};

/* range(): 0 = *data / *len valid (len 1..max), *total = the whole file's size;
 *          1 = try again (link down, timeout, 5xx, low heap); 2 = no (404, 401/403) */
typedef struct {
    void *user;
    uint32_t cap;                                                   /* image region size */
    int  (*range)(void *u, const char *rel, uint32_t off, uint16_t max, const uint8_t **data, uint16_t *len, uint32_t *total);
    int  (*erase)(void *u, uint32_t off, uint32_t n);               /* 0 ok */
    int  (*write)(void *u, uint32_t off, const uint8_t *b, uint32_t n);
    int  (*read)(void *u, uint32_t off, uint8_t *b, uint32_t n);
    int  (*jr_load)(void *u, void *j, uint32_t n);                  /* 0 ok */
    int  (*jr_save)(void *u, const void *j, uint32_t n);
    void (*sleep_ms)(void *u, uint32_t ms);
    void (*log)(void *u, const char *m);
} cf_fetch_host_t;

typedef struct {
    uint32_t magic;
    uint32_t size, crc32;
    uint32_t done;              /* bytes known written (block aligned while in flight) */
    uint8_t  ok;                /* 1: the whole image is in the region and verified */
    char     rel[95];
} cf_fetch_jr_t;

/* Fetch rel (a path below the gateway base). Returns CF_FE_*. */
int cf_fetch(const cf_fetch_host_t *h, const char *rel, uint32_t size, uint32_t crc32, int max_retries);
/* Is a verified image staged? Re-reads the region and checks size / crc32 / header.
 * Returns the size, or 0. */
uint32_t cf_fetch_staged(const cf_fetch_host_t *h, uint32_t *crc32);
#endif
