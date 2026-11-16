# Two-Chromatic link simulator (host only, not part of the IDF build).
#   make -C components/cwl/host/linksim          (needs the SameBoy build in ../chromatic_wifi/third_party)
# ROMs and save states stay outside the repo (see run_matrix.sh).
SAMEBOY ?= ../../../../../third_party/SameBoy
CWL     = ../..
CFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
CPPFLAGS += -I$(SAMEBOY)/build/include/sameboy -I$(SAMEBOY) -I$(CWL) -DBOOTROM_DIR=\"$(abspath $(SAMEBOY)/build/bin/BootROMs)\"

linksim: linksim.c $(CWL)/cwl_proto.c $(CWL)/cwl_proto.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ linksim.c $(CWL)/cwl_proto.c $(SAMEBOY)/build/lib/libsameboy.a -lm

