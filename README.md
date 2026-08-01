# Experimental Linux driver for EgisTec EH57E (`1c7a:057e`)

This repository contains an independently reverse-engineered, experimental
libfprint driver for the EgisTec/LighTuning EH57E fingerprint reader with USB
ID `1c7a:057e`. This reader is found in at least some Samsung Galaxy Book Pro
360 systems and may also appear in unrelated hardware.

The prototype can calibrate the sensor, detect a touch, capture its native
70×57 grayscale image, enroll several samples, store a libfprint template, and
verify that template through fprintd and PAM. Enrollment and authentication
were demonstrated on one development machine, including desktop UI enrollment,
`sudo`, and Cinnamon lock-screen unlock.

## Important warning

There is **no warranty that this code will work on any other computer, sensor
revision, Linux distribution, libfprint release, or desktop environment**.
It may fail, lock the reader, prevent authentication, generate false accepts or
false rejects, or require a reboot. It is not security-audited and should not
be the only way to access important data. Keep a tested password and recovery
method available.

The matcher and automatic touch detector are empirical and tuned from a small
sample set on one EH57E unit. Treat this as research-quality software.

## Development environment

The driver and experimental binary package were developed and tested on this
platform:

- System: Samsung Galaxy Book 360 Pro (`950QDB`, version `P12AKG`)
- Operating system: Linux Mint 22.3 "Zena" (Ubuntu 24.04 Noble base)
- Kernel: Linux `6.8.0-136-generic`, x86-64
- Processor: 11th Gen Intel Core i7-1165G7 (4 cores, 8 threads, up to 4.7 GHz)
- Memory: 15 GiB RAM with 2 GiB swap
- Graphics: integrated Intel Iris Xe
- Storage: 1 TB WD Black SN850X NVMe SSD
- Wireless: Intel AX210 Wi-Fi 6E and Bluetooth
- USB controllers: Intel USB 3.2 and Thunderbolt 4
- Fingerprint reader: LighTuning/EgisTec EH57E (`1c7a:057e`), integrated with
  the power button
- Other relevant USB devices: Realtek RTS5129 card reader (`0bda:0129`) and a
  generic 720p HD camera
- Driver build baseline: libfprint 1.94.10 and glibc 2.39

The published binary is specific to this tested x86-64 environment. Source
compatibility and binary compatibility with other systems are not guaranteed.

## Repository layout

```text
src/
  egis057e.c                 libfprint driver
  egis057e.h                 protocol constants and image geometry
patches/
  libfprint-integration.patch
                              small-area matcher hooks and Meson registration
tools/
  egis057e_usb_probe.c       direct libusb protocol probe
  egis057e_match_eval.c      offline ridge-correlation evaluator
  raw_to_pgm.c               diagnostic raw-frame converter
docs/
  protocol.md                commands and confirmed state sequences
  architecture.md            driver and matcher design
  testing.md                 safe validation procedure
  security-and-limitations.md
  reimplementation-for-ai.md detailed handoff for another AI/engineer
examples/
  systemd/                   local fprintd library override
  pam/                       optional PAM example
```

Biometric images, enrolled templates, USB packet captures, vendor binaries,
hostnames, usernames, personal paths, and credentials are intentionally absent.

## Hardware identification

Confirm the USB ID before doing anything:

```sh
lsusb -d 1c7a:057e
```

Expected endpoints:

- bulk OUT `0x01`
- bulk IN `0x82`
- interrupt IN `0x83`
- interrupt IN `0x84`

The working implementation uses the bulk endpoints. Merely waiting on either
interrupt endpoint did not produce touch events on the tested unit.

## Building the probe tools

On Debian/Ubuntu-family systems, install a C compiler and libusb development
files, then run:

```sh
cc -O2 -Wall -Wextra -o egis057e_usb_probe \
  tools/egis057e_usb_probe.c $(pkg-config --cflags --libs libusb-1.0)

cc -O2 -Wall -Wextra -o egis057e_match_eval \
  tools/egis057e_match_eval.c -lm

cc -O2 -Wall -Wextra -o raw_to_pgm tools/raw_to_pgm.c
```

The direct probe detaches/claims a USB interface and can disrupt fprintd.
Read `docs/testing.md` before using it. Never publish captured fingerprint
frames without the subject's informed consent.

## Integrating with libfprint

This snapshot was developed against libfprint 1.94.x. Exact source compatibility
with later versions is not guaranteed.

From a clean libfprint checkout:

```sh
cp /path/to/this-project/src/egis057e.[ch] libfprint/drivers/
git apply /path/to/this-project/patches/libfprint-integration.patch

meson setup build-egis057e -Ddrivers=egis057e
ninja -C build-egis057e
meson test -C build-egis057e --print-errorlogs
```

If an existing build directory is used, reconfigure it instead of running a
second initial setup. The patch adds two optional internal `FpImageDeviceClass`
callbacks for sensors that cannot use NBIS minutiae matching. It also registers
the new driver in Meson.

For a reversible local deployment, install the built shared library under a
private prefix and point only fprintd at it. See
`examples/systemd/egis057e-local-libfprint.conf`. Do not overwrite the
distribution copy in `/usr/lib`.

Example layout:

```sh
sudo install -d /opt/egis057e-libfprint/lib
sudo install -m 0755 \
  build-egis057e/libfprint/libfprint-2.so.2.0.0 \
  /opt/egis057e-libfprint/lib/libfprint-2.so.2.0.0

sudo install -d /etc/systemd/system/fprintd.service.d
sudo install -m 0644 \
  examples/systemd/egis057e-local-libfprint.conf \
  /etc/systemd/system/fprintd.service.d/egis057e-local-libfprint.conf
sudo systemctl daemon-reload
sudo systemctl restart fprintd.service
```

Adjust the library filename if the libfprint ABI version differs.

## Enrollment and verification

Start with command-line tools and retain password access:

```sh
fprintd-enroll
fprintd-verify
```

Only after those work should fingerprint PAM authentication be enabled. PAM
configuration errors can lock users out. The example in `examples/pam/` is a
snippet, not a complete replacement for a distribution's PAM stack.

## How it works

The sensor returns exactly `0x0f96` bytes per image, equal to `70 × 57`. That
area is too small to reliably contain the ridge endings and bifurcations NBIS
expects. The driver therefore stores five raw enrollment images and performs
translation-tolerant normalized correlation over image gradients.

To avoid a one-template false accept, verification requires agreement with at
least two enrollment images: the best score must be at least `0.34` and the
second-best at least `0.27` in this snapshot.

Working image mode does not expose the vendor `reg01` finger bit. The driver
therefore estimates touch from temporal frame activity. It learns a clear-state
activity baseline on each activation, waits for two above-threshold frames,
allows three more frames for contact to settle, and then matches the image.

See the documents under `docs/` for the full protocol and rationale.

## Licensing

Original driver and utility code is released under the Zero-Clause BSD license
(`0BSD`), allowing use, copying, modification, and redistribution with or
without attribution. The libfprint integration patch modifies LGPL-covered
libfprint code and remains subject to libfprint's applicable license. See
`LICENSE` and `NOTICE`.
