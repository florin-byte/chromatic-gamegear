cmake_minimum_required(VERSION 3.5)

add_definitions(-DLV_LVGL_H_INCLUDE_SIMPLE)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# fwshrink1: at -Os GCC's snprintf length analysis sees more (inlining) and warns of truncations it did not see at
# -Og; snprintf's truncation is intended there (bounded screen and log lines). Warnings stay, not errors.
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=format-truncation" APPEND)
project(mcu_fw)

# The firmware builds at -Og (CONFIG_COMPILER_OPTIMIZATION_DEBUG); mbedTLS's
# bignum/ECC code at -Og made one HTTPS handshake to the Shiny Link device API
# take ~6 s on the 160 MHz ESP32. -O2 for the crypto libraries only.
foreach(_lib mbedcrypto mbedx509 mbedtls)
    if(TARGET ${_lib})
        target_compile_options(${_lib} PRIVATE -O2)
    endif()
endforeach()

# usb-cart (lane cartflash1): the staged cart install keeps the ROM and the save in this flash
# from 0x208000 on (components/nw/cartflash/cf_usb.h). The app image starts at 0x10000 and must
# end below that: the build fails if it does not (the firmware checks again when it runs).
if(CONFIG_NW_CARTFLASH)
    add_custom_target(cart_stage_room ALL
        COMMAND ${PYTHON} -c "import os,sys; n=os.path.getsize(sys.argv[1]); lim=0x208000-0x10000; print('cart staging area: the app image is %d bytes, the limit is %d (%d to spare)' % (n, lim, lim-n)); sys.exit(0 if n <= lim else 1)" ${CMAKE_BINARY_DIR}/mcu_fw.bin
        # ... and no cart command may need more stack than its task has (usb-cart-1 overflowed the console's 4 KB)
        COMMAND ${PYTHON} ${CMAKE_SOURCE_DIR}/components/nw/cartflash/host/check_stack.py ${CMAKE_BINARY_DIR}
        # ... and no raw flash erase / write may hit a region the IDF guards (usb-cart-2 aborted erasing the staged ROM)
        COMMAND ${PYTHON} ${CMAKE_SOURCE_DIR}/components/nw/cartflash/host/check_flash.py ${CMAKE_BINARY_DIR} ${CMAKE_SOURCE_DIR}
        DEPENDS gen_project_binary
        VERBATIM)
endif()
