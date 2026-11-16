# Mac test of cwl_sec.c (not part of the IDF build).
CFLAGS ?= -O2 -g -Wall -Wextra -Werror -std=c11
CWL = ..

sec_test: sec_test.c $(CWL)/cwl_sec.c $(CWL)/cwl_sec.h
	$(CC) $(CFLAGS) -DCWLSEC_HOST -I$(CWL) -o $@ sec_test.c $(CWL)/cwl_sec.c -lm

wdt_test: wdt_test.c $(CWL)/cwl_wdt.c $(CWL)/cwl_wdt.h
	$(CC) $(CFLAGS) -DCWLWDT_HOST -I$(CWL) -o $@ wdt_test.c $(CWL)/cwl_wdt.c

portal_test: portal_test.c $(CWL)/cwl_portal.c $(CWL)/cwl_portal.h
	$(CC) $(CFLAGS) -fsanitize=address,undefined -I$(CWL) -o $@ portal_test.c $(CWL)/cwl_portal.c

logic_test: logic_test.c $(CWL)/cwl_logic.h
	$(CC) $(CFLAGS) -I$(CWL) -o $@ logic_test.c

test: sec_test wdt_test portal_test logic_test
	./sec_test
	./wdt_test
	./portal_test
	./logic_test

.PHONY: test

# Host render of SYSTEM > WI-FI > SET UP WITH PHONE: the three screens (choose,
# QR, type it in) from the font file at wifi_net.c's positions over a device
# screenshot, the QR from wifi_qr.c itself (on LVGL's qrcodegen.c), and an
# OpenCV decode of the QR screen:
#   make -C components/cwl/host osd-shot PW=KX7RE-HC3PN-4FTJA
LVQR ?= ../../../managed_components/lvgl__lvgl/src/extra/libs/qrcode
OSDSYS = ../../osd/system
FONT = ../../fonts/fingfai.c
BG ?= ../../../../shots/ble1/phone2.png
PW ?= KX7RE-HC3PN-4FTJA
SSID ?= ShinyRelay-1A2B
qrhost: qrhost.c $(OSDSYS)/wifi_qr.c $(OSDSYS)/wifi_qr.h
	mkdir -p qrshim && sed 's#"../../../misc/lv_assert.h"#"lv_assert_shim.h"#' $(LVQR)/qrcodegen.c > qrshim/qrcodegen.c
	printf '#include <assert.h>\n#define LV_ASSERT(x) assert(x)\n' > qrshim/lv_assert_shim.h
	cp $(LVQR)/qrcodegen.h qrshim/
	$(CC) -O2 -Wall -DWIFIQR_HOST -Iqrshim -I$(OSDSYS) -o $@ qrhost.c $(OSDSYS)/wifi_qr.c qrshim/qrcodegen.c
osd-shot: qrhost
	python3 -I osd_phone.py $(FONT) $(BG) osd_wifi_list.png "! CHOOSE  A  NETWORK" "!>SET  UP  WITH  PHONE" " SAVED  NETWORKS" " SCAN  AGAIN" "# ||||           spiderplan*" "# |||.           NETGEAR42"
	python3 -I osd_phone.py $(FONT) $(BG) osd_wifi_list_scrolled.png "! CHOOSE  A  NETWORK" " SCAN  AGAIN" "# ||||           spiderplan*" "# |||.           NETGEAR42" " ||..           xfinitywifi" "#!>|...        DIRECT-7F"
	python3 -I osd_phone.py $(FONT) $(BG) osd_phone_choose.png " PHONE  SETUP" "!>SCAN  A  QR  CODE" " TYPE  IT  IN" "" " READY  FOR  A  PHONE" " A:  OK    B:  STOP  4:59"
	python3 -I osd_phone.py $(FONT) $(BG) osd_phone_choose2.png " PHONE  SETUP" " SCAN  A  QR  CODE" "!>TYPE  IT  IN" "" " TRYING  spiderplants" " A:  OK    B:  STOP  3:12"
	python3 -I osd_phone.py $(FONT) $(BG) osd_phone_type.png " ON  THE  PHONE,  JOIN" "! $(SSID)" " PASSWORD  (CAPS)" "! $(PW)" " THEN OPEN 192.168.4.1" " B:  BACK    4:59"
	./qrhost 62 "WIFI:T:WPA;S:$(SSID);P:$(PW);;" > qr62.txt
	python3 -I osd_phone.py $(FONT) $(BG) osd_phone_qr.png --qr qr62.txt 80 39 "" "" "" "" "" "" " B:  BACK    4:59"
	python3 -I qrdecode.py osd_phone_qr.png
.PHONY: osd-shot
