/* sd_lib.c: lane sdcard1: the game library on the card.
 *
 * The games are .gb / .gbc files anywhere on the card (SDC_DEPTH_MAX folders deep; hidden folders,
 * the system's and ours are skipped). The library is an index on the card, /SHINY/LIBRARY.IDX: a
 * 256-byte header (count, the walk's fingerprint) then one 256-byte record per game sorted by the
 * name shown (the header title, else the file name). Opening the library never rescans: the OSD
 * reads records by number. A refresh (when a card is mounted, and after the unit itself adds or
 * deletes a game) walks the folders only (no file is opened) and compares the fingerprint of every
 * game's path, size and date; only a change rebuilds the index: every game's header read into
 * LIBRARY.NEW, sorted in passes of a few hundred (sdc_sort, bounded memory) into LIBRARY.TMP, which
 * then replaces LIBRARY.IDX (a cut in between: the TMP is taken at the next mount).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "ff.h"
#include "sd.h"
#include "sd_core.h"

/* Three bounded-memory views: the original files contain all games; GB/GG files use the
 * same format and folder ids, with their own counts and runs. No ROM is read by a page. */
static int (*_Core)(void);
static int _BuildCore = -2, _OpenCore = -3;
void SdLib_SetCore(int (*core)(void)) { _Core = core; }
static int view_core(void) { return _BuildCore != -2 ? _BuildCore : _Core ? _Core() : -1; }
int SdLib_CorePending(void) { return view_core() == -2; }
static const char *view_path(const char *path)
{
    static const char *const paths[2][8] = {
        {"/SHINY/GBLIB.IDX", "/SHINY/GBLIB.TMP", "/SHINY/GBDIR.IDX", "/SHINY/GBDIR.TMP",
         "/SHINY/GBFOLD.IDX", "/SHINY/GBFOLD.TMP", "/SHINY/GBORDER.IDX", "/SHINY/GBORDER.TMP"},
        {"/SHINY/GGLIB.IDX", "/SHINY/GGLIB.TMP", "/SHINY/GGDIR.IDX", "/SHINY/GGDIR.TMP",
         "/SHINY/GGFOLD.IDX", "/SHINY/GGFOLD.TMP", "/SHINY/GGORDER.IDX", "/SHINY/GGORDER.TMP"}
    };
    static const char *const all[] = { SDC_INDEX, SDC_INDEX_TMP, SDC_DIRIDX, SDC_DIRIDX_TMP,
        SDC_FOLDERS, SDC_FOLDERS_TMP, SDC_FORDER, SDC_FORDER_TMP };
    const int core = view_core();
    if (core < 0 || core > 1) return path;
    for (int i = 0; i < 8; i++) if (!strcmp(path, all[i])) return paths[core][i];
    return path;
}
static FIL *_Ix;                          /* the index, open while the card is (read by the OSD) */
static uint32_t _IxGen = ~0u;             /* the card generation it was opened on */
static uint32_t _Count;
static int _HaveIx;
static volatile int _BuildPct = -1;       /* a rebuild: 0..100, else -1 */
static uint32_t _Fp;                      /* the index's walk fingerprint */
static uint32_t _Folders;                 /* fr1: records in FOLDERS.IDX */
static void aux_close(void);

static void ix_close(void)
{
    if (_Ix) { (void)f_close(_Ix); free(_Ix); _Ix = NULL; }
    _HaveIx = 0; _Count = 0;
}
/* the index open (and its header read) for this card; 0 = there */
static int ix_open(void)
{
    if (SdLib_CorePending()) { ix_close(); aux_close(); return -1; }
    if (!Sd_Mounted()) { ix_close(); _IxGen = Sd_Generation(); return -1; }
    if (_OpenCore != view_core()) { ix_close(); aux_close(); _OpenCore = view_core(); }
    if (_Ix && _IxGen == Sd_Generation()) return _HaveIx ? 0 : -1;
    ix_close();
    _IxGen = Sd_Generation();
    _Ix = malloc(sizeof *_Ix);
    if (!_Ix) return -1;
    FRESULT r = f_open(_Ix, view_path(SDC_INDEX), FA_READ);
    if (r == FR_NO_FILE && Sd_Writable() && f_rename(view_path(SDC_INDEX_TMP), view_path(SDC_INDEX)) == FR_OK) {   /* a cut between unlink and rename */
        printf("sd: library: the finished index taken from LIBRARY.TMP\n");
        r = f_open(_Ix, view_path(SDC_INDEX), FA_READ);
    }
    UINT got = 0;
    uint32_t h[8];                              /* magic, version, count, fingerprint, .., folders (sdc_idx_hdr_t) */
    if (r != FR_OK || f_read(_Ix, h, sizeof h, &got) != FR_OK || got != sizeof h || h[0] != SDC_IDX_MAGIC ||
        h[1] != SDC_IDX_VERSION || f_size(_Ix) < (FSIZE_t)SDC_REC_SIZE * (1u + h[2])) {
        if (r == FR_OK) (void)f_close(_Ix);
        free(_Ix); _Ix = NULL;
        return -1;
    }
    _Count = h[2]; _Fp = h[3]; _Folders = h[7];
    _HaveIx = 1;
    return 0;
}

/* ---- fr1: the folder view's files (LIBDIR.IDX, FOLDERS.IDX, FORDER.IDX): one more file open at a time (its ~0.6 KB of
 * heap), opened again when the page reads another one; each checked once against the index (the same walk) */
enum { kAux_None, kAux_Dir, kAux_Folders, kAux_Order, kAux_N };
static FIL *_Ax;
static int _AxWhich;
static uint32_t _AxGen = ~0u;
static int8_t _AxOk[kAux_N];                 /* per file, this card generation: 0 not looked at, 1 good, -1 not usable */
static void aux_close(void)
{
    if (_Ax) { if (_AxWhich != kAux_None) (void)f_close(_Ax); free(_Ax); _Ax = NULL; }
    _AxWhich = kAux_None;
    memset(_AxOk, 0, sizeof _AxOk);
    _AxGen = ~0u;
}
/* file `which` open and checked; 0 = there (call with the card locked and the index open) */
static int aux_open(int which)
{
    if (_AxGen != Sd_Generation() || !_HaveIx) { aux_close(); _AxGen = Sd_Generation(); }
    if (_AxOk[which] < 0) return -1;
    if (_Ax && _AxWhich == which) return 0;
    if (!_Ax && !(_Ax = malloc(sizeof *_Ax))) return -1;
    if (_AxWhich != kAux_None) { (void)f_close(_Ax); _AxWhich = kAux_None; }
    static const char *const kPath[kAux_N] = { "", SDC_DIRIDX, SDC_FOLDERS, SDC_FORDER };
    if (f_open(_Ax, view_path(kPath[which]), FA_READ) != FR_OK) { _AxOk[which] = -1; return -1; }
    _AxWhich = which;
    if (!_AxOk[which]) {
        uint32_t h[8];
        UINT got = 0;
        const int ok = f_read(_Ax, h, sizeof h, &got) == FR_OK && got == sizeof h && h[0] == SDC_IDX_MAGIC && h[1] == SDC_IDX_VERSION &&
                       h[3] == _Fp && h[2] == _Count && h[7] == _Folders;
        _AxOk[which] = ok ? 1 : -1;
        if (!ok) { (void)f_close(_Ax); _AxWhich = kAux_None; return -1; }
    }
    return 0;
}
static int aux_read(int which, FSIZE_t off, void *b, UINT n)
{
    UINT got = 0;
    if (ix_open() || aux_open(which)) return -1;
    return f_lseek(_Ax, off) == FR_OK && f_read(_Ax, b, n, &got) == FR_OK && got == n ? 0 : -1;
}
/* the folder files all there and made with this index (1), else 0 (no folder view: the page shows A-Z only) */
static int folders_ok(void)
{
    if (ix_open() || !_Folders) return 0;
    for (int k = kAux_Dir; k < kAux_N; k++) if (aux_open(k)) return 0;
    return 1;
}
int SdLib_Folders(void)
{
    if (!Sd_TryLock(30)) return -2;
    const int r = folders_ok() ? (int)_Folders : 0;
    Sd_Unlock();
    return r;
}
int SdLib_Folder(uint32_t id, sdc_folder_t *f)
{
    if (!Sd_TryLock(30)) return -2;
    const int r = ix_open() == 0 && id < _Folders &&
                  aux_read(kAux_Folders, SDC_FOLDER_OFF(id), f, sizeof *f) == 0 ? 0 : -1;
    if (!r) f->name[SDC_FNAME_LEN - 1] = 0;
    Sd_Unlock();
    return r;
}
int SdLib_FolderChild(uint32_t pos, uint32_t *id)
{
    if (!Sd_TryLock(30)) return -2;
    const int r = aux_read(kAux_Order, (FSIZE_t)SDC_REC_SIZE + (FSIZE_t)pos * sizeof *id, id, sizeof *id) == 0 ? 0 : -1;
    Sd_Unlock();
    return r;
}
int SdLib_DirGet(uint32_t pos, sdc_rec_t *rec)
{
    if (!Sd_TryLock(30)) return -2;
    int r = -1;
    if (ix_open() == 0 && pos < _Count && aux_read(kAux_Dir, (FSIZE_t)SDC_REC_SIZE * (1u + pos), rec, sizeof *rec) == 0) {
        rec->title[SDC_NAME_LEN - 1] = 0; rec->path[SDC_PATH_MAX - 1] = 0;
        r = 0;
    }
    Sd_Unlock();
    return r;
}

static const char *recent_path(int core)
{
    return core == 0 ? "/SHINY/GBRECENT.TXT" : core == 1 ? "/SHINY/GGRECENT.TXT" : SDC_RECENT;
}
/* Rebuild the two short recent views only when the history or index changes. */
static int recent_views(void)
{
    const size_t cap = SDC_RECENT_MAX * (SDC_PATH_MAX + SDC_NAME_LEN + 2) + 1;
    char *b = malloc(cap), *out = malloc(cap);
    FIL *f = malloc(sizeof *f);
    int rc = -1, saved = _BuildCore;
    if (!b || !out || !f) goto done;
    UINT got = 0;
    if (f_open(f, SDC_RECENT, FA_READ) == FR_OK) {
        if (f_read(f, b, (UINT)(cap - 1), &got) != FR_OK) { (void)f_close(f); goto done; }
        (void)f_close(f);
    }
    b[got] = 0;
    _BuildCore = -1;
    for (int core = 0; core < 2; core++) {
        size_t n = 0;
        for (char *l = b; *l;) {
            char *e = strchr(l, '\n'), *t = strchr(l, '\t');
            size_t len = e ? (size_t)(e - l) : strlen(l);
            if (t && (size_t)(t - l) < len) {
                *t = 0;
                int i = SdLib_Find(l);
                *t = '\t';
                sdc_rec_t r;
                if (i >= 0 && !SdLib_Get((uint32_t)i, &r) && !!(r.flags & SDC_F_GG) == core && n + len + 1 < cap) {
                    memcpy(out + n, l, len); n += len; out[n++] = '\n';
                }
            }
            if (!e) break;
            l = e + 1;
        }
        if (f_open(f, recent_path(core), FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) goto done;
        UINT w = 0;
        int bad = f_write(f, out, (UINT)n, &w) != FR_OK || w != n;
        if (f_close(f) != FR_OK || bad) goto done;
    }
    rc = 0;
done:
    _BuildCore = saved;
    free(b); free(out); free(f);
    return rc;
}

/* ---- fr1: the games played last (RECENT.TXT: "path<TAB>name" lines, the newest first) */
int SdLib_RecentAdd(const char *path, const char *title)
{
    if (!path || !path[0] || !Sd_Writable()) return -1;
    const size_t cap = SDC_RECENT_MAX * (SDC_PATH_MAX + SDC_NAME_LEN + 2) + 1;
    char *old = malloc(cap), *nw = malloc(cap);
    FIL *fp = malloc(sizeof *fp);
    int r = -1;
    if (!old || !nw || !fp) goto out;
    size_t on = 0, nn = 0;
    UINT got = 0;
    /* (called from the menu: never waits behind a long job of the card; a play it misses is not remembered) */
    if (!Sd_TryLock(50)) goto out;
    if (f_open(fp, SDC_RECENT, FA_READ) == FR_OK) { if (f_read(fp, old, (UINT)(cap - 1), &got) == FR_OK) on = got; (void)f_close(fp); }
    old[on] = 0;
    nn = (size_t)snprintf(nw, cap, "%s\t%s\n", path, title ? title : "");
    uint32_t lines = 1;
    for (char *l = old; *l && lines < SDC_RECENT_MAX;) {
        char *e = strchr(l, '\n');
        const size_t len = e ? (size_t)(e - l) : strlen(l);
        const size_t pl = strlen(path);
        const int same = len > pl && !strncmp(l, path, pl) && l[pl] == '\t';
        if (len && !same && nn + len + 1 < cap) { memcpy(nw + nn, l, len); nn += len; nw[nn++] = '\n'; lines++; }
        if (!e) break;
        l = e + 1;
    }
    UINT w = 0;
    if (f_open(fp, SDC_RECENT, FA_CREATE_ALWAYS | FA_WRITE) == FR_OK) {
        r = f_write(fp, nw, (UINT)nn, &w) == FR_OK && w == nn ? 0 : -1;
        if (f_close(fp) != FR_OK) r = -1;
    }
    Sd_Unlock();
out:
    free(old); free(nw); free(fp);
    if (!r && Sd_TryLock(50)) { r = recent_views(); Sd_Unlock(); }
    return r;
}
/* the k-th game played last (0 the newest): its path and name; 0 = there, -1 none, -2 busy */
int SdLib_Recent(uint32_t k, char *path, size_t pn, char *title, size_t tn)
{
    if (SdLib_CorePending()) return -1;
    if (!Sd_TryLock(30)) return -2;
    int r = -1;
    const size_t cap = SDC_RECENT_MAX * (SDC_PATH_MAX + SDC_NAME_LEN + 2) + 1;
    FIL *f = Sd_Mounted() ? malloc(sizeof *f) : NULL;
    char *b = f ? malloc(cap) : NULL;
    UINT got = 0;
    if (b && f_open(f, recent_path(view_core()), FA_READ) == FR_OK) {
        if (f_read(f, b, (UINT)(cap - 1), &got) != FR_OK) got = 0;
        (void)f_close(f);
        b[got] = 0;
        char *l = b;
        for (uint32_t i = 0; l && *l; i++) {
            char *e = strchr(l, '\n');
            if (e) *e = 0;
            char *t = strchr(l, '\t');
            if (t) *t++ = 0;
            if (i == k) {
                if (l[0]) { snprintf(path, pn, "%s", l); snprintf(title, tn, "%s", t ? t : ""); r = 0; }
                break;
            }
            l = e ? e + 1 : NULL;
        }
    }
    free(b); free(f);
    Sd_Unlock();
    return r;
}

/* (the OSD's calls: never blocked behind a rebuild; -2 = busy, try again) */
int SdLib_Count(void)
{
    if (!Sd_TryLock(30)) return -2;
    const int r = ix_open() == 0 ? (int)_Count : -1;
    Sd_Unlock();
    return r;
}
int SdLib_Total(void)
{
    if (!Sd_TryLock(30)) return -2;
    _BuildCore = -1;
    const int n = SdLib_Count();
    _BuildCore = -2;
    Sd_Unlock();
    return n;
}
int SdLib_GetAll(uint32_t i, sdc_rec_t *rec)
{
    if (!Sd_TryLock(30)) return -2;
    _BuildCore = -1;
    const int r = SdLib_Get(i, rec);
    _BuildCore = -2;
    Sd_Unlock();
    return r;
}
int SdLib_Building(void) { return _BuildPct; }
int SdLib_Get(uint32_t i, sdc_rec_t *rec)
{
    if (!Sd_TryLock(30)) return -2;
    int r = -1;
    UINT got = 0;
    if (ix_open() == 0 && i < _Count && f_lseek(_Ix, (FSIZE_t)SDC_REC_SIZE * (1u + i)) == FR_OK &&
        f_read(_Ix, rec, sizeof *rec, &got) == FR_OK && got == sizeof *rec) {
        rec->title[SDC_NAME_LEN - 1] = 0; rec->path[SDC_PATH_MAX - 1] = 0;
        r = 0;
    }
    Sd_Unlock();
    return r;
}
/* the record of a path (-1 none) */
int SdLib_Find(const char *path)
{
    Sd_Lock();
    const int n = ix_open() == 0 ? (int)_Count : 0;
    sdc_rec_t r;
    int at = -1;
    for (int i = 0; i < n && at < 0; i++) if (SdLib_Get((uint32_t)i, &r) == 0 && !strcmp(r.path, path)) at = i;
    Sd_Unlock();
    return at;
}

/* ---- the walk: every game file, depth first. fr1: each folder is read twice, its games first, then its folders,
 * so a folder's games come together; every folder entered gets an id (the order it was entered: a parent's id is
 * always smaller than its children's). Past SDC_MAX_FOLDERS a folder's games count as its parent's. */
typedef struct {
    char path[260];
    FF_DIR dir[SDC_DEPTH_MAX + 1];
    uint16_t plen[SDC_DEPTH_MAX + 1];
    uint8_t ph[SDC_DEPTH_MAX + 1];                           /* 0 the folder's games, 1 its folders */
    uint32_t id[SDC_DEPTH_MAX + 1];
    uint32_t next_id;
    FILINFO fi;
} walk_t;
typedef int (*walk_fn)(void *u, const char *path, const FILINFO *fi, uint32_t folder);
typedef int (*folder_fn)(void *u, uint32_t id, uint32_t parent, uint32_t depth, const char *name);
static int walk(walk_t *w, walk_fn fn, folder_fn dfn, void *u, uint32_t *too_long)
{
    int d = 0;
    snprintf(w->path, sizeof w->path, "/");
    w->plen[0] = 0; w->ph[0] = 0; w->id[0] = 0; w->next_id = 1;
    if (f_opendir(&w->dir[0], "/") != FR_OK) return -1;
    int r = dfn ? dfn(u, 0, 0, 0, "") : 0;
    while (d >= 0 && !r) {
        const FRESULT fr = f_readdir(&w->dir[d], &w->fi);
        if (fr != FR_OK) { r = -1; break; }
        if (!w->fi.fname[0]) {                               /* this folder is done (its games, then its folders) */
            if (w->ph[d] == 0) { w->ph[d] = 1; if (f_readdir(&w->dir[d], NULL) != FR_OK) r = -1; continue; }
            (void)f_closedir(&w->dir[d]);
            d--;
            if (d >= 0) w->path[w->plen[d]] = 0;
            continue;
        }
        if (w->fi.fattrib & (AM_HID | AM_SYS)) continue;
        const size_t base = w->plen[d];
        const size_t nl = strlen(w->fi.fname);
        if (w->fi.fattrib & AM_DIR) {
            if (w->ph[d] == 0) continue;
            if (d >= SDC_DEPTH_MAX || sdc_skip_dir(w->fi.fname) || base + 1 + nl + 1 >= sizeof w->path) continue;
            w->path[base] = '/';
            memcpy(w->path + base + 1, w->fi.fname, nl + 1);
            if (f_opendir(&w->dir[d + 1], w->path) != FR_OK) { w->path[base] = 0; continue; }
            uint32_t id = w->id[d];
            if (w->next_id < SDC_MAX_FOLDERS) {
                id = w->next_id++;
                if (dfn && (r = dfn(u, id, w->id[d], (uint32_t)d + 1, w->fi.fname)) != 0) { (void)f_closedir(&w->dir[d + 1]); break; }
            }
            d++;
            w->plen[d] = (uint16_t)(base + 1 + nl);
            w->ph[d] = 0; w->id[d] = id;
            continue;
        }
        if (w->ph[d] != 0 || !sdc_is_game_name(w->fi.fname)) continue;
        if (base + 1 + nl + 1 > SDC_PATH_MAX) { if (too_long) (*too_long)++; continue; }
        w->path[base] = '/';
        memcpy(w->path + base + 1, w->fi.fname, nl + 1);
        r = fn(u, w->path, &w->fi, w->id[d]);
        w->path[base] = 0;
    }
    while (d >= 0) (void)f_closedir(&w->dir[d--]);
    return r;
}

typedef struct { uint32_t fp, n; FIL *out, *fo; uint8_t *hb; uint32_t skipped, done, total, nf; } build_t;
static int fp_fn(void *u, const char *path, const FILINFO *fi, uint32_t folder)
{
    (void)folder;
    build_t *b = u;
    b->fp = sdc_fp_add(b->fp, path, (uint32_t)fi->fsize, (uint32_t)fi->fdate << 16 | fi->ftime);
    b->n++;
    return b->n > SDC_MAX_GAMES ? 1 : 0;
}
/* fr1: a folder entered: its record (the counts are made after the walk) */
static int dir_fn(void *u, uint32_t id, uint32_t parent, uint32_t depth, const char *name)
{
    build_t *b = u;
    sdc_folder_t f;
    memset(&f, 0, sizeof f);
    snprintf(f.name, sizeof f.name, "%s", name);
    f.parent = parent; f.depth = depth;
    UINT w = 0;
    if (f_lseek(b->fo, SDC_FOLDER_OFF(id)) != FR_OK || f_write(b->fo, &f, sizeof f, &w) != FR_OK || w != sizeof f) return -1;
    b->nf = id + 1;
    return 0;
}
static int rec_fn(void *u, const char *path, const FILINFO *fi, uint32_t folder)
{
    build_t *b = u;
    FIL f;
    UINT got = 0;
    sdc_hdr_t h;
    memset(&h, 0, sizeof h);
    int ok = -1;
    if (f_open(&f, path, FA_READ) == FR_OK) {
        if (f_read(&f, b->hb, 0x158, &got) == FR_OK && got >= 0x150) ok = sdc_hdr_parse(b->hb, (uint32_t)fi->fsize, &h) == 0 ? 0 : 1;
        (void)f_close(&f);
    }
    if (ok < 0) { b->skipped++; return 0; }               /* (not readable, or shorter than a header) */
    sdc_rec_t r;
    sdc_rec_make(&r, path, (uint32_t)fi->fsize, (uint32_t)fi->fdate << 16 | fi->ftime, &h, ok);
    if (got >= 0x158 && !memcmp(b->hb + 0x150, "GGCHROM1", 8)) r.flags |= SDC_F_GG;
    if (view_core() >= 0 && !!(r.flags & SDC_F_GG) != view_core()) return 0;
    r.folder = folder;
    UINT w = 0;
    if (f_write(b->out, &r, sizeof r, &w) != FR_OK || w != sizeof r) return -1;
    b->n++;
    b->done++;
    if (b->total) _BuildPct = (int)(10u + (uint64_t)b->done * 70u / b->total);
    return b->n >= SDC_MAX_GAMES ? 1 : 0;
}

/* the sorts' reads: a file of fixed-size records in order, a few KiB at a time; key() makes a record's 16-byte key */
typedef struct {
    FIL *in, *out, *fo;
    uint8_t *buf;
    uint32_t at, have, n, rsz, base;    /* base: the first record's offset in `in` */
    void (*key)(const uint8_t *rec, char t[17]);
    /* fr1: the runs (LIBDIR: a folder's games; FORDER: a parent's folders) */
    uint32_t pos, cur, first, cnt;
    int run;
} sort_t;
#define SORT_BUF 2048u
static void key_title(const uint8_t *rec, char t[17]) { memcpy(t, rec, 16); t[16] = 0; }
static void key_dir(const uint8_t *rec, char t[17])
{
    const sdc_rec_t *r = (const sdc_rec_t *)rec;
    sdc_key_folder(t, r->folder, r->title);
}
static void key_folder(const uint8_t *rec, char t[17])
{
    const sdc_folder_t *f = (const sdc_folder_t *)rec;
    if (!f->total) { memset(t, '~', 16); t[16] = 0; return; }   /* (no games in it or below: last, then left out) */
    sdc_key_folder(t, f->parent, f->name);
}
static int sort_get(void *u, uint32_t i, char t[17])
{
    sort_t *s = u;
    if (i < s->at || i >= s->at + s->have) {
        UINT got = 0;
        s->at = i;
        const uint32_t want = (s->n - i) * s->rsz < SORT_BUF ? (s->n - i) * s->rsz : SORT_BUF;
        if (f_lseek(s->in, s->base + (FSIZE_t)i * s->rsz) != FR_OK || f_read(s->in, s->buf, want, &got) != FR_OK || got < s->rsz) return -1;
        s->have = got / s->rsz;
    }
    s->key(s->buf + (i - s->at) * s->rsz, t);
    return 0;
}
static int read_at(FIL *f, FSIZE_t off, void *b, UINT n)
{
    UINT got = 0;
    return f_lseek(f, off) == FR_OK && f_read(f, b, n, &got) == FR_OK && got == n ? 0 : -1;
}
static int write_at(FIL *f, FSIZE_t off, const void *b, UINT n)
{
    UINT w = 0;
    return f_lseek(f, off) == FR_OK && f_write(f, b, n, &w) == FR_OK && w == n ? 0 : -1;
}
static int folder_rd(FIL *fo, uint32_t id, sdc_folder_t *f) { return read_at(fo, SDC_FOLDER_OFF(id), f, sizeof *f); }
static int folder_wr(FIL *fo, uint32_t id, const sdc_folder_t *f) { return write_at(fo, SDC_FOLDER_OFF(id), f, sizeof *f); }
/* a run ended: the folder's games (LIBDIR) or a parent's folders (FORDER) written into its record */
static int run_end(sort_t *s, int games)
{
    if (!s->run) return 0;
    sdc_folder_t f;
    if (folder_rd(s->fo, s->cur, &f)) return -1;
    if (games) { f.game_first = s->first; f.game_count = s->cnt; }
    else { f.child_first = s->first; f.child_count = s->cnt; }
    s->run = 0;
    return folder_wr(s->fo, s->cur, &f);
}
static int run_add(sort_t *s, uint32_t id, int games)
{
    if (s->run && s->cur != id && run_end(s, games)) return -1;
    if (!s->run) { s->run = 1; s->cur = id; s->first = s->pos; s->cnt = 0; }
    s->cnt++;
    s->pos++;
    return 0;
}
/* LIBRARY.TMP / LIBDIR.TMP: the record copied (LIBDIR: its folder's run kept) */
static int emit_rec(sort_t *s, uint32_t i, int dir)
{
    sdc_rec_t r;
    if (read_at(s->in, s->base + (FSIZE_t)i * SDC_REC_SIZE, &r, sizeof r)) return -1;
    s->have = 0;                                          /* (the read moved the window; the next get reads again) */
    UINT w = 0;
    if (f_write(s->out, &r, sizeof r, &w) != FR_OK || w != sizeof r) return -1;
    return dir ? run_add(s, r.folder, 1) : 0;
}
static int emit_title(void *u, uint32_t i) { return emit_rec(u, i, 0); }
static int emit_dir(void *u, uint32_t i) { return emit_rec(u, i, 1); }
/* FORDER.TMP: folder i + 1 (the top is not sorted), its parent's run kept; a folder with no games is left out */
static int emit_folder(void *u, uint32_t i)
{
    sort_t *s = u;
    sdc_folder_t f;
    const uint32_t id = i + 1;
    if (folder_rd(s->in, id, &f)) return -1;
    s->have = 0;
    if (!f.total) return 0;
    UINT w = 0;
    if (f_write(s->out, &id, sizeof id, &w) != FR_OK || w != sizeof id) return -1;
    return run_add(s, f.parent, 0);
}
static int run_sort(sort_t *s, int (*emit)(void *u, uint32_t i), uint32_t n, sdc_key_t *work, uint32_t cap, uint32_t *passes)
{
    s->n = n; s->at = 0; s->have = 0; s->pos = 0; s->run = 0;
    uint32_t p = 0;
    const int r = n ? sdc_sort(n, sort_get, emit, s, work, cap, &p) : 0;
    if (passes) *passes += p;
    return r;
}
/* the counts of every folder: its games and those below (children have larger ids: from the last id up) */
static int folder_totals(FIL *fo, uint32_t nf)
{
    for (uint32_t id = nf; id-- > 0;) {
        sdc_folder_t f, p;
        if (folder_rd(fo, id, &f)) return -1;
        f.total += f.game_count;
        if (folder_wr(fo, id, &f)) return -1;
        if (!id) break;
        if (folder_rd(fo, f.parent, &p)) return -1;
        p.total += f.total;
        if (folder_wr(fo, f.parent, &p)) return -1;
    }
    return 0;
}
static int put_hdr(FIL *f, const sdc_idx_hdr_t *h) { return write_at(f, 0, h, sizeof *h); }
static int hdr_again(FIL *f, const char *path, const sdc_idx_hdr_t *h)
{
    if (f_open(f, view_path(path), FA_WRITE) != FR_OK) return -1;
    const int r = put_hdr(f, h);
    return f_close(f) == FR_OK ? r : -1;
}

static int rebuild(uint32_t fp, uint32_t total)
{
    const int64_t t0 = esp_timer_get_time();
    int rc = -1;
    build_t b = { .fp = fp, .total = total };
    sort_t s;
    memset(&s, 0, sizeof s);
    walk_t *w = malloc(sizeof *w);
    FIL *in = malloc(sizeof *in), *out = malloc(sizeof *out), *fo = malloc(sizeof *fo);
    b.hb = heap_caps_malloc(SORT_BUF, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    uint32_t too_long = 0, passes = 0, cap = 0;
    sdc_key_t *work = NULL;
    int in_open = 0, out_open = 0, fo_open = 0;
    sdc_idx_hdr_t h;
    memset(&h, 0, sizeof h);
    if (!w || !in || !out || !fo || !b.hb) goto done;
    _BuildPct = 10;
    if (f_open(out, SDC_UNSORTED, FA_CREATE_ALWAYS | FA_WRITE | FA_READ) != FR_OK) goto done;
    out_open = 1;
    if (f_open(fo, view_path(SDC_FOLDERS_TMP), FA_CREATE_ALWAYS | FA_WRITE | FA_READ) != FR_OK) goto done;
    fo_open = 1;
    b.out = out; b.fo = fo;
    const int wr = walk(w, rec_fn, dir_fn, &b, &too_long);
    free(w); w = NULL;
    out_open = 0;
    if (f_close(out) != FR_OK || wr < 0) goto done;
    /* the sorts' work: as many keys as the heap gives (at least 32, at most 512) */
    cap = (esp_get_free_heap_size() > 30000u ? (esp_get_free_heap_size() - 24000u) : 640u) / sizeof(sdc_key_t);
    if (cap > 512) cap = 512;
    if (cap < 32) cap = 32;
    while (!work && cap >= 32) { work = malloc(cap * sizeof *work); if (!work) cap /= 2; }
    if (!work) goto done;
    h.version = SDC_IDX_VERSION; h.count = b.n; h.fingerprint = fp; h.too_long = too_long; h.skipped = b.skipped;
    h.folders = b.nf;
    s.buf = b.hb; s.rsz = SDC_REC_SIZE; s.base = 0; s.fo = fo;
    /* 1. LIBRARY.NEW -> LIBRARY.TMP by name (the header first: its magic written last) */
    if (f_open(in, SDC_UNSORTED, FA_READ) != FR_OK) goto done;
    in_open = 1;
    if (f_open(out, view_path(SDC_INDEX_TMP), FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) goto done;
    out_open = 1;
    _BuildPct = 82;
    s.in = in; s.out = out; s.key = key_title;
    if (put_hdr(out, &h) || run_sort(&s, emit_title, b.n, work, cap, &passes)) goto done;
    out_open = 0;
    if (f_close(out) != FR_OK) goto done;
    /* 2. LIBRARY.NEW -> LIBDIR.TMP by folder, then name; each folder's run of games into its record */
    if (f_open(out, view_path(SDC_DIRIDX_TMP), FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) goto done;
    out_open = 1;
    _BuildPct = 88;
    s.key = key_dir;
    if (put_hdr(out, &h) || run_sort(&s, emit_dir, b.n, work, cap, &passes) || run_end(&s, 1)) goto done;
    out_open = 0;
    if (f_close(out) != FR_OK) goto done;
    in_open = 0;
    (void)f_close(in);
    /* 3. the folders' counts, then FORDER.TMP: each folder's folders by name (those with games) */
    if (folder_totals(fo, b.nf)) goto done;
    if (f_open(out, view_path(SDC_FORDER_TMP), FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) goto done;
    out_open = 1;
    _BuildPct = 94;
    s.in = fo; s.rsz = SDC_FOLDER_SIZE; s.base = SDC_FOLDER_OFF(1); s.key = key_folder;   /* (from id 1: the top is not sorted) */
    if (put_hdr(out, &h) || run_sort(&s, emit_folder, b.nf ? b.nf - 1 : 0, work, cap, &passes) || run_end(&s, 0)) goto done;
    /* the headers, the magic last (a cut leaves no file that looks whole) */
    h.magic = SDC_IDX_MAGIC;
    h.built_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    if (put_hdr(out, &h)) goto done;
    out_open = 0;
    if (f_close(out) != FR_OK) goto done;
    if (put_hdr(fo, &h)) goto done;
    fo_open = 0;
    if (f_close(fo) != FR_OK) goto done;
    if (hdr_again(out, SDC_DIRIDX_TMP, &h) || hdr_again(out, SDC_INDEX_TMP, &h)) goto done;
    /* the folder files first, the index last: an index is never newer than the folder files it is checked against */
    ix_close();
    aux_close();                                          /* (none of the old files stays open while it is replaced) */
    const char *const from[] = { SDC_FOLDERS_TMP, SDC_FORDER_TMP, SDC_DIRIDX_TMP, SDC_INDEX_TMP };
    const char *const to[]   = { SDC_FOLDERS,     SDC_FORDER,     SDC_DIRIDX,     SDC_INDEX };
    for (int k = 0; k < 4; k++) { (void)f_unlink(view_path(to[k])); if (f_rename(view_path(from[k]), view_path(to[k])) != FR_OK) goto done; }
    (void)f_unlink(SDC_UNSORTED);
    rc = 0;
    printf("sd: library: %lu games in %lu folders indexed in %lu ms (%lu sort passes of %lu; %lu not readable, %lu with too long a path)\n",
           (unsigned long)b.n, (unsigned long)b.nf, (unsigned long)h.built_ms, (unsigned long)passes, (unsigned long)cap,
           (unsigned long)b.skipped, (unsigned long)too_long);
done:
    if (in_open) (void)f_close(in);
    if (out_open) (void)f_close(out);
    if (fo_open) (void)f_close(fo);
    free(work);
    free(w); free(in); free(out); free(fo); heap_caps_free(b.hb);
    _BuildPct = -1;
    _IxGen = ~0u;
    aux_close();
    return rc;
}

/* the library up to date with the card (the walk; a rebuild only when it changed). 0 = an index, 1 = rebuilt */
int SdLib_Refresh(int force)
{
    Sd_Lock();
    if (!Sd_Mounted()) { Sd_Unlock(); return -1; }
    const int64_t t0 = esp_timer_get_time();
    walk_t *w = malloc(sizeof *w);
    if (!w) { Sd_Unlock(); return -2; }
    build_t b = { .fp = sdc_fp_start() };
    uint32_t too_long = 0;
    _BuildPct = 0;
    const int wr = walk(w, fp_fn, NULL, &b, &too_long);
    free(w);
    const uint32_t ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    if (wr < 0) { _BuildPct = -1; Sd_Unlock(); printf("sd: library: the card cannot be read\n"); (void)Sd_Present(); return -3; }
    int have = 1;
    for (int core = -1; core <= 1; core++) {
        _BuildCore = core;
        if (ix_open() || _Fp != b.fp || !folders_ok()) have = 0;
    }
    _BuildCore = -2;
    (void)ix_open();
    /* All three views were checked above; the current view may still be pending. */
    if (have && !force) {
        printf("sd: library: %lu games, unchanged (walk %lu ms)\n", (unsigned long)b.n, (unsigned long)ms);
        _BuildPct = -1;
        Sd_Unlock();
        return 0;
    }
    if (!Sd_Writable()) {                                 /* a read-only card keeps the index it has (or none) */
        printf("sd: library: the card changed but is read only: %s\n", have ? "the old index is used" : "no index");
        _BuildPct = -1;
        Sd_Unlock();
        return have ? 0 : -4;
    }
    printf("sd: library: %lu game files (walk %lu ms): %s\n", (unsigned long)b.n, (unsigned long)ms, have ? "changed, indexing again" : "indexing");
    int r = 0;
    for (int core = 1; core >= -1 && !r; core--) {
        _BuildCore = core;
        r = rebuild(b.fp, b.n);
    }
    _BuildCore = -2;
    if (!r) r = recent_views();
    Sd_Unlock();
    return r == 0 ? 1 : -5;
}
void SdLib_Forget(void) { Sd_Lock(); ix_close(); aux_close(); _IxGen = ~0u; Sd_Unlock(); }
