# Mac checks of the card library's portable rules (lane sdcard1). Not part of the IDF build.
#   make -C components/sdcard/host test
CFLAGS = -O1 -g -Wall -Wextra -std=gnu11
sd_core_test: sd_core_test.c ../sd_core.c ../sd_core.h
	$(CC) $(CFLAGS) -o $@ sd_core_test.c ../sd_core.c
FF = ../../fatfs/src
fs_test: fs_test.c host_card.c ../sd_lib.c ../sd_file.c ../sd_core.c $(FF)/ff.c $(FF)/ffunicode.c ../sd.h
	$(CC) $(CFLAGS) -Wno-unused-parameter -Wno-sign-compare -Istub -I.. -I$(FF) -o $@ fs_test.c host_card.c ../sd_lib.c ../sd_file.c ../sd_core.c $(FF)/ff.c $(FF)/ffunicode.c
test: sd_core_test fs_test
	./fs_test
	./sd_core_test
	python3 -I check_sd_lines.py ../../fonts/fingfai.c ../../osd/system/games.c ../sd.c ../sd_card.c ../../nw/cartflash/cf_rc_dev.c
.PHONY: test

# the FAT32 card image the library made, read by macOS (attached read-only, listed, detached)
mac-check: fs_test
	./fs_test /private/tmp/claude-501/sd_fs_fat32.img /private/tmp/claude-501/sd_fs_exfat.img
	sh ./mac_check.sh /private/tmp/claude-501/sd_fs_fat32.img
	sh ./mac_check.sh /private/tmp/claude-501/sd_fs_exfat.img
.PHONY: mac-check
