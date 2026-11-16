cmake_minimum_required(VERSION 3.22)
# lane sdcard1: the microSD card (sd.h)
idf_component_register(SRCS "sd.c" "sd_card.c" "sd_lib.c" "sd_file.c" "sd_core.c" "sd_probe.c"
                    INCLUDE_DIRS "."
                    REQUIRES console fatfs esp_driver_gpio esp_driver_sdmmc esp_driver_spi sdmmc esp_timer spi_flash app_update)
target_compile_options(${COMPONENT_LIB} PRIVATE -fstack-usage)
