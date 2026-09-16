# Safe testing procedure

## Isolated verification following the observed false acceptance

Do not use lock-screen or sudo unlocks to test the current matcher. Keep
fprintd stopped and masked while exercising libfprint directly. The optional
`tools/egis057e_verify_isolated.c` reads an existing serialized template and
reports decisions without PAM, template writes, image files, or session access.

Build with `make build/egis057e_verify_isolated` using libfprint development
headers; `FPRINT_CFLAGS` and `FPRINT_LIBS` can select a development build.
Run with the EH57E library explicitly selected:

```sh
sudo env LD_LIBRARY_PATH=/path/to/test-build/libfprint \
  ./build/egis057e_verify_isolated /path/to/protected/template 3
```

The optional count is 1–3. Each trial has a 60-second limit. Exit status reports
test execution, not authentication success. Inspect the printed MATCH/NO MATCH
or RETRY result. It cancels each verification after the early result to exercise
the same restart lifecycle as fprintd. A held contact should block the next
decision until the finger is removed. Cancellation and hardware errors are
reported separately from decisions. Do not publish template files or raw scans.

## 1. Preserve password access

### September 2026 isolated retry regression results

With thresholds restored to 0.34/0.27, a three-trial run on one open device
produced these best/second scores:

| Trial | Finger presented | Scores | Decision |
| --- | --- | --- | --- |
| 1 | Unenrolled | 0.1194 / 0.0903 | No match |
| 2, after removal | Same unenrolled finger | 0.1653 / 0.1091 | No match |
| 3, after removal | Enrolled finger | 0.5225 / 0.3415 | Match |

Each restart waited for removal while the previous contact was held. Removal
was detected and an empty-sensor baseline collected before the next placement.
No runtime calibration command was repeated between these trials. A separate
unenrolled placement scored 0.2195/0.1632 and was rejected. All of these
unenrolled results also fall below the former lower thresholds, so these tests
validate retry behavior but do not establish an acceptable false-accept rate.
System fingerprint authentication remained disabled throughout testing.

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

## 8. Diagnosing detection versus matching

Enable verbose fprintd/libfprint logging only for a short reproduction. A
runtime systemd drop-in avoids making diagnostics persistent:

```ini
# /run/systemd/system/fprintd.service.d/eh57e-debug.conf
[Service]
Environment=G_MESSAGES_DEBUG=all
```

Reload systemd, restart fprintd, reproduce once, and inspect the fprintd unit
journal. Relevant messages include baseline activity, initial-contact
detection, contact transitions, release differences, matcher scores, and the
final verify result.

- No verification start indicates a desktop or PAM integration problem.
- No contact transition indicates touch-detection or sensor-state trouble.
- A captured image followed by `verify-no-match` indicates matcher/placement
  sensitivity rather than a failure to recognize the touch.
- A first action that works followed by unchanged frames suggests the sensor's
  image mode was not fully restored on reactivation.

Remove the runtime drop-in, reload systemd, and restart fprintd immediately
after the test. Diagnostic logging must never include raw fingerprint frames or
stored templates.

For interactive testing, explicitly prompt the operator to place or remove the
finger and wait for an `OK` response at every step. This keeps human timing
separate from driver timing and makes journal evidence interpretable.
