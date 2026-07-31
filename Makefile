# SPDX-License-Identifier: 0BSD

CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra
LIBUSB_CFLAGS := $(shell pkg-config --cflags libusb-1.0)
LIBUSB_LIBS := $(shell pkg-config --libs libusb-1.0)

.PHONY: all clean

all: build/egis057e_usb_probe build/egis057e_match_eval build/raw_to_pgm

build:
	mkdir -p build

build/egis057e_usb_probe: tools/egis057e_usb_probe.c | build
	$(CC) $(CFLAGS) $(LIBUSB_CFLAGS) -o $@ $< $(LIBUSB_LIBS)

build/egis057e_match_eval: tools/egis057e_match_eval.c | build
	$(CC) $(CFLAGS) -o $@ $< -lm

build/raw_to_pgm: tools/raw_to_pgm.c | build
	$(CC) $(CFLAGS) -o $@ $<

clean:
	$(RM) -r build
