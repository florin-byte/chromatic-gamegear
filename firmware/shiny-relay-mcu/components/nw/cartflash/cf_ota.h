/* cf_ota.h: Shiny Relay's own update over Wi-Fi (1.1.0, owner 2026-10-20), the device's whole path.
 *
 * The flash has two app slots (partitions_ota.csv) and the boot loader's rollback. SYSTEM > UPDATE:
 *   A        CHECK FOR UPDATE: the link stops (its heap is for the TLS call, as receive mode), then
 *   check    POST /link_device/fw/check {"version","fpga","pt","bl"} (TLS, the unit's registration token)
 *            -> {"update": false} | {"update": false, "usb": true, "why": "fpga"|"layout", "version"}
 *             | {"update": true, "version", "size", "sha256", "url": "http://<blob host>/gbnet/v1/fw/<sha256>.bin"}
 *            pt / bl: the sha256 of this unit's partition table (0x8000, 3 KiB) and boot loader (0x1000-0x8000):
 *            an update that needs another table or boot loader is "UPDATE OVER USB", like a newer FPGA.
 *   download the image into the slot that is not running (plain http, Range: a drop goes on from where it
 *            was), hashed as it comes; the IDF checks the image too. Then UPDATE READY x.y.z (A: INSTALL,
 *            B: LATER). LATER keeps it there (NVS "ota"/"ready"); it is checked again before it is used.
 *   install  the slot is made the boot slot, the unit restarts. The new app confirms itself once it has run
 *            CF_OTA_CONFIRM_MS with its FPGA link up (cf_dev.c); a new app that never gets there is left
 *            for the old one at the next start (the boot loader's rollback), and the old one says so.
 * The image is the installer's own release (the image set's mcu_fw.bin): public, so no key; its sha256
 * comes over TLS. Nothing here polls: the unit asks only when A is pressed.
 * FPGA 18.31 on (cf_fota.h, layout rf): the check also says the FPGA boot flash's layout ("fl": "rf" | "rfrec" (the
 * recovery design runs: it takes any 18.x design) | "rffull" (no header place left) | "legacy"), and an answer may
 * carry the release's FPGA image too (fpga_version, fpga_size, fpga_sha256, fpga_url), alone or with the app. The
 * FPGA image is staged first in the app slot that is not running (sha256, read back, a GW5A-25 image check). Alone,
 * INSTALL writes it into the FPGA's boot flash (cf_fota_install: the recovery design boots while it is written; a cut
 * is finished by the next start); with an app, it is installed as soon as it is staged (the app then goes into the
 * same slot) and INSTALL switches the app slot. The FPGA boots at the next power-on. An update that needs a new FPGA
 * on a unit whose boot flash cannot take one ("rffull" / "legacy") is still UPDATE OVER USB.
 * 1.1.12, SYSTEM > CUSTOM (custom firmware, shiny_gen_3 notes/CUSTOM_FIRMWARE_PLAN_2026_11_09.md): the same machine
 * has a second channel. UPDATE is the official releases only: its check says "split":1 and an answer that names a
 * custom version ("-cfw.") is refused here. CUSTOM asks
 *   POST /link_device/fw/custom {the same body} (TLS, the unit's PAIRING token: the build is its account's choice)
 *            -> {"update": false} (none chosen) | {"update": false, "running": true} (this unit runs it)
 *             | {"update": false, "first": true} (the release must go on first: UPDATE)
 *             | {"update": false, "usb": true, "why": "layout", "version"}
 *             | {"update": true, "version": "<release>-cfw.<id>", "name", "size", "sha256", "url"}   (an app only)
 * and shows the build (BUILD WAITING, A: DOWNLOAD) before anything is fetched; an answer that is not a custom version,
 * or carries an FPGA part, is refused. Download, READY, INSTALL and the rollback are the update's own. One image waits
 * in the slot at a time: a check on one channel drops a waiting image of the other.
 * Portable C, driven by cf_ota_tick() from the nw task; nothing here blocks. */
#ifndef CF_OTA_H
#define CF_OTA_H
#include <stdint.h>

#define CF_OTA_URL_MAX   200
#define CF_OTA_BUF       512                 /* the check's answer */
#define CF_OTA_API_MS    (60u * 1000u)
#define CF_OTA_HOLD_MS   (6u * 1000u)
#define CF_OTA_WIFI_MS   (30u * 1000u)
#define CF_OTA_CONFIRM_MS (2u * 1000u)      /* a new app confirms itself after this long with its FPGA link up */
#define CF_OTA_OFFER_MS (120u * 1000u)     /* BUILD WAITING is shown this long (the link is held meanwhile) */
#define CF_OTA_MIN       0x20000u            /* an app image's size: 128 KiB .. one slot */
#define CF_OTA_MAX       0x1E0000u

enum { OT_IDLE, OT_HOLD, OT_REG, OT_CHECK, OT_DL, OT_READY, OT_SWITCH, OT_RESULT, OT_OFFER, OT_NSTATES };
enum { CF_OTA_OFFICIAL, CF_OTA_CUSTOM };   /* the channel */
enum { OV_MENU,      /* SHINY RELAY x.y.z, A: CHECK FOR UPDATE (or UPDATE READY when one waits) */
       OV_BUSY,      /* CHECKING / GETTING READY (B: CANCEL) */
       OV_DL,        /* DOWNLOADING x.y.z NN% (B: CANCEL) */
       OV_READY,     /* UPDATE READY x.y.z, A: INSTALL, B: LATER */
       OV_SWITCH,    /* RESTARTING */
       OV_RESULT,    /* title + two rows (A: OK) */
       OV_OFFER = 7 };  /* CUSTOM: BUILD WAITING name version, A: DOWNLOAD, B: BACK (6 is the page's own FPGA view) */
typedef struct {
    int      kind;
    char     title[24];
    char     note[40];       /* "ROW1|ROW2" */
    char     version[16];    /* the running one (menu) or the update's */
    int      pct;
    int      custom;         /* the channel of what is shown (CF_OTA_CUSTOM: a custom build) */
    char     name[24];       /* the custom build's project name ("" not known) */
} cf_ota_view_t;

#define CF_OTA_FPGA_MIN  0x40000u            /* an FPGA image: 256 KiB .. the biggest full design (cf_fota.h CF_FOTA_FULL_MAX) */
#define CF_OTA_FPGA_MAX  0x0D0000u   /* = CF_FOTA_FULL_MAX */
typedef struct {
    char     version[16];
    uint32_t size;                   /* 0: no app in this update */
    char     sha256[65];
    char     url[CF_OTA_URL_MAX + 1];
    char     fversion[8];            /* the FPGA image, when fsize != 0 */
    uint32_t fsize;
    char     fsha256[65];
    char     furl[CF_OTA_URL_MAX + 1];
    char     name[24];               /* CUSTOM: the project's name as the server wrote it for the screen */
} cf_ota_offer_t;

typedef struct {
    void *user;
    /* the device API, as cf_install's (registration token; "/link_device/register" without one) */
    int  (*api_start)(void *u, const char *path, const char *body, char *out, int cap);
    int  (*api_result)(void *u);
    int  (*registered)(void *u);
    void (*reg_forget)(void *u);
    int  (*wifi)(void *u);                         /* 0 no network saved, 1 connecting, 2 connected */
    void (*hold)(void *u, int on);
    int  (*held)(void *u);
    int  (*busy)(void *u);                         /* 1 = a cart job, receive mode or the staging area is in use: not now */
    int  (*slots)(void *u);                        /* 1 = two app slots (else: UPDATE OVER USB) */
    /* this unit: its version ("1.1.0"), FPGA ("18.28"), sha256 of its partition table and boot loader, the FPGA boot
       flash's layout ("ab" / "full" / "legacy") */
    void (*me)(void *u, char ver16[16], char fpga8[8], char pt65[65], char bl65[65], char fl8[8]);
    int  (*dl_start)(void *u, const cf_ota_offer_t *o);   /* 0 = started */
    int  (*dl_result)(void *u, int *pct);                 /* -1 running, 0 = in the slot and checked, else failed (CF_OTA_E_*) */
    void (*dl_cancel)(void *u);
    int  (*ready)(void *u, cf_ota_offer_t *o);            /* 1 = an update waits in the slot (and still hashes right) */
    void (*ready_forget)(void *u);
    /* the FPGA's next header, then the app's boot slot and a restart; with no app in it: 0 = done (the FPGA image
       takes over at the next power-on), CF_OTA_PENDING = still writing (asked again next turn), else failed */
    int  (*install)(void *u, const cf_ota_offer_t *o);
    void (*log)(void *u, const char *m);
} cf_ota_host_t;
enum { CF_OTA_E_NET = 1, CF_OTA_E_HASH, CF_OTA_E_IMAGE, CF_OTA_E_FLASH, CF_OTA_E_GONE };
#define CF_OTA_PENDING 2                     /* install(): the FPGA header is being written on another task */

typedef struct {
    cf_ota_host_t h;
    int      st;
    uint32_t now, t0;
    char    *buf;
    volatile int btn;
    int      rx, reg_tries;
    int      chan;                       /* CF_OTA_OFFICIAL / CF_OTA_CUSTOM: what this run is for */
    cf_ota_offer_t offer;
    int      pct;
    const char *res_title, *res_rows;    /* literals */
    char     res_ver[16];
    unsigned n_checks, n_dl, n_ok, n_fail;
} cf_ota_t;

void cf_ota_init(cf_ota_t *c, const cf_ota_host_t *h, uint32_t now);
void cf_ota_tick(cf_ota_t *c, uint32_t now);
void cf_ota_button(cf_ota_t *c, int a);
void cf_ota_custom(cf_ota_t *c);                 /* SYSTEM > CUSTOM's A on its menu: check for the custom build */
int  cf_ota_is_custom(const char *version);      /* 1 = a custom build's version ("-cfw.") */
void cf_ota_view(const cf_ota_t *c, cf_ota_view_t *v);
int  cf_ota_busy(const cf_ota_t *c);
/* what the boot found (cf_dev.c): 1 = this is a new version, confirmed; 2 = the last update did not start
   and the old version is back; 3 = confirmed, and its FPGA image runs from the next power-on. Shown until A. */
void cf_ota_notice(cf_ota_t *c, int what, const char *version);
/* parse the check's answer (exposed for the tests): 1 update (in *o), 0 none, 2 = over USB only, -1 refused */
int  cf_ota_parse(const char *json, cf_ota_offer_t *o, const char **why);
#endif
