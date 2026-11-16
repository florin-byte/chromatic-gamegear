# Mac builds of the ESP32's SHLINK side (lane chromshlink1). Not part of the
# IDF build (components/nw/CMakeLists.txt lists its sources).
CFLAGS ?= -O2 -g -Wall -Wextra -std=gnu11
NW = ..
TOOLS = ../../../../tools/nw
SAMEBOY ?= ../../../../third_party/SameBoy
INC = -I$(NW) -I$(TOOLS)
SRC = $(NW)/nw_esp.c $(NW)/nw_link.c $(NW)/shl_client.c $(TOOLS)/nw_fpga_model.c shl_dev.c

all: shl_test shl_sameboy

shl_test: shl_test.c $(SRC) $(NW)/*.h shl_dev.h
	$(CC) $(CFLAGS) $(INC) -o $@ shl_test.c $(SRC)

shl_sameboy: shl_sameboy.c $(SRC) $(NW)/*.h shl_dev.h
	$(CC) $(CFLAGS) $(INC) -I$(SAMEBOY)/build/include/sameboy -I$(SAMEBOY) \
	    -DBOOTROM_DIR=\"$(abspath $(SAMEBOY)/build/bin/BootROMs)\" \
	    -o $@ shl_sameboy.c $(SRC) $(SAMEBOY)/build/lib/libsameboy.a -lm

party_test: party_test.c $(NW)/party_ask.c $(NW)/party_ask.h $(NW)/shl_client.c $(NW)/shl_client.h
	$(CC) $(CFLAGS) -Werror -fsanitize=address,undefined -I$(NW) -o $@ party_test.c $(NW)/party_ask.c $(NW)/shl_client.c

test: shl_test shl_burst_sim party_test
	./shl_test
	./shl_burst_sim
	./party_test

.PHONY: all test

shl_echo: shl_echo.c $(SRC) $(NW)/*.h shl_dev.h
	$(CC) $(CFLAGS) $(INC) -o $@ shl_echo.c $(SRC)

shl_burst_sim: shl_burst_sim.c $(SRC) $(NW)/*.h shl_dev.h
	$(CC) $(CFLAGS) $(INC) -o $@ shl_burst_sim.c $(NW)/shl_client.c

sim: shl_burst_sim
	./shl_burst_sim
.PHONY: sim
