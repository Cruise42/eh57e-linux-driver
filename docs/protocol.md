# EH57E protocol notes

## Scope

These notes describe behavior confirmed on one `USB\VID_1C7A&PID_057E`
device. Values may differ between silicon or firmware revisions.

## Transport and framing

Commands are sent to bulk OUT endpoint `0x01`. Replies and image data arrive
on bulk IN endpoint `0x82`.

Command headers begin with ASCII `EGIS` (`45 47 49 53`). Normal command
responses begin with reversed ASCII `SIGE` (`53 49 47 45`). For command
responses, byte 6 is normally `01` on success.

Confirmed command classes:

| Opcode | Meaning | Request form |
|---|---|---|
| `0x60` | Read an 8-bit register | `EGIS 60 <reg> 00` |
| `0x61` | Write an 8-bit register | `EGIS 61 <reg> <value>` |
| `0x62` | Read a buffer | `EGIS 62 <command> <length>` |
| `0x63` | Write a buffer | `EGIS 63 <command> <length> <payload...>` |
| `0x64` | Read a normal image | `EGIS 64 0f 96` |
| `0x71` | Bidirectional buffer transfer | command-specific |
| `0x72` | Read a calibration image | `EGIS 72 0f 96` |

Buffered commands generally return a response as long as the request and echo
their payload. Scalar commands normally return seven bytes.

## Confirmed image geometry

- payload length: `0x0f96` = 3990 bytes
- geometry: 70 columns × 57 rows
- pixel format: one unsigned grayscale byte per pixel
- order: row-major

Normal capture must use opcode `0x64` and the exact length. Opcode `0x72` is
used during calibration and is not interchangeable with normal capture.

## Recovery and image calibration

The driver sends this recovery prefix on every activation. When calibration is
still valid, it reuses that calibration only after the recovery prefix succeeds:

1. `write_u8(0x0a, 0xf4)`
2. `write_u8(0x0c, 0x44)`
3. `write_u8(0x40, 0x00)`
4. transfer command `0x02` with payload `01 0c`

The confirmed image calibration phase is:

1. `write_buf(0x09, 83 24 00 44 0f 08 20 20 00 00 52)`
2. `write_buf(0x2c, 00 57)`
3. poll register `0x2d` until its busy bit clears
4. `read_buf(0x67, 3)`; the first returned byte is the calibration sample
5. `write_buf(0x33, <sample> 10 01)`
6. poll register `0x35`
7. write power/recovery values to registers `0x0a`, `0x0c`, and `0x50`
8. poll `0x50`
9. `write_buf(0x54, 01 22 1c)`
10. request and drain one 3990-byte image using opcode `0x72`
11. `write_u8(0x54, 0x00)`

The development snapshot currently uses the empirically stable calibration
sample `0x6d` in its static initialization table. A production-quality version
should propagate the runtime byte returned by step 4.

## Normal image mode

After calibration:

1. `write_buf(0x09, 83 24 00 44 0f 08 20 20 01 05 12)`
2. `write_buf(0x26, 0e 36 04 0a 2e 04)`
3. `write_u8(0x23, 0x00)`
4. `write_u8(0x24, 0x38)`
5. `write_u8(0x20, 0x00)`
6. `write_u8(0x21, 0x45)`

Capture one frame:

1. `write_buf(0x2c, 00 13)`
2. read register `0x00` (the tested device reports ready immediately)
3. send `EGIS 64 0f 96`
4. read exactly 3990 bytes from endpoint `0x82`
5. `write_u8(0x2d, 0x20)`

For a three-frame burst, the arm payload is `02 13`, meaning
`{ frame_count - 1, 0x13 }`, followed by three normal image reads.

## Finger detection findings

The vendor's separate detect-mode initialization can make bit 2 of register
`0x01` change from `0x00` to `0x04` when touched. Unfortunately, running the
dynamic detect/gain calibration after image calibration blanked or destroyed
the working normal-image path on the tested device.

In the working image mode, register `0x01` remained `0x00` before and after a
touch. Interrupt endpoints `0x83` and `0x84` did not produce useful events
without the incompatible detect-mode sequence.

The current driver consequently uses temporal image activity:

- acquire and discard initial settling frames;
- calculate mean absolute difference from the preceding frame;
- learn an activation-specific clear baseline;
- threshold at baseline + `0.10`;
- require two above-threshold frames;
- wait three additional frames before matching;
- after every captured action, retain the accepted finger frame and require two frames
  that differ substantially from it before reporting finger removal; low
  inter-frame activity alone cannot distinguish a held finger from an empty
  sensor.

This is empirical and must be retuned or replaced if another unit behaves
differently.

## Important negative result

A previously investigated `RTCR` command stream belonged to a Realtek RTS5129
card reader (`0bda:0129`), not the EH57E. Never send that stream to this
fingerprint sensor. Always map a USB capture to VID/PID and bus address before
interpreting payloads.
