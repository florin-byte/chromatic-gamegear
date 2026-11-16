# Mac checks of SYSTEM > NETWORK's text (0.17.7). Not part of the IDF build.
#   make -C components/osd/system/host test    play_help.c is what the generator makes,
#                                             and every line on the page fits the panel
#   make -C components/osd/system/host shots   PNGs of the help, the question and the hints
FONT = ../../../fonts/fingfai.c
SHOT = ../../../cwl/host/osd_phone.py
BG ?= ../../../../../shots/ble1/phone2.png
OUT ?= shots

test:
	python3 -I make_play_help.py $(FONT) play_help_check.c > /dev/null
	cmp play_help_check.c ../play_help.c && echo "play_help.c: up to date"
	python3 -I check_lines.py $(FONT) ../network.c
	python3 -I check_lines.py $(FONT) ../download.c ../../../nw/cartflash/cf_install.c
	python3 -I check_lines.py $(FONT) ../play.c ../../../nw/cartflash/cf_rc_dev.c
	python3 -I check_words.py $(FONT) ../games.c ../play.c ../getgames.c ../../../nw/cartflash/cf_dev.c ../../../nw/cartflash/cf_rc_dev.c ../../../nw/cartflash/cf_sup.c
	$(MAKE) -s games_shots && ./games_shots > /dev/null
	$(MAKE) -s gg_shots && ./gg_shots > /dev/null
	$(MAKE) -s clock_shots && ./clock_shots > /dev/null
	python3 -I check_words.py $(FONT) ../timeset.c
	$(MAKE) -s custom_shots && ./custom_shots > custom_rows.txt && python3 -I share_fit.py $(FONT) < custom_rows.txt
	$(MAKE) -s fw_shots && ./fw_shots > fw_rows.txt && python3 -I fw_fit.py $(FONT) < fw_rows.txt

shots:
	mkdir -p $(OUT)
	python3 -I help_shots.py $(SHOT) $(FONT) $(BG) $(OUT) ../play_help.c
	python3 -I $(SHOT) $(FONT) $(BG) $(OUT)/public_match.png "!PUBLIC  MATCH" "YOUR  NAME  WILL" "BE  VISIBLE" "!A:  OK" "!SELECT:" "!GENERIC  NAME" "!B:  BACK"
	python3 -I $(SHOT) $(FONT) $(BG) $(OUT)/toggle_hint.png "B:  HOW  TO  PLAY"
	python3 -I $(SHOT) $(FONT) $(BG) $(OUT)/room_view.png "!NOT  IN  A  ROOM" "" "" "GAME  ROOM" "" "!A:  HOST  OR  JOIN" "B:  HOW  TO  PLAY"
	python3 -I $(SHOT) $(FONT) $(BG) $(OUT)/room_saving.png "!SAVING..." "" "" "GAME  ROOM" "" "!A:  HOST  OR  JOIN" "B:  HOW  TO  PLAY"
	python3 -I $(SHOT) $(FONT) $(BG) $(OUT)/room_failed.png "!NO  ROOM" "TRY  AGAIN  LATER" "" "GAME  ROOM" "" "!A:  HOST  OR  JOIN" "B:  HOW  TO  PLAY"

.PHONY: test shots

# SYSTEM > DOWNLOAD (wifirecv1): the real page over the real install machine, every screen as a PNG
DLOUT ?= shots_download
dl_shots: dl_shots.c ../download.c ../download.h ../../../nw/cartflash/cf_install.c ../../../nw/cartflash/cf_install.h
	$(CC) -O1 -g -Wall -Wextra -std=gnu11 -Istub -I../../../nw -I../../../nw/cartflash -o $@ dl_shots.c ../download.c ../share.c ../../../nw/cartflash/cf_install.c
dl-shots: dl_shots
	mkdir -p $(DLOUT)
	./dl_shots | while IFS='|' read -r n r0 r1 r2 r3 r4 r5 r6; do \
	    python3 -I $(SHOT) $(FONT) $(BG) $(DLOUT)/$$n.png "$$r0" "$$r1" "$$r2" "$$r3" "$$r4" "$$r5" "$$r6" > /dev/null || exit 1; echo $(DLOUT)/$$n.png; done
.PHONY: dl-shots

# SYSTEM > GAMES (lane ui110dev): the real page over mock places, every screen as a PNG, its rules checked
GOUT ?= shots_games
games_shots: games_shots.c ../games.c ../play.c ../games.h ../play.h games_share_stub.c ../share.h ../../../nw/cartflash/cf_sup.c ../../../nw/cartflash/cf_sup.h
	$(CC) -O1 -g -Wall -Wextra -std=gnu11 -DGAMES_HOST -Igstub -I../../../nw -I../../../nw/cartflash -I../../../sdcard -I.. -o $@ games_shots.c ../games.c ../play.c games_share_stub.c ../../../nw/cartflash/cf_sup.c
games-shots: games_shots
	mkdir -p $(GOUT)
	./games_shots | while IFS='|' read -r n t r0 r1 r2 r3 r4 r5 r6; do \
	    python3 -I osd_page.py $(FONT) $(GOUT)/$$n.png "$$t" "$$r0" "$$r1" "$$r2" "$$r3" "$$r4" "$$r5" "$$r6" | sed "s|^|$$n: |" | grep -v ' OK ' ; done; true
.PHONY: games-shots

# SYSTEM > GET GAMES (lane ui110dev): the real page over the real install machine (catalog mode), every screen as a PNG
QOUT ?= shots_getgames
gg_shots: gg_shots.c ../getgames.c ../getgames.h ../../../nw/cartflash/cf_install.c ../../../nw/cartflash/cf_install.h
	$(CC) -O1 -g -Wall -Wextra -std=gnu11 -DGAMES_HOST -Igstub -I../../../nw -I../../../nw/cartflash -I../../../sdcard -o $@ gg_shots.c ../getgames.c ../../../nw/cartflash/cf_install.c
gg-shots: gg_shots
	mkdir -p $(QOUT)
	./gg_shots | while IFS='|' read -r n t r0 r1 r2 r3 r4 r5 r6; do \
	    python3 -I osd_page.py $(FONT) $(QOUT)/$$n.png "$$t" "$$r0" "$$r1" "$$r2" "$$r3" "$$r4" "$$r5" "$$r6" | sed "s|^|$$n: |" | grep -v ' OK ' ; done; true
.PHONY: gg-shots

# lane share1: SHARE / RECEIVE FROM A CHROMATIC, every page's rows (share.c) checked against the panel and rendered
SHOUT ?= shots_share
share_shots: share_shots.c ../share.c ../share.h ../../../nw/cartflash/cf_share_dev.h ../../../nw/cartflash/cf_share.h
	$(CC) -O1 -g -Wall -Wextra -std=gnu11 -DSHARE_HOST_TEST -Istub -I.. -I../../../nw -I../../../nw/cartflash -o $@ share_shots.c ../share.c
share-fit: share_shots
	./share_shots | python3 -I share_fit.py $(FONT)
share-shots: share_shots
	mkdir -p $(SHOUT)
	./share_shots | while IFS='|' read -r n r0 r1 r2 r3 r4 r5 r6; do \
	    python3 -I $(SHOT) $(FONT) $(BG) $(SHOUT)/$$n.png "$$r0" "$$r1" "$$r2" "$$r3" "$$r4" "$$r5" "$$r6" > /dev/null || exit 1; echo $(SHOUT)/$$n.png; done
.PHONY: share-fit share-shots

# lane m110s: share.c's page side over a fake session (a result nobody dismissed; a share that could not start)
share_page_test: share_page_test.c ../share.c ../share.h ../../../nw/cartflash/cf_share_dev.h ../../../nw/cartflash/cf_share.h
	$(CC) -O1 -g -Wall -Wextra -std=gnu11 -DSHARE_HOST_TEST -Istub -I.. -I../../../nw -I../../../nw/cartflash -o $@ share_page_test.c ../share.c
share-page: share_page_test
	./share_page_test
.PHONY: share-page

# SYSTEM > CLOCK (1.1.7): the real page over a scripted clock, every screen as a PNG, its rules checked
COUT ?= shots_clock
clock_shots: clock_shots.c ../timeset.c ../timeset.h ../../../cwl/cwl_civil.h
	$(CC) -O1 -g -Wall -Wextra -std=gnu11 -Igstub -I../../../cwl -I.. -o $@ clock_shots.c ../timeset.c
clock-shots: clock_shots
	mkdir -p $(COUT)
	./clock_shots | while IFS='|' read -r n t r0 r1 r2 r3 r4 r5 r6; do \
	    python3 -I osd_page.py $(FONT) $(COUT)/$$n.png "$$t" "$$r0" "$$r1" "$$r2" "$$r3" "$$r4" "$$r5" "$$r6" | sed "s|^|$$n: |" | grep -v ' OK ' ; done; true
.PHONY: clock-shots

# SYSTEM > CUSTOM and SYSTEM > UPDATE (1.1.12): the real pages over the real update machine (cf_ota.c, both channels) with a
# scripted server and pairing: the rules checked (UPDATE official only, CUSTOM custom only), every row fits, every screen a PNG
CUOUT ?= shots_custom
custom_shots: custom_shots.c ../custom.c ../custom.h ../update.c ../update.h ../../../nw/cartflash/cf_ota.c ../../../nw/cartflash/cf_ota.h stub/cwl_online.h
	$(CC) -O1 -g -Wall -Wextra -std=gnu11 -Istub -I.. -I../../../nw -I../../../nw/cartflash -o $@ custom_shots.c ../custom.c ../update.c ../../../nw/cartflash/cf_ota.c
custom-shots: custom_shots
	mkdir -p $(CUOUT)
	./custom_shots | while IFS='|' read -r n r0 r1 r2 r3 r4 r5 r6; do \
	    python3 -I $(SHOT) $(FONT) $(BG) $(CUOUT)/$$n.png "$$r0" "$$r1" "$$r2" "$$r3" "$$r4" "$$r5" "$$r6" > /dev/null || exit 1; echo $(CUOUT)/$$n.png; done
.PHONY: custom-shots

# SYSTEM INFO (status/fw.c, 1.1.12): the real page with the longest release and custom versions; every character of
# every label is drawn (not cut by the details box, inside the panel, off the picture). fw-shots: every page a PNG
FW_C ?= ../../status/fw.c
FWOUT ?= shots_fw
FWBG ?= $(BG)
fw_shots: fw_shots.c $(FW_C) ../../status/fw.h fwstub/lvgl.h fwstub/osd_shared.h
	$(CC) -O1 -g -Wall -Wextra -std=gnu11 -DFW_HOST_TEST -Ifwstub -I../../status -o $@ fw_shots.c $(FW_C)
fw-shots: fw_shots
	mkdir -p $(FWOUT)
	./fw_shots > fw_rows.txt; python3 -I fw_fit.py $(FONT) ../../../images/img_chromatic.c $(FWBG) $(FWOUT) < fw_rows.txt
.PHONY: fw-shots
