/* sd_file.c: lane sdcard1: crash-safe files on the card (saves, games banked over Wi-Fi, cart backups).
 * A file is written whole as "<name>.new" and synced; only then is the old "<name>" removed and the
 * new one renamed in. A pulled card or a power cut leaves either the old file (the .new partial: it is
 * dropped) or no old file and a whole .new (taken: Sd_ReadOpen renames it in). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ff.h"
#include "sd.h"
#include "sd_core.h"

static int tmp_name(const char *dst, char *out, size_t n)
{
    const int w = snprintf(out, n, "%s.new", dst);
    return (w < 0 || (size_t)w >= n) ? -1 : 0;
}

/* the folders of a path made (one level below the root is all the unit ever writes) */
static void mkdirs(const char *path)
{
    char d[SDC_PATH_MAX + 8];
    snprintf(d, sizeof d, "%s", path);
    for (char *p = d + 1; *p; p++) if (*p == '/') { *p = 0; (void)f_mkdir(d); *p = '/'; }
}

int Sd_WriteBegin(sd_wr_t *w, const char *dst, uint32_t size_hint)
{
    memset(w, 0, sizeof *w);
    if (!Sd_Writable()) return -1;
    if (snprintf(w->dst, sizeof w->dst, "%s", dst) >= (int)sizeof w->dst || tmp_name(dst, w->tmp, sizeof w->tmp)) return -2;
    Sd_Lock();
    mkdirs(dst);
    FRESULT r = f_open(&w->f, w->tmp, FA_CREATE_ALWAYS | FA_WRITE);
    if (r == FR_OK && size_hint) (void)f_expand(&w->f, size_hint, 1);   /* (contiguous when the card has the room: faster reads later) */
    if (r == FR_OK && size_hint) (void)f_lseek(&w->f, 0);
    Sd_Unlock();
    if (r != FR_OK) { printf("sd: %s: cannot be written (FatFs %d)\n", w->tmp, r); if (r == FR_DISK_ERR || r == FR_NOT_READY) (void)Sd_Present(); return -3; }
    w->open = 1;
    return 0;
}
int Sd_WriteAt(sd_wr_t *w, uint32_t off, const void *b, uint32_t n)
{
    if (!w->open) return -1;
    Sd_Lock();
    UINT k = 0;
    FRESULT r = FR_OK;
    if (f_tell(&w->f) != off) r = f_lseek(&w->f, off);
    if (r == FR_OK) r = f_write(&w->f, b, n, &k);
    Sd_Unlock();
    if (r != FR_OK || k != n) { printf("sd: %s: write at %lu failed (FatFs %d, %u of %lu)\n", w->tmp, (unsigned long)off, r, (unsigned)k, (unsigned long)n); return r == FR_OK ? -4 : -3; }
    return 0;
}
int Sd_WriteCommit(sd_wr_t *w)
{
    if (!w->open) return -1;
    Sd_Lock();
    FRESULT r = f_truncate(&w->f);                       /* (f_expand gave it the hinted size) */
    if (r == FR_OK) r = f_sync(&w->f);
    const FRESULT c = f_close(&w->f);
    w->open = 0;
    if (r == FR_OK) r = c;
    if (r == FR_OK) {
        const FRESULT u = f_unlink(w->dst);
        if (u != FR_OK && u != FR_NO_FILE) r = u;
    }
    if (r == FR_OK) r = f_rename(w->tmp, w->dst);
    if (r != FR_OK) (void)f_unlink(w->tmp);
    Sd_Unlock();
    if (r != FR_OK) { printf("sd: %s: not saved (FatFs %d)\n", w->dst, r); return -3; }
    return 0;
}
void Sd_WriteAbort(sd_wr_t *w)
{
    if (!w->open) return;
    Sd_Lock();
    (void)f_close(&w->f);
    (void)f_unlink(w->tmp);
    w->open = 0;
    Sd_Unlock();
}

/* open a file for reading, taking a whole .new left by a cut (see the top). 0 = open */
int Sd_ReadOpen(FIL *f, const char *path, uint32_t *size)
{
    char tmp[SDC_PATH_MAX + 8];
    if (tmp_name(path, tmp, sizeof tmp)) return -2;
    Sd_Lock();
    FRESULT r = f_open(f, path, FA_READ);
    if (Sd_Writable()) {
        if (r == FR_NO_FILE && f_rename(tmp, path) == FR_OK) { printf("sd: %s: the whole copy left by a cut taken\n", path); r = f_open(f, path, FA_READ); }
        else if (r == FR_OK) (void)f_unlink(tmp);        /* (a partial copy beside a whole file: dropped; usually FR_NO_FILE) */
    }
    if (r == FR_OK && size) *size = (uint32_t)f_size(f);
    Sd_Unlock();
    if (r != FR_OK && r != FR_NO_FILE) printf("sd: %s: cannot be opened (FatFs %d)\n", path, r);
    return r == FR_OK ? 0 : (r == FR_NO_FILE || r == FR_NO_PATH) ? 1 : -1;
}
int Sd_Read(FIL *f, uint32_t off, void *b, uint32_t n, uint32_t *got)
{
    Sd_Lock();
    UINT k = 0;
    FRESULT r = FR_OK;
    if (f_tell(f) != off) r = f_lseek(f, off);
    if (r == FR_OK) r = f_read(f, b, n, &k);
    Sd_Unlock();
    if (got) *got = k;
    return r == FR_OK ? 0 : -1;
}
void Sd_Close(FIL *f) { Sd_Lock(); (void)f_close(f); Sd_Unlock(); }

int Sd_Delete(const char *path)
{
    if (!Sd_Writable()) return -1;
    Sd_Lock();
    const FRESULT r = f_unlink(path);
    Sd_Unlock();
    return r == FR_OK ? 0 : -1;
}
int Sd_Rename(const char *from, const char *to)
{
    if (!Sd_Writable()) return -1;
    Sd_Lock();
    const FRESULT r = f_rename(from, to);
    Sd_Unlock();
    return r == FR_OK ? 0 : -1;
}
/* fr3j: a file or a folder moved (never onto a name that exists), the folders of `to` made first; 0 = moved */
int Sd_Move(const char *from, const char *to)
{
    if (!Sd_Writable() || !from || !to || from[0] != '/' || to[0] != '/' || strlen(to) >= SDC_PATH_MAX) return -1;
    Sd_Lock();
    mkdirs(to);
    const FRESULT r = f_rename(from, to);
    Sd_Unlock();
    return r == FR_OK ? 0 : -1;
}
int Sd_Exists(const char *path) { uint32_t s; Sd_Lock(); const int r = Sd_FileSize(path, &s) == 0; Sd_Unlock(); return r; }

/* a free name in /GAMES for `file` (already clean): file, else "name (2).ext", ... 0 = out holds it */
int Sd_FreeName(const char *dir, const char *file, char *out, size_t n)
{
    const char *d = strrchr(file, '.');
    const int bl = d ? (int)(d - file) : (int)strlen(file);
    for (int k = 1; k < 100; k++) {
        const int w = k == 1 ? snprintf(out, n, "%s/%s", dir, file) : snprintf(out, n, "%s/%.*s (%d)%s", dir, bl, file, k, d ? d : "");
        if (w < 0 || (size_t)w >= n) return -1;
        if (!Sd_Exists(out)) return 0;
    }
    return -1;
}
