# Reimplementation guide for an AI or engineer

This document is intended to let another capable coding agent reconstruct the
driver without access to the original development conversation or biometric
captures.

## Objective

Implement Linux/libfprint support for USB `1c7a:057e` with:

1. deterministic sensor recovery and image calibration;
2. 70×57 normal image acquisition;
3. automatic touch and release estimation;
4. raw multi-sample enrollment;
5. translation-tolerant small-area matching;
6. fprintd/PAM compatibility.

Do not assume this protocol applies to another EgisTec PID.

## Ground truths

- endpoints: bulk OUT 1, bulk IN `0x82`; interrupts `0x83/0x84` exist but are
  not useful in the working configuration;
- framing: requests start `EGIS`, command responses start `SIGE`;
- response byte 6 is normally the success byte;
- native image length is exactly 3990 bytes = 70×57;
- calibration read opcode is `0x72`;
- normal capture opcode is `0x64`;
- normal capture request bytes are `45 47 49 53 64 0f 96`;
- normal one-frame arm payload is command `0x2c`, data `00 13`;
- completion is register write `0x2d = 0x20`;
- NBIS is inappropriate because the physical image is usually minutiae-free.

## Recommended implementation order

### Phase 1: transport probe

Implement `read_u8`, `write_u8`, `read_buf`, `write_buf`, and raw exact-length
read helpers with libusb. Validate request and response lengths. Never treat a
seven-byte `SIGE` response as image data.

### Phase 2: image calibration

Implement the recovery and calibration sequence from `protocol.md`. Drain the
entire 3990-byte opcode-`0x72` calibration frame. Failure to drain it shifts all
subsequent command/response boundaries.

Improve the supplied snapshot by replacing the static `0x6d` calibration byte
with the first byte returned by `read_buf(0x67, 3)`.

### Phase 3: normal image capture

Set normal mode and window registers, arm command `0x2c`, request with opcode
`0x64`, read exactly 3990 bytes, and finish with register `0x2d = 0x20`.

Verify visually using a private PGM conversion. Correct data shows coherent
fingerprint ridges; incorrect state commonly yields constant/blank frames or a
short `SIGE` echo.

### Phase 4: libfprint state machine

Use asynchronous `FpiSsm` states; do not perform blocking USB loops inside a
callback. Every transfer should either advance, jump, complete, or fail the
state machine exactly once.

Initialization should call `fpi_image_device_activate_complete()` once. On
cancellation, finish the current safe boundary, complete the state machine,
and call `fpi_image_device_deactivate_complete()` once.

### Phase 5: custom image matching

Either use the included libfprint core hooks or redesign them upstream. The
essential requirement is to bypass NBIS while preserving normal fprintd
enroll/verify/identify semantics.

Represent a template as `aay`, one 3990-byte child per enrollment stage. Use
`FPI_PRINT_RAW`. Make sure the print type is selected before the first image is
appended; repeatedly calling `fpi_print_set_type()` violates its assertion.

For each template image:

1. search translations -12..+12 in both axes;
2. exclude a four-pixel border;
3. compute the combined horizontal/vertical central-difference gradient;
4. compute zero-mean normalized cross-correlation;
5. retain best and second-best scores across enrollment images.

The snapshot accepts only if best ≥ 0.34 and second-best ≥ 0.27. Do not weaken
to best-only matching; that produced a measured false accept.

### Phase 6: touch detection

Do not run the recovered detect/gain calibration after working image
calibration: it destroyed normal images on the tested device. `reg01` stays zero
in working image mode.

Instead:

1. capture previews every 250 ms;
2. store the preceding frame;
3. compute mean absolute pixel difference;
4. discard eight settling comparisons and average them;
5. threshold at baseline + 0.10;
6. slowly adapt baseline on below-threshold samples;
7. require two consecutive above-threshold comparisons;
8. wait three more frames for full contact;
9. submit the final frame to matching;
10. for enrollment, require three consecutive below-threshold comparisons
    before reporting finger-off.

The activation must begin while the sensor is clear. If a finger is already
present during baseline acquisition, removal can resemble placement. A future
driver should solve this with a non-destructive hardware detect configuration.

### Phase 7: completion ordering

For custom matching, preserve the order used by asynchronous minutiae
processing:

1. mark scan processing active;
2. move image state to `AWAIT_FINGER_OFF`;
3. clear processing-active inside custom processing;
4. report enrollment progress or verify/identify result;
5. request final deactivation only after the state change.

The wrong order caused final enrollment to enter an invalid state transition
and never save the print.

### Phase 8: integration testing

Run libfprint unit tests, then `fprintd-enroll` and `fprintd-verify`. Test several
genuine and non-enrolled fingers before PAM. Test PAM in a terminal with a
password fallback before graphical login or screen lock.

## Empirical observations useful for debugging

- Consecutive stable finger frames differed by roughly 1.8 grayscale levels
  per pixel on the development sensor.
- Clear-state preview activity varied by calibration session (approximately
  1.4 to 1.7), requiring an adaptive baseline.
- Initial touch transitions produced mean differences in the tens; matching
  those transitional frames caused false rejects.
- A stable genuine scan had examples such as best/second `0.41/0.36` and
  `0.55/0.29`.
- A problematic non-enrolled finger had best/second `0.47/0.20`.
- The image-device superclass advertised six enrollment stages in the tested
  release even when the subclass attempted to set five. Account for the actual
  runtime value rather than assuming the class assignment won.

## Common failed approaches

- Waiting 30 seconds on interrupt endpoints without enabling vendor detect
  mode: no event.
- Running dynamic finger-detect calibration after image calibration: normal
  image buffer became blank/unusable.
- Reading calibration data with opcode `0x64`: wrong phase/opcode.
- Using opcode `0x72` for normal live images: wrong virtual method.
- Upscaling 70×57 images before NBIS: ridges became larger, but no real
  minutiae appeared.
- Best-template-only ridge correlation: false accept from periodic parallel
  ridges.
- Fixed temporal activity threshold: clear noise changed between calibration
  sessions.
- Capturing immediately at the first large touch transition: incomplete image
  and false reject.

## Privacy rules for future agents

- Never commit `.raw`, `.pgm`, `.png`, `.pcap`, `.pcapng`, fprintd storage, or
  journal excerpts containing personal identifiers.
- Treat every sensor image and template as biometric data.
- Use placeholders for usernames, hostnames, email addresses, and absolute
  home directories.
- Do not redistribute proprietary vendor DLLs; document hashes/versions only if
  legally appropriate.
- Before publishing, scan the entire export for credentials and private paths.

## Suggested future improvements

1. Dynamically propagate the calibration sample.
2. Find a working hardware touch mode that preserves image calibration.
3. Replace the simple gradient correlation with a tested small-area fingerprint
   descriptor supporting rotation and deformation.
4. Add unit tests with synthetic/non-biometric fixtures.
5. Turn the core callbacks into a reviewed upstream libfprint abstraction.
6. Add runtime model/revision detection. Command responses are validated for
   exact length, `SIGE` magic, and success status, but broader revision-specific
   validation remains future work.
