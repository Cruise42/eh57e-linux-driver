# Experimental binary package

The release archive contains a prebuilt replacement `libfprint-2.so.2` with the
EH57E driver enabled. It was built and tested on Linux Mint 22.3 x86-64 (Ubuntu
24.04 base, glibc 2.39). It is not a portable, distribution-independent driver.

The installer places the library in `/opt/egis057e-libfprint/lib` and adds a
systemd drop-in that changes `LD_LIBRARY_PATH` only for `fprintd`. It does not
overwrite the distribution library under `/usr/lib`.

After extracting a release archive:

```sh
sudo ./install.sh
fprintd-enroll
fprintd-verify
```

Keep password authentication enabled. To remove the package:

```sh
sudo ./uninstall.sh
```

The binary is derived from libfprint commit
`d79f157282085738ea8ffbe8c2ae96fb8b3ad831` plus the source and integration
patch published in this repository. libfprint-derived portions remain under
libfprint's LGPL license; see `NOTICE`.

There is no warranty that this build will work on another computer, Linux
distribution, libfprint/fprintd release, desktop, kernel, or EH57E firmware.
