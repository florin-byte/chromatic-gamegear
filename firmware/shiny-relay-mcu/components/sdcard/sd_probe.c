/* sd_probe.c: lane sdcard1 bench probe of the Chromatic's internal microSD slot (how it is wired).
 *   sd pins            the levels of the free ESP32 pins with the pull-down, then the pull-up on
 *   sd mmc W KHZ       an SDMMC slot 1 card init (IO_MUX pins 14 CLK, 15 CMD, 2 D0, 4 D1, 12 D2, 13 D3), W = 1 or 4
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "sdmmc_cmd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sd_probe.h"

static const int kPins[] = { 0, 2, 4, 12, 13, 14, 15, 32, 34, 39, 37, 38 };

void SdProbe_Pins(void)
{
    printf("sd: pin  pulldown pullup float\n");
    for (unsigned i = 0; i < sizeof kPins / sizeof kPins[0]; i++) {
        const int p = kPins[i];
        if (!GPIO_IS_VALID_GPIO(p)) continue;
        int lv[3] = { -1, -1, -1 };
        const gpio_pull_mode_t m[3] = { GPIO_PULLDOWN_ONLY, GPIO_PULLUP_ONLY, GPIO_FLOATING };
        for (int k = 0; k < 3; k++) {
            gpio_reset_pin(p);
            gpio_set_direction(p, GPIO_MODE_INPUT);
            if (p < 34) gpio_set_pull_mode(p, m[k]);
            vTaskDelay(pdMS_TO_TICKS(5));
            lv[k] = gpio_get_level(p);
        }
        if (p < 34) gpio_set_pull_mode(p, GPIO_FLOATING);
        printf("sd: %3d  %d        %d      %d\n", p, lv[0], lv[1], lv[2]);
    }
    printf("sd: pins e=0\n");
}

int SdProbe_Mmc(int width, int khz)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_1;
    host.max_freq_khz = khz;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = (uint8_t)width;
    slot.flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    sdmmc_card_t *card = calloc(1, sizeof *card);
    if (!card) return -1;
    esp_err_t e = sdmmc_host_init();
    if (e == ESP_OK) e = sdmmc_host_init_slot(SDMMC_HOST_SLOT_1, &slot);
    const int64_t t0 = esp_timer_get_time();
    if (e == ESP_OK) e = sdmmc_card_init(&host, card);
    printf("sd: mmc width %d %d kHz: card_init 0x%x (%s) in %lld ms\n", width, khz, e, esp_err_to_name(e), (long long)(esp_timer_get_time() - t0) / 1000);
    if (e == ESP_OK) {
        sdmmc_card_print_info(stdout, card);
        uint8_t *blk = heap_caps_malloc(4096, MALLOC_CAP_DMA);
        if (!blk) { sdmmc_host_deinit(); free(card); return -1; }
        const int64_t t1 = esp_timer_get_time();
        int n = 0;
        for (; n < 256; n++) if (sdmmc_read_sectors(card, blk, (size_t)n * 8, 8) != ESP_OK) break;
        const int64_t us = esp_timer_get_time() - t1;
        printf("sd: read %d x 4 KiB in %lld us (%.2f MB/s); sector 0 ends %02x %02x\n", n, (long long)us, us ? (n * 4096.0) / us : 0.0, blk[510], blk[511]);
        printf("sd: cid mfg 0x%02x oem 0x%04x name %s rev %d serial %08x date %d\n", card->cid.mfg_id, card->cid.oem_id, card->cid.name, card->cid.revision, (unsigned)card->cid.serial, card->cid.date);
        if (sdmmc_read_sectors(card, blk, 0, 1) == ESP_OK) {
            const uint8_t *pe = blk + 446;
            const uint32_t lba = pe[8] | pe[9] << 8 | pe[10] << 16 | (uint32_t)pe[11] << 24;
            const uint32_t cnt = pe[12] | pe[13] << 8 | pe[14] << 16 | (uint32_t)pe[15] << 24;
            printf("sd: sector 0 sig %02x%02x, partition 1 type 0x%02x lba %lu sectors %lu\n", blk[510], blk[511], pe[4], (unsigned long)lba, (unsigned long)cnt);
            if (lba && sdmmc_read_sectors(card, blk, lba, 1) == ESP_OK)
                printf("sd: boot sector oem '%.8s' fat32 '%.8s' sig %02x%02x\n", blk + 3, blk + 82, blk[510], blk[511]);
        }
        heap_caps_free(blk);
    }
    sdmmc_host_deinit();
    free(card);
    return e == ESP_OK ? 0 : 1;
}

