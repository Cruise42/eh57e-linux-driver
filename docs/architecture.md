# Driver architecture

## Why the normal libfprint image path was insufficient

libfprint's conventional `FpImageDevice` flow extracts minutiae with NBIS and
matches them with Bozorth3. The EH57E image is only 70×57 pixels. It clearly
contains ridges, but often contains no ridge ending or bifurcation. Scaling the
image—nearest-neighbor or bilinear—does not create real minutiae. NBIS therefore
returns “No minutiae found.”

The integration patch adds two optional internal image-device callbacks:

```c
gboolean (*enroll_image)(FpImageDevice *, FpPrint *, FpImage *, GError **);
FpiMatchResult (*match_image)(FpImageDevice *, FpPrint *, FpImage *, GError **);
```

Drivers that do not set both callbacks retain the existing NBIS behavior.
EH57E sets both and stores `FPI_PRINT_RAW` data.

## Enrollment format

The print's private `fpi-data` value is a GVariant of type `aay`: an array of
byte arrays. Each child is one complete 3990-byte native image. The current
image-device superclass requests six stages on the tested libfprint release;
the effective public prototype may therefore collect the superclass default
even though five diverse images are sufficient for matching.

Templates are stored by fprintd. They are biometric data and must be protected
like credentials. Do not commit them to source control.

## Ridge matcher

For each enrolled image, the matcher searches translations `dx,dy` from -12
through +12 pixels. At each translation it compares a central crop, excluding
a four-pixel border.

Each pixel becomes a simple combined gradient:

```text
g(x,y) = I(x+1,y) - I(x-1,y) + I(x,y+1) - I(x,y-1)
```

The score is zero-mean normalized cross-correlation of these gradients. This
suppresses brightness changes and produces a score approximately in `[-1,1]`.

A single maximum is unsafe because unrelated parallel ridges can align. A
measured non-enrolled finger once scored about `0.47` against one enrollment
sample while scoring at most `0.20` against every other sample. The current
decision rule therefore requires:

```text
best score >= 0.34 AND second-best score >= 0.27
```

Those values are experimental, unit-specific, and not a security proof.

## Capture state machine

The driver uses one libfprint sequential state machine for initialization and
repeated capture:

```text
INIT_SEND -> INIT_RECV -> INIT_NEXT
    |
    v
CAPTURE_DELAY -> CAPTURE_SEND -> CAPTURE_RECV -> CAPTURE_NEXT
                                    |
                                    v
CAPTURE_REQUEST -> CAPTURE_IMAGE -> CAPTURE_FINISH
                                      |
                                      v
                               CAPTURE_FINISH_RECV
                                      |
                                      v
                                DETECT_DONE -> loop
```

`CAPTURE_DELAY` schedules a short timeout instead of blocking. Cancellation is
checked between frames. The calibrated image path is reused within one open
device session but rebuilt after the device is reopened.

## Automatic touch detection

The reader does not provide a usable touch bit in working image mode. The
driver captures preview frames every 250 ms and computes temporal activity.
Eight frames establish a per-activation baseline. Two consecutive frames over
baseline + 0.10 start a three-frame contact-settling period. The final settled
frame is delivered to the matcher.

If the initial eight-frame activity is substantially above the measured empty
sensor range, the driver treats the finger as already present and enters the
contact-settling period directly. This supports lock screens where the user can
touch the reader before PAM has finished activating it.

Every image-device activation runs the complete initialization sequence. The
short four-command recovery prefix resets command state but does not reliably
restore live-image updates after an identify-to-enroll transition; reapplying
normal image mode and the image-window registers does.

After every capture, including verify/identify, the driver retains the accepted
finger frame and keeps sampling until two consecutive frames differ
substantially from it. This matters because fprintd can chain an IDENTIFY
duplicate check directly into ENROLL: a synthetic finger-off would allow the
enrollment activation to calibrate while the finger is still present.
Comparing against the captured image, rather than merely looking for low
inter-frame activity, also distinguishes a held-but-stable finger from the
clear sensor after removal.
The minimum release difference is deliberately above the measured long-hold
drift (about 5.5 intensity units); a physical lift measured about 34.7 units.

## Core completion ordering

The custom image path mirrors the asynchronous NBIS completion ordering. It
sets the internal scan-active flag, moves to `AWAIT_FINGER_OFF`, clears the
flag inside custom processing, then reports progress or match results. Changing
that order can cause invalid transitions such as
`DEACTIVATING -> AWAIT_FINGER_OFF` at the final enrollment stage.
