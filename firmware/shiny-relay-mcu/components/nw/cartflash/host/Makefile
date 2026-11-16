# Mac simulation of the cart flash work (lane cartflash1). Not part of the IDF build.
#   make            the SM83 programs + boot ROM (../sm83/make_sm83.sh) and cf_sim
#   make test       every scenario (run_tests.sh: starts a local cart_probe_rx.py for the probe)
CFLAGS ?= -O2 -g -Wall -Wextra -std=gnu11
CF = ..
NW = ../..
TOOLS = ../../../../../tools/nw
SAMEBOY ?= ../../../../../third_party/SameBoy
INC = -I$(CF) -I$(NW) -I$(TOOLS) -I$(SAMEBOY)/build/include/sameboy -I$(SAMEBOY)
SRC = cf_sim.c cf_cart.c $(CF)/cf_esp.c $(CF)/cf_sha256.c $(CF)/cf_usb.c $(CF)/cf_chips.c $(NW)/nw_esp.c $(TOOLS)/nw_fpga_model.c

all: sm83 cf_sim cf_fetch_test cf_install_test cf_ctr_test cf_wire_check cf_bps_test cf_ota_test cf_fota_test cf_sgz_test cf_sgz_test_unit cf_lib_test cf_rc_win_test cf_keep_test cf_pack_test cf_share_test cf_hdr_test cf_pad_test cf_zc_test cf_sleep_test cf_sup_test

sm83:
	sh ../sm83/make_sm83.sh

cf_sim: $(SRC) cf_xs_model.inc cf_cart.h $(CF)/*.h $(NW)/nw_esp.h ../sm83/build/netboot_sameboy.bin
	$(CC) $(CFLAGS) -DNW_CARTFLASH $(INC) -o $@ $(SRC) $(SAMEBOY)/build/lib/libsameboy.a -lm

cf_fetch_test: cf_fetch_test.c $(CF)/cf_fetch.c $(CF)/cf_fetch.h $(CF)/cf_esp.c
	$(CC) $(CFLAGS) -DNW_CARTFLASH $(INC) -o $@ cf_fetch_test.c $(CF)/cf_fetch.c $(CF)/cf_esp.c $(CF)/cf_sha256.c $(CF)/cf_chips.c $(NW)/nw_esp.c

# 1.0.5: the PARTY BATTLE patch applied on the unit (cf_bps.c); the real files: see host_tests.sh
cf_bps_test: cf_bps_test.c $(CF)/cf_bps.c $(CF)/cf_bps.h $(CF)/cf_sha256.c
	$(CC) $(CFLAGS) $(INC) -o $@ cf_bps_test.c $(CF)/cf_bps.c $(CF)/cf_sha256.c

# 1.1.0: the FPGA boot flash's update layout (cf_fota.c)
cf_fota_test: cf_fota_test.c $(CF)/cf_fota.c $(CF)/cf_fota.h
	$(CC) $(CFLAGS) $(INC) -o $@ cf_fota_test.c $(CF)/cf_fota.c

# 1.1.0: the update over Wi-Fi (cf_ota.c)
cf_ota_test: cf_ota_test.c $(CF)/cf_ota.c $(CF)/cf_ota.h
	$(CC) $(CFLAGS) $(INC) -o $@ cf_ota_test.c $(CF)/cf_ota.c

cf_install_test: cf_install_test.c $(CF)/cf_install.c $(CF)/cf_install.h
	$(CC) $(CFLAGS) $(INC) -o $@ cf_install_test.c $(CF)/cf_install.c

OPENSSL ?= /opt/homebrew/opt/openssl@3
cf_ctr_test: cf_ctr_test.c $(CF)/cf_install.c $(CF)/cf_install.h
	$(CC) $(CFLAGS) $(INC) -I$(OPENSSL)/include -o $@ cf_ctr_test.c $(CF)/cf_install.c -L$(OPENSSL)/lib -lcrypto

# wifirecv1: the real server's receive-mode answers through the device's parser and CTR (recv_e2e.py)
cf_wire_check: cf_wire_check.c $(CF)/cf_install.c $(CF)/cf_install.h $(CF)/cf_sha256.c
	$(CC) $(CFLAGS) $(INC) -I$(OPENSSL)/include -o $@ cf_wire_check.c $(CF)/cf_install.c $(CF)/cf_sha256.c -L$(OPENSSL)/lib -lcrypto

# lane gamecomp1: SGZ1 unpack (cf_sgz.c + cf_inflate.c) on the golden vectors; cf_inflate against the Mac's zlib on
# random streams of every kind (stored, fixed, dynamic, every level and strategy) and broken ones
SGZ = cf_sgz_test.c $(CF)/cf_sgz.c $(CF)/cf_inflate.c $(CF)/cf_sha256.c $(CF)/cf_esp.c $(CF)/cf_chips.c $(NW)/nw_esp.c
cf_sgz_test: $(SGZ) $(CF)/cf_sgz.h $(CF)/cf_inflate.h
	$(CC) $(CFLAGS) -DNW_CARTFLASH -DCF_SGZ_HOST_TEST $(INC) -o $@ $(SGZ) -lz
cf_sgz_test_unit: $(SGZ) $(CF)/cf_sgz.h $(CF)/cf_inflate.h
	$(CC) $(CFLAGS) -DNW_CARTFLASH -DCF_SGZ_SHIFT_MAX=13u -DCF_SGZ_HOST_TEST $(INC) -o $@ $(SGZ) -lz

# lane gamecomp1: the game library on the FPGA flash (cf_lib.c) on a NOR model with power cuts
# lane ui110dev: the unit's SGZ1 packer, its containers unpacked by cf_sgz.c and by zlib
PACK = cf_pack_test.c $(CF)/cf_pack.c $(CF)/cf_sgz.c $(CF)/cf_inflate.c $(CF)/cf_sha256.c $(CF)/cf_esp.c $(CF)/cf_chips.c $(NW)/nw_esp.c
cf_pack_test: $(PACK) $(CF)/cf_pack.h $(CF)/cf_sgz.h
	$(CC) $(CFLAGS) -DNW_CARTFLASH -DCF_SGZ_HOST_TEST $(INC) -o $@ $(PACK) -lz
cf_lib_test: cf_lib_test.c $(CF)/cf_lib.c $(CF)/cf_lib.h $(CF)/cf_esp.c
	$(CC) $(CFLAGS) -DNW_CARTFLASH $(INC) -o $@ cf_lib_test.c $(CF)/cf_lib.c $(CF)/cf_esp.c $(CF)/cf_sha256.c $(CF)/cf_chips.c $(NW)/nw_esp.c

cf_keep_test: cf_keep_test.c $(CF)/cf_rc_keep.inc
	$(CC) $(CFLAGS) $(INC) -o $@ cf_keep_test.c

# lane savefix1: the light-sleep timer wake while a game plays from RAM, and the pending-save keep-awake
cf_sleep_test: cf_sleep_test.c $(CF)/cf_rc_sleep.h ../../../../main/pwrmgr_logic.h
	$(CC) $(CFLAGS) $(SLEEP_INC) $(INC) -I../../../../main -o $@ cf_sleep_test.c

# lane savecloud1: SAVE TO MY GAMES's decisions (cf_sup.c) against a mock flash and a mock server
cf_sup_test: cf_sup_test.c $(CF)/cf_sup.c $(CF)/cf_sup.h
	$(CC) $(CFLAGS) $(INC) -o $@ cf_sup_test.c $(CF)/cf_sup.c -lz

cf_hdr_test: cf_hdr_test.c $(CF)/cf_rc_hdr.inc
	$(CC) $(CFLAGS) $(INC) -o $@ cf_hdr_test.c

# lane m110t: a trimmed game (shorter than its header) plays: the ROM area's fill (cf_rc_pad.inc) and the size rule (sd_core.c)
cf_pad_test: cf_pad_test.c $(CF)/cf_rc_pad.inc ../../../sdcard/sd_core.c ../../../sdcard/sd_core.h
	$(CC) $(CFLAGS) $(INC) -o $@ cf_pad_test.c ../../../sdcard/sd_core.c

# lane m110t: GET GAMES -> CARD gives up on a card that stops taking writes (cf_zc_guard.inc)
cf_zc_test: cf_zc_test.c $(CF)/cf_zc_guard.inc
	$(CC) $(CFLAGS) $(INC) -o $@ cf_zc_test.c

cf_rc_win_test: cf_rc_win_test.c $(CF)/cf_rc_win.inc $(CF)/cf_rc_dev.h $(CF)/cf_proto.h
	$(CC) $(CFLAGS) -DNW_CARTFLASH $(INC) -o $@ cf_rc_win_test.c

test: all
	sh ./run_tests.sh

.PHONY: all sm83 test

# lane share1: Chromatic to Chromatic sharing, the protocol (cf_share.c) over a simulated radio (loss, cuts, restarts)
cf_share_test: cf_share_test.c $(CF)/cf_share.c $(CF)/cf_share.h $(CF)/cf_esp.c
	$(CC) $(CFLAGS) -DNW_CARTFLASH $(INC) -o $@ cf_share_test.c $(CF)/cf_share.c $(CF)/cf_esp.c $(CF)/cf_sha256.c $(CF)/cf_chips.c $(NW)/nw_esp.c
