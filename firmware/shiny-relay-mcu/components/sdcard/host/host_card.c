/* host_card.c (lane sdcard1): the card under the library on the Mac: a RAM disk (FatFs diskio), the lock, the
 * card state calls sd_lib.c / sd_file.c use. A test can fail writes from a given count on (a pulled card). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ff.h"
#include "diskio.h"
#include "sd.h"

uint8_t *host_disk; uint32_t host_sectors; long host_fail_after = -1; long host_writes;
static FATFS host_fs; static int mounted, gen, rw = 1;
uint32_t host_free_heap = 60000;

DSTATUS ff_disk_initialize(BYTE p) { (void)p; return 0; }
DSTATUS ff_disk_status(BYTE p) { (void)p; return 0; }
DRESULT ff_disk_read(BYTE p, BYTE *b, LBA_t s, UINT n) { (void)p; if (s + n > host_sectors) return RES_PARERR; memcpy(b, host_disk + (size_t)s * 512, (size_t)n * 512); return RES_OK; }
DRESULT ff_disk_write(BYTE p, const BYTE *b, LBA_t s, UINT n)
{
    (void)p;
    if (s + n > host_sectors) return RES_PARERR;
    if (host_fail_after >= 0 && host_writes >= host_fail_after) return RES_ERROR;
    host_writes++;
    memcpy(host_disk + (size_t)s * 512, b, (size_t)n * 512);
    return RES_OK;
}
DRESULT ff_disk_ioctl(BYTE p, BYTE c, void *b)
{
    (void)p;
    if (c == CTRL_SYNC) return RES_OK;
    if (c == GET_SECTOR_COUNT) { *(LBA_t *)b = host_sectors; return RES_OK; }
    if (c == GET_SECTOR_SIZE) { *(WORD *)b = 512; return RES_OK; }
    if (c == GET_BLOCK_SIZE) { *(DWORD *)b = 1; return RES_OK; }
    return RES_PARERR;
}
void *ff_memalloc(UINT n) { return malloc(n); }
void ff_memfree(void *p) { free(p); }
int ff_mutex_create(int v) { (void)v; return 1; }
void ff_mutex_delete(int v) { (void)v; }
int ff_mutex_take(int v) { (void)v; return 1; }
void ff_mutex_give(int v) { (void)v; }

void Sd_Lock(void) {}
int Sd_TryLock(uint32_t ms) { (void)ms; return 1; }
void Sd_Unlock(void) {}
int Sd_Mounted(void) { return mounted; }
int Sd_Writable(void) { return mounted && rw; }
uint32_t Sd_Generation(void) { return (uint32_t)gen; }
int Sd_Present(void) { return mounted; }
int Sd_State(void) { return mounted ? (rw ? kSdSt_Ready : kSdSt_ReadOnly) : kSdSt_None; }
int Sd_Mount(void) { if (mounted) return 0; if (f_mount(&host_fs, "", 1) != FR_OK) return -2; mounted = 1; gen++; return 0; }
void Sd_Unmount(void) { if (mounted) { f_unmount(""); mounted = 0; gen++; } }
void host_readonly(int ro) { rw = !ro; }
int Sd_FileSize(const char *path, uint32_t *size)
{
    FILINFO fi;
    const FRESULT r = f_stat(path, &fi);
    if (r == FR_OK && size) *size = (uint32_t)fi.fsize;
    return r == FR_OK ? 0 : 1;
}
const PARTITION VolToPart[FF_VOLUMES] = { { 0, 0 } };   /* (the IDF diskio.c one: drive 0, the first partition or none) */
