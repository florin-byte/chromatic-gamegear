# Mac test of the button poke masks (0.17.6). Not part of the IDF build.
CFLAGS ?= -O1 -g -Wall -Wextra -Werror -std=c11

poke_test: poke_test.c ../button_poke.h ../../osd/osd_default.c ../../cwl/cwl_esp.c
	$(CC) $(CFLAGS) -o $@ poke_test.c

routing_test: routing_test.c ../button.c ../button.h stub/host.h
	$(CC) $(CFLAGS) -Istub -I.. -I../../mutex -o $@ routing_test.c

test: poke_test routing_test
	./poke_test
	./routing_test

.PHONY: test
