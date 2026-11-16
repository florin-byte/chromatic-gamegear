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
	python3 -I check_words.py $(FONT) ../games.c ../play.c ../getgames.c ../../../nw/cartflash/cf_dev.c ../../../nw/cartflash/cf_rc_dev.c
	$(MAKE) -s games_shots && ./games_shots > /dev/null
	$(MAKE) -s gg_shots && ./gg_shots > /dev/null

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
games_shots: games_shots.c ../games.c ../play.c ../games.h ../play.h games_share_stub.c ../share.h
	$(CC) -O1 -g -Wall -Wextra -std=gnu11 -DGAMES_HOST -Igstub -I../../../nw -I../../../sdcard -I.. -o $@ games_shots.c ../games.c ../play.c games_share_stub.c
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
