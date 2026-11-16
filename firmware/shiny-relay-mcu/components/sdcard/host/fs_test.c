/* fs_test.c (lane sdcard1): the card library over the firmware's own FatFs (exFAT on, 512-byte sectors) on a RAM
 * disk on the Mac: FORMAT (FAT32 and exFAT, as Sd_Format makes them), games found in any folder, the index rebuilt
 * only when the card changed, sorted by name in bounded passes, crash-safe writes and a cut in each of their steps,
 * a pulled card. `fs_test IMAGE` also leaves the FAT32 card's image for macOS to read (make mac-check). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ff.h"
#include "sd.h"

extern uint8_t *host_disk; extern uint32_t host_sectors; extern long host_fail_after, host_writes; extern uint32_t host_free_heap;
void host_readonly(int ro);
static int fails, passes;
static void check(int ok, const char *what) { if (ok) passes++; else { fails++; printf("FAIL: %s\n", what); } }

static void rom(uint8_t *b, uint32_t size, const char *title, uint8_t type, uint8_t rs, uint8_t ra)
{
    memset(b, 0xFF, size);
    memset(b + 0x100, 0, 0x50);
    memcpy(b + 0x134, title, strlen(title) > 15 ? 15 : strlen(title));
    b[0x143] = 0x80; b[0x147] = type; b[0x148] = rs; b[0x149] = ra;
}
static int put(const char *path, const uint8_t *b, uint32_t n)
{
    FIL f; UINT w = 0;
    char d[128]; snprintf(d, sizeof d, "%s", path);
    for (char *p = d + 1; *p; p++) if (*p == '/') { *p = 0; f_mkdir(d); *p = '/'; }
    if (f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return -1;
    const FRESULT r = f_write(&f, b, n, &w);
    f_close(&f);
    return r == FR_OK && w == n ? 0 : -1;
}
static uint32_t rnd = 7;
static uint32_t next(void) { rnd = rnd * 1103515245u + 12345u; return rnd >> 8; }

static void format(BYTE fmt)
{
    Sd_Unmount();
    memset(host_disk, 0, (size_t)host_sectors * 512);
    static uint8_t work[4096];
    MKFS_PARM o = { .fmt = fmt };
    check(f_mkfs("", &o, work, sizeof work) == FR_OK, fmt == FM_EXFAT ? "f_mkfs exFAT" : "f_mkfs FAT32");
    check(Sd_Mount() == 0, "mounted after the format");
    f_mkdir(SDC_SHINY_DIR); f_mkdir(SDC_GAMES_DIR); f_mkdir(SDC_SAVES_DIR);
}

/* fr1: the folder view: / has GAMES and OTHER GAMES (by name), GAMES has its games and POKEMON, POKEMON has GEN 1;
   every folder's games are one run of LIBDIR in name order, and the counts add up */
static void t_folders(const char *fsname, uint32_t want)
{
    char what[160];
    const int nf = SdLib_Folders();
    for (int q = 0; getenv("FDUMP") && q < nf; q++) { sdc_folder_t z; if (SdLib_Folder((uint32_t)q, &z) == 0) printf("folder %d '%s' parent %u depth %u child %u+%u games %u+%u total %u\n", q, z.name, (unsigned)z.parent, (unsigned)z.depth, (unsigned)z.child_first, (unsigned)z.child_count, (unsigned)z.game_first, (unsigned)z.game_count, (unsigned)z.total); }
    for (uint32_t q = 0; getenv("FDUMP") && q < 8; q++) { uint32_t id = 99; int e = SdLib_FolderChild(q, &id); printf("order %u: %d id %u\n", (unsigned)q, e, (unsigned)id); }
    snprintf(what, sizeof what, "%s: the folder view is there (%d folders)", fsname, nf);
    check(nf >= 5, what);
    sdc_folder_t top, g, o, pk, g1;
    uint32_t id0 = 0, id1 = 0, idp = 0, idg = 0;
    int ok = SdLib_Folder(0, &top) == 0 && top.total == want && top.child_count == 2 && top.game_count == 0 &&
             SdLib_FolderChild(top.child_first, &id0) == 0 && SdLib_FolderChild(top.child_first + 1, &id1) == 0 &&
             SdLib_Folder(id0, &g) == 0 && SdLib_Folder(id1, &o) == 0 &&
             !strcmp(g.name, "GAMES") && !strcmp(o.name, "Other Games");
    snprintf(what, sizeof what, "%s: the top: GAMES, then Other Games (games %u)", fsname, (unsigned)top.total);
    check(ok, what);
    ok = ok && g.child_count == 1 && SdLib_FolderChild(g.child_first, &idp) == 0 && SdLib_Folder(idp, &pk) == 0 &&
         !strcmp(pk.name, "Pokemon") && pk.game_count == 0 && pk.child_count == 1 &&
         SdLib_FolderChild(pk.child_first, &idg) == 0 && SdLib_Folder(idg, &g1) == 0 && !strcmp(g1.name, "Gen 1") &&
         g1.parent == idp && pk.parent == id0 && g.parent == 0;
    snprintf(what, sizeof what, "%s: GAMES / Pokemon / Gen 1", fsname);
    check(ok, what);
    ok = ok && g.game_count + o.game_count + g1.game_count == want && g.total == g.game_count + g1.game_count && pk.total == g1.game_count;
    snprintf(what, sizeof what, "%s: the counts add up (%u + %u + %u)", fsname, (unsigned)g.game_count, (unsigned)o.game_count, (unsigned)g1.game_count);
    check(ok, what);
    const sdc_folder_t *fs[3] = { &g, &o, &g1 };
    const uint32_t ids[3] = { id0, id1, idg };
    int runs = ok;
    for (int k = 0; k < 3 && runs; k++) {
        char prev[SDC_NAME_LEN] = "";
        for (uint32_t i = 0; i < fs[k]->game_count; i++) {
            sdc_rec_t r;
            if (SdLib_DirGet(fs[k]->game_first + i, &r) || r.folder != ids[k] || strncmp(prev, r.title, 13) > 0) { runs = 0; break; }
            snprintf(prev, sizeof prev, "%s", r.title);
        }
    }
    snprintf(what, sizeof what, "%s: each folder's games: one run, in name order", fsname);
    check(runs, what);
    check(SdLib_RecentAdd("/GAMES/a.gb", "A") == 0 && SdLib_RecentAdd("/GAMES/b.gb", "B") == 0 && SdLib_RecentAdd("/GAMES/a.gb", "A") == 0, "recent: three plays");
    char p[SDC_PATH_MAX], t[SDC_NAME_LEN];
    check(SdLib_Recent(0, p, sizeof p, t, sizeof t) == 0 && !strcmp(p, "/GAMES/a.gb") && !strcmp(t, "A") &&
          SdLib_Recent(1, p, sizeof p, t, sizeof t) == 0 && !strcmp(p, "/GAMES/b.gb") && SdLib_Recent(2, p, sizeof p, t, sizeof t) == -1,
          "recent: the newest first, a game once");
}

/* the library: N games in folders, some not games, a hidden folder, a too-long path; sorted and complete */
static void t_library(const char *fsname, uint32_t n, uint32_t heap)
{
    static uint8_t b[0x8000];
    char what[160];
    static char titles[3000][17];
    uint32_t want = 0;
    for (uint32_t i = 0; i < n; i++) {
        char t[16], p[128];
        const int len = 3 + (int)(next() % 10);
        for (int k = 0; k < len; k++) t[k] = (char)('A' + next() % 26);
        t[len] = 0;
        rom(b, sizeof b, t, 0x1B, 0, 3);
        const char *dir = (i % 3 == 0) ? "/GAMES" : (i % 3 == 1) ? "/GAMES/Pokemon/Gen 1" : "/Other Games";
        snprintf(p, sizeof p, "%s/game %04u.%s", dir, (unsigned)i, i % 2 ? "gbc" : "gb");
        if (put(p, b, sizeof b) == 0) { memcpy(titles[want], t, 17); want++; }
    }
    put("/GAMES/readme.txt", b, 100);
    put("/.hidden/x.gb", b, sizeof b);
    put("/SAVES/not a game.gb", b, sizeof b);
    /* fr1: paths up to 159 bytes are games now; this one (171) is still too long */
    put("/GAMES/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.gb", b, sizeof b);
    host_free_heap = heap;
    SdLib_Forget();
    const int r = SdLib_Refresh(0);
    snprintf(what, sizeof what, "%s: %u games indexed (heap %u: the sort's passes)", fsname, (unsigned)want, (unsigned)heap);
    check(r == 1 && SdLib_Count() == (int)want, what);
    int sorted = 1, all = 1;
    char prev[SDC_NAME_LEN] = "";
    for (uint32_t i = 0; i < want; i++) {
        sdc_rec_t rec;
        if (SdLib_Get(i, &rec)) { all = 0; break; }
        if (strncmp(prev, rec.title, 16) > 0) sorted = 0;
        snprintf(prev, sizeof prev, "%s", rec.title);
        if (!(rec.flags & SDC_F_PLAYABLE) || strstr(rec.path, ".hidden") || strstr(rec.path, "/SAVES/")) all = 0;
    }
    snprintf(what, sizeof what, "%s: the library in name order, every record a playable game outside hidden / SAVES", fsname);
    check(sorted && all, what);
    t_folders(fsname, want);
    snprintf(what, sizeof what, "%s: a second look finds it unchanged (no rebuild)", fsname);
    check(SdLib_Refresh(0) == 0, what);
    rom(b, sizeof b, "ZZZ NEW", 0x1B, 0, 3);
    put("/GAMES/new one.gbc", b, sizeof b);
    snprintf(what, sizeof what, "%s: a game added on a computer: rebuilt, one more", fsname);
    check(SdLib_Refresh(0) == 1 && SdLib_Count() == (int)want + 1, what);
    snprintf(what, sizeof what, "%s: the new game found by its path", fsname);
    check(SdLib_Find("/GAMES/new one.gbc") == (int)want, what);
    f_unlink("/GAMES/new one.gbc");
    check(SdLib_Refresh(0) == 1 && SdLib_Count() == (int)want, "a game deleted: one less");
}

static int read_all(const char *path, uint8_t *b, uint32_t cap, uint32_t *n)
{
    FIL f;
    uint32_t sz = 0, got = 0;
    if (Sd_ReadOpen(&f, path, &sz) != 0) return -1;
    if (sz > cap) sz = cap;
    const int r = Sd_Read(&f, 0, b, sz, &got);
    Sd_Close(&f);
    *n = got;
    return r;
}

static void t_saves(void)
{
    static uint8_t a[0x8030], b[0x8030];
    uint32_t n = 0;
    sd_wr_t w;
    memset(a, 0x11, sizeof a);
    check(Sd_WriteBegin(&w, "/SAVES/Game.sav", 0) == 0 && Sd_WriteAt(&w, 0, a, sizeof a) == 0 && Sd_WriteCommit(&w) == 0, "a save written (temp, then renamed in)");
    check(read_all("/SAVES/Game.sav", b, sizeof b, &n) == 0 && n == sizeof a && !memcmp(a, b, n), "and read back whole");
    check(Sd_Exists("/SAVES/Game.sav.new") == 0, "no temp file left");
    /* a cut while the new copy is written: the old save stays */
    memset(a, 0x22, sizeof a);
    check(Sd_WriteBegin(&w, "/SAVES/Game.sav", 0) == 0 && Sd_WriteAt(&w, 0, a, 1000) == 0, "a second save half written");
    f_close(&w.f);                                         /* (the cut: nothing more happens to it) */
    check(read_all("/SAVES/Game.sav", b, sizeof b, &n) == 0 && b[0] == 0x11 && n == sizeof a, "cut mid-write: the old save is read");
    check(!Sd_Exists("/SAVES/Game.sav.new"), "and the partial copy beside it dropped");
    /* a cut between removing the old one and the rename: the whole new copy is taken */
    memset(a, 0x33, sizeof a);
    check(Sd_WriteBegin(&w, "/SAVES/Game.sav", 0) == 0 && Sd_WriteAt(&w, 0, a, sizeof a) == 0, "a third save written whole");
    f_sync(&w.f); f_close(&w.f); f_unlink("/SAVES/Game.sav");   /* (the cut right after the old one went) */
    check(read_all("/SAVES/Game.sav", b, sizeof b, &n) == 0 && b[0] == 0x33 && n == sizeof a, "cut before the rename: the whole new save is taken");
    /* a pulled card while a save is written: an error, and the old save is still the one on the card */
    host_fail_after = host_writes + 2;
    memset(a, 0x44, sizeof a);
    int e = Sd_WriteBegin(&w, "/SAVES/Game.sav", 0);
    if (!e) e = Sd_WriteAt(&w, 0, a, sizeof a);
    if (!e) e = Sd_WriteCommit(&w); else Sd_WriteAbort(&w);
    host_fail_after = -1;
    check(e != 0, "a pulled card: the write fails (no crash)");
    Sd_Unmount(); Sd_Mount();
    check(read_all("/SAVES/Game.sav", b, sizeof b, &n) == 0 && b[0] == 0x33, "the card back: the last whole save is there");
    /* a locked card: nothing written */
    host_readonly(1);
    check(Sd_WriteBegin(&w, "/SAVES/Other.sav", 0) != 0, "a read-only card: no write starts");
    check(SdLib_Refresh(0) >= 0, "a read-only card keeps its library");
    host_readonly(0);
    /* names */
    char p[SDC_PATH_MAX];
    check(Sd_FreeName(SDC_GAMES_DIR, "game 0000.gb", p, sizeof p) == 0 && !strcmp(p, "/GAMES/game 0000 (2).gb"), "a taken name: (2)");
    /* fr3j: a move makes the target's folders, takes a folder with what is in it, never lands on a taken name */
    check(Sd_WriteBegin(&w, "/GAMES/move me.bin", 0) == 0 && Sd_WriteAt(&w, 0, b, 512) == 0 && Sd_WriteCommit(&w) == 0, "move: a file to move");
    check(Sd_Move("/GAMES/move me.bin", "/Sega GG/01 Platformers/move me.bin") == 0 && Sd_Exists("/Sega GG/01 Platformers/move me.bin") &&
          !Sd_Exists("/GAMES/move me.bin"), "move: a file into folders that were not there");
    check(Sd_Move("/Sega GG/01 Platformers", "/All/Sega GG/01 Platformers") == 0 && Sd_Exists("/All/Sega GG/01 Platformers/move me.bin"),
          "move: a folder with its file");
    check(Sd_Move("/GAMES/game 0000.gb", "/All/Sega GG/01 Platformers/move me.bin") != 0 && Sd_Exists("/GAMES/game 0000.gb"),
          "move: a taken name is refused, the file stays");
    check(Sd_Move("/GAMES/not there.gb", "/All/x.gb") != 0 && Sd_Move("GAMES/game 0000.gb", "/All/y.gb") != 0, "move: no source, no root: refused");
}

static int running_core = -1;
static int core(void) { return running_core; }
static void t_core_views(void)
{
    static uint8_t b[0x8000];
    rom(b, sizeof b, "GB", 0x1B, 0, 3);
    check(!put("/Mixed/GB.gb", b, sizeof b), "core: GB in mixed folder");
    memcpy(b + 0x150, "GGCHROM1", 8);
    check(!put("/Mixed/GG.gb", b, sizeof b) && !put("/GG only/Nested/GG.gb", b, sizeof b), "core: GG marker, mixed and nested folders");
    SdLib_SetCore(core);
    check(SdLib_Refresh(1) == 1, "core: build all three views");
    int total = SdLib_Count();
    check(!SdLib_RecentAdd("/Mixed/GB.gb", "GB") && !SdLib_RecentAdd("/Mixed/GG.gb", "GG"), "core: mixed history");
    running_core = -2;
    check(SdLib_CorePending() && SdLib_Count() == -1 && SdLib_Total() == total, "core: pending hides games, total still available");
    /* A mount opens the library before the FPGA slot is known. All stored views
       still describe the same card; neither this refresh nor resolving the slot writes it. */
    SdLib_Forget();
    const long writes = host_writes;
    check(SdLib_Refresh(0) == 0 && host_writes == writes,
          "core: unchanged mount while pending: no rebuild or write");
    check(SdLib_CorePending() && SdLib_Count() == -1 && SdLib_Total() == total,
          "core: unchanged pending index keeps total and hides shown games");
    sdc_rec_t pending_rec;
    char pending_path[SDC_PATH_MAX], pending_title[SDC_NAME_LEN];
    check(SdLib_Get(0, &pending_rec) != 0 && SdLib_Recent(0, pending_path, sizeof pending_path, pending_title, sizeof pending_title) != 0,
          "core: pending has no startable record or recent game");
    for (int c = 0; c < 2; c++) {
        running_core = c;
        check(SdLib_Refresh(0) == 0 && host_writes == writes,
              "core: slot resolved on unchanged card: no rebuild or write");
        check(SdLib_Count() == (c ? 2 : total - 2) && SdLib_Total() == total, "core: shown and total counts");
        int ok = 1;
        for (int i = 0; i < SdLib_Count(); i++) {
            sdc_rec_t r;
            if (SdLib_Get((uint32_t)i, &r) || !!(r.flags & SDC_F_GG) != c) ok = 0;
        }
        check(ok, "core: every A-Z record matches the running core");
        sdc_folder_t top;
        check(!SdLib_Folder(0, &top) && top.total == (uint32_t)SdLib_Count(), "core: folder total matches A-Z");
        ok = 1;
        for (uint32_t k = 0; k < top.child_count; k++) {
            uint32_t id; sdc_folder_t f;
            if (SdLib_FolderChild(top.child_first + k, &id) || SdLib_Folder(id, &f) || !f.total) { ok = 0; break; }
            if (!c && !strcmp(f.name, "GG only")) ok = 0;
            for (uint32_t j = 0; j < f.game_count; j++) {
                sdc_rec_t r;
                if (SdLib_DirGet(f.game_first + j, &r) || !!(r.flags & SDC_F_GG) != c) ok = 0;
            }
        }
        check(ok, "core: visible folders and their direct game runs");
        char path[SDC_PATH_MAX], title[SDC_NAME_LEN];
        check(!SdLib_Recent(0, path, sizeof path, title, sizeof title) && !strcmp(title, c ? "GG" : "GB"), "core: recent view skips the other core");
    }
    running_core = -1;
    check(SdLib_Count() == total && SdLib_Refresh(0) == 0, "core: unknown shows all without rebuilding");
}

int main(int argc, char **argv)
{
    host_sectors = 128u * 2048u;                            /* a 128 MiB card */
    host_disk = calloc(host_sectors, 512);
    format(FM_EXFAT);
    t_library("exFAT", 600, 60000);
    t_saves();
    if (argc >= 3) {                                        /* the exFAT card's image too */
        Sd_Unmount();
        FILE *f = fopen(argv[2], "wb");
        check(f && fwrite(host_disk, 512, host_sectors, f) == host_sectors, "the exFAT card's image written for macOS");
        if (f) fclose(f);
        Sd_Mount();
    }
    format(FM_FAT32);
    t_library("FAT32", 600, 24000 + 40 * 20);               /* a starved heap: the sort works 40 keys at a time */
    t_saves();
    /* a few named games for the Mac-side look at the image */
    static uint8_t b[0x8000];
    rom(b, sizeof b, "SHINY TEST", 0x1B, 0, 3);
    put("/GAMES/Shiny Test.gbc", b, sizeof b);
    SdLib_Refresh(0);
    t_core_views();
    Sd_Unmount();
    if (argc >= 2) {
        FILE *f = fopen(argv[1], "wb");
        check(f && fwrite(host_disk, 512, host_sectors, f) == host_sectors, "the FAT32 card's image written for macOS");
        if (f) fclose(f);
    }
    printf("fs_test: %d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
