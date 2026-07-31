# Safe testing procedure

## 1. Preserve password access

Before loading the driver:

- verify the account password;
- keep a root/recovery console available;
- do not remove existing authentication methods;
- back up PAM files before changing them.

## 2. Confirm hardware identity

```sh
lsusb -d 1c7a:057e
lsusb -v -d 1c7a:057e
```

Do not run the probe against a different PID merely because it is branded
EgisTec.

## 3. Test libfprint without PAM

```sh
fprintd-list "$USER"
fprintd-enroll
fprintd-verify
```

Enrollment should require removal and replacement between stages. Verification
should accept repeated natural placements of the enrolled finger.

## 4. Test rejection

Use at least three non-enrolled fingers. Repeat each at varied positions. Record
the best and second-best scores from a debug build. A single correct rejection
does not establish safety.

The original development sample showed why two-template agreement is needed:
an unrelated finger aligned with one template at roughly 0.47 but had a
second-best score near 0.20.

## 5. Test PAM cautiously

Use a terminal before testing the login manager:

```sh
sudo -k
sudo -v
```

Confirm password fallback works after a timeout or mismatch. Then test screen
unlock and graphical login. Desktop interfaces may continue showing a password
field while fingerprint verification is active.

## 6. Probe use

The direct libusb probe and fprintd cannot own the interface simultaneously.
Stop or release fprintd first. Some probe operations require root privileges.

Interactive probe rules:

- establish no-finger calibration only after the operator explicitly confirms
  the sensor is clear;
- arm capture before asking for a finger;
- do not assume a finger was removed;
- never publish the resulting raw file.

## 7. Useful recovery

If the reader stops responding:

1. stop fprintd;
2. close any probe process;
3. restart fprintd;
4. if necessary, reboot with the sensor untouched during startup.

Remove the systemd override to return fprintd to the distribution library.
