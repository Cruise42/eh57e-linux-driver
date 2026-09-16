# SPDX-License-Identifier: 0BSD

CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra
LIBUSB_CFLAGS := $(shell pkg-config --cflags libusb-1.0)
LIBUSB_LIBS := $(shell pkg-config --libs libusb-1.0)
FPRINT_CFLAGS = $(shell pkg-config --cflags libfprint-2 gio-2.0)
FPRINT_LIBS = $(shell pkg-config --libs libfprint-2 gio-2.0)

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

# Optional: requires libfprint development headers and the EH57E build at runtime.
build/egis057e_verify_isolated: tools/egis057e_verify_isolated.c | build
	$(CC) $(CFLAGS) $(FPRINT_CFLAGS) -o $@ $< $(FPRINT_LIBS)

clean:
	$(RM) -r build
