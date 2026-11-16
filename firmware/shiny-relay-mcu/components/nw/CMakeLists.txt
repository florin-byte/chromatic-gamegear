cmake_minimum_required(VERSION 3.22)

set(srcs "nw_dev.c" "nw_esp.c" "nw_link.c" "shl_client.c" "party_ask.c")
set(incs ".")
if(CONFIG_NW_CARTFLASH)
    # lane cartflash1 (DRAFT): off by default, see cartflash/cf_proto.h
    list(APPEND srcs "cartflash/cf_esp.c" "cartflash/cf_chips.c" "cartflash/cf_dev.c" "cartflash/cf_usb.c" "cartflash/cf_sha256.c" "cartflash/cf_rc_dev.c" "cartflash/cf_fota.c" "cartflash/cf_sgz.c" "cartflash/cf_inflate.c" "cartflash/cf_lib.c")
    if(CONFIG_NW_CARTFLASH_WIFI)
        # the Wi-Fi install ("Send to my Chromatic"): not in the USB-only build
        list(APPEND srcs "cartflash/cf_fetch.c" "cartflash/cf_install.c" "cartflash/cf_bps.c" "cartflash/cf_ota.c" "cartflash/cf_pack.c" "cartflash/cf_share.c" "cartflash/cf_share_dev.c")
    endif()
    list(APPEND incs "cartflash")
endif()

idf_component_register(SRCS ${srcs}
                    INCLUDE_DIRS ${incs}
                    REQUIRES sdcard cwl battery crc console esp_driver_uart esp_http_client esp_timer nvs_flash
                             esp_app_format mbedtls lwip esp_system esp_partition app_update bootloader_support spi_flash)
# every function's stack frame is written next to its object (.su): tools/check_stack.py reads
# them for the console handlers (usb-cart-1 overflowed console_repl's 4 KB with a 3.4 KB frame)
target_compile_options(${COMPONENT_LIB} PRIVATE -fstack-usage)
if(CONFIG_NW_CARTFLASH)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE NW_CARTFLASH=1)
endif()
# lane gamecomp1: the SGZ1 unpack is CPU bound on a play's start: -O2 (the build default is -Os)
if(CONFIG_NW_CARTFLASH)
    set_source_files_properties(cartflash/cf_inflate.c cartflash/cf_sgz.c cartflash/cf_pack.c PROPERTIES COMPILE_FLAGS -O2)   # (lane ui110dev: the packer too)
endif()
