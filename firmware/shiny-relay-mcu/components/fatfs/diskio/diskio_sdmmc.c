/*
 * SPDX-FileCopyrightText: 2015-2021 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "diskio_impl.h"
#include "ffconf.h"
#include "ff.h"
#include "sdmmc_cmd.h"
#include "esp_log.h"
#include "esp_compiler.h"
#include <stdio.h>
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sd_protocol_defs.h"

static sdmmc_card_t* s_cards[FF_VOLUMES] = { NULL };
static bool s_disk_status_check_en[FF_VOLUMES] = { };

static const char* TAG = "diskio_sdmmc";

//Check if SD/MMC card is present
static DSTATUS ff_sdmmc_card_available(BYTE pdrv)
{
    sdmmc_card_t* card = s_cards[pdrv];
    assert(card);
    esp_err_t err = sdmmc_get_status(card);
    if (unlikely(err != ESP_OK)) {
        ESP_LOGE(TAG, "Check status failed (0x%x)", err);
        return STA_NOINIT;
    }
    return 0;
}

/**
*   ff_sdmmc_status() and ff_sdmmc_initialize() return STA_NOINIT when sdmmc_get_status()
*   fails. This error value is checked throughout the FATFS code.
*   Both functions return 0 on success.
*/
DSTATUS ff_sdmmc_initialize (BYTE pdrv)
{
    return ff_sdmmc_card_available(pdrv);
}

DSTATUS ff_sdmmc_status(BYTE pdrv)
{
    if (s_disk_status_check_en[pdrv]) {
        return ff_sdmmc_card_available(pdrv);
    }
    return 0;
}

DRESULT ff_sdmmc_read (BYTE pdrv, BYTE* buff, DWORD sector, UINT count)
{
    sdmmc_card_t* card = s_cards[pdrv];
    assert(card);
    esp_err_t err = sdmmc_read_sectors(card, buff, sector, count);
    if (unlikely(err != ESP_OK)) err = sdmmc_read_sectors(card, buff, sector, count);   /* lane sdcard1: once more */
    if (unlikely(err != ESP_OK)) {
        printf("[%u ms] sd: card read of %u sectors at %lu failed: 0x%x (buffer %p, free heap %lu)\n", (unsigned)esp_log_timestamp(), (unsigned)count, (unsigned long)sector, err, buff, (unsigned long)esp_get_free_heap_size());
        ESP_LOGE(TAG, "sdmmc_read_blocks failed (0x%x)", err);
        return RES_ERROR;
    }
    return RES_OK;
}

DRESULT ff_sdmmc_write (BYTE pdrv, const BYTE* buff, DWORD sector, UINT count)
{
    sdmmc_card_t* card = s_cards[pdrv];
    assert(card);
    esp_err_t err = sdmmc_write_sectors(card, buff, sector, count);
    /* lane sdcard1: a failed write (a card busy past the host's timeout, a CRC status error) leaves the card in the
       middle of it: STOP_TRANSMISSION, the card's status asked until it is back in transfer state, then again,
       at most 3 more times */
    for (int t = 0; unlikely(err != ESP_OK) && t < 3; t++) {
        sdmmc_command_t stop = { .opcode = MMC_STOP_TRANSMISSION, .flags = SCF_CMD_AC | SCF_RSP_R1B, .timeout_ms = 2000 };
        (void)card->host.do_transaction(card->host.slot, &stop);
        for (int k = 0; k < 200; k++) {
            sdmmc_command_t st = { .opcode = MMC_SEND_STATUS, .arg = MMC_ARG_RCA(card->rca), .flags = SCF_CMD_AC | SCF_RSP_R1, .timeout_ms = 100 };
            if (card->host.do_transaction(card->host.slot, &st) == ESP_OK && MMC_R1_CURRENT_STATE(st.response) == MMC_R1_CURRENT_STATE_TRAN &&
                (MMC_R1(st.response) & MMC_R1_READY_FOR_DATA)) break;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        err = sdmmc_write_sectors(card, buff, sector, count);
        if (err == ESP_OK) printf("sd: card write of %u sectors at %lu went through on try %d\n", (unsigned)count, (unsigned long)sector, t + 2);
    }
    if (unlikely(err != ESP_OK)) {
        printf("sd: card write of %u sectors at %lu failed: 0x%x (buffer %p, free heap %lu)\n", (unsigned)count, (unsigned long)sector, err, buff, (unsigned long)esp_get_free_heap_size());
        ESP_LOGE(TAG, "sdmmc_write_blocks failed (0x%x)", err);
        return RES_ERROR;
    }
    return RES_OK;
}

#if FF_USE_TRIM
DRESULT ff_sdmmc_trim (BYTE pdrv, DWORD start_sector, DWORD sector_count)
{
    sdmmc_card_t* card = s_cards[pdrv];
    assert(card);
    sdmmc_erase_arg_t arg;

    arg = sdmmc_can_discard(card) == ESP_OK ? SDMMC_DISCARD_ARG : SDMMC_ERASE_ARG;
    esp_err_t err = sdmmc_erase_sectors(card, start_sector, sector_count, arg);
    if (unlikely(err != ESP_OK)) {
        ESP_LOGE(TAG, "sdmmc_erase_sectors failed (%d)", err);
        return RES_ERROR;
    }
    return RES_OK;
}
#endif //FF_USE_TRIM

DRESULT ff_sdmmc_ioctl (BYTE pdrv, BYTE cmd, void* buff)
{
    sdmmc_card_t* card = s_cards[pdrv];
    assert(card);
    switch(cmd) {
        case CTRL_SYNC:
            return RES_OK;
        case GET_SECTOR_COUNT:
            *((DWORD*) buff) = card->csd.capacity;
            return RES_OK;
        case GET_SECTOR_SIZE:
            *((WORD*) buff) = card->csd.sector_size;
            return RES_OK;
        case GET_BLOCK_SIZE:
            return RES_ERROR;
#if FF_USE_TRIM
        case CTRL_TRIM:
            if (sdmmc_can_trim(card) != ESP_OK) {
                return RES_PARERR;
            }
            return ff_sdmmc_trim (pdrv, *((DWORD*)buff), //start_sector
                    (*((DWORD*)buff + 1) - *((DWORD*)buff) + 1)); //sector_count
#endif //FF_USE_TRIM
    }
    return RES_ERROR;
}

void ff_sdmmc_set_disk_status_check(BYTE pdrv, bool enable)
{
    s_disk_status_check_en[pdrv] = enable;
}

void ff_diskio_register_sdmmc(BYTE pdrv, sdmmc_card_t* card)
{
    static const ff_diskio_impl_t sdmmc_impl = {
        .init = &ff_sdmmc_initialize,
        .status = &ff_sdmmc_status,
        .read = &ff_sdmmc_read,
        .write = &ff_sdmmc_write,
        .ioctl = &ff_sdmmc_ioctl
    };
    s_cards[pdrv] = card;
    s_disk_status_check_en[pdrv] = false;
    ff_diskio_register(pdrv, &sdmmc_impl);
}

BYTE ff_diskio_get_pdrv_card(const sdmmc_card_t* card)
{
    for (int i = 0; i < FF_VOLUMES; i++) {
        if (card == s_cards[i]) {
            return i;
        }
    }
    return 0xff;
}
