/* SPDX-License-Identifier: 0BSD */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libusb.h>

#define VID 0x1c7a
#define PID 0x057e
#define EP_OUT 0x01
#define EP_IN  0x82
#define EP_INTERRUPT_1 0x83
#define EP_INTERRUPT_2 0x84
#define TIMEOUT_MS 2000

static int quiet_exchange;

static int poll_u8 (libusb_device_handle *h, unsigned char reg,
                    unsigned char mask, unsigned char expected,
                    unsigned int timeout_ms);

static void
dump_bytes (const char *label, const unsigned char *buf, int len)
{
  printf ("%s (%d):", label, len);
  for (int i = 0; i < len; i++)
    printf (" %02x", buf[i]);
  putchar ('\n');
}

static int
exchange (libusb_device_handle *h, const char *name,
          const unsigned char *request, int request_len,
          unsigned char *response, int response_size)
{
  int transferred = 0;
  int rc;

  if (!quiet_exchange)
    dump_bytes (name, request, request_len);
  rc = libusb_bulk_transfer (h, EP_OUT, (unsigned char *) request,
                             request_len, &transferred, TIMEOUT_MS);
  if (rc != 0 || transferred != request_len)
    {
      fprintf (stderr, "%s write failed: %s (%d/%d bytes)\n", name,
               libusb_error_name (rc), transferred, request_len);
      return rc ? rc : LIBUSB_ERROR_IO;
    }

  memset (response, 0, response_size);
  rc = libusb_bulk_transfer (h, EP_IN, response, response_size,
                             &transferred, TIMEOUT_MS);
  if (rc != 0)
    {
      fprintf (stderr, "%s read failed: %s\n", name,
               libusb_error_name (rc));
      return rc;
    }
  if (!quiet_exchange)
    dump_bytes ("response", response, transferred);

  if (transferred < 7 || memcmp (response, "SIGE", 4) != 0)
    {
      fprintf (stderr, "%s: malformed response\n", name);
      return LIBUSB_ERROR_IO;
    }
  if (response[6] != 1)
    {
      fprintf (stderr, "%s: device rejected request (status 0x%02x)\n",
               name, response[6]);
      return LIBUSB_ERROR_PIPE;
    }
  return 0;
}

static int
read_u8 (libusb_device_handle *h, unsigned char reg, unsigned char *value)
{
  unsigned char request[] = { 'E', 'G', 'I', 'S', 0x60, reg, 0x00 };
  unsigned char response[7];
  int rc = exchange (h, "read_u8", request, sizeof request,
                     response, sizeof response);
  if (rc == 0)
    *value = response[5];
  return rc;
}

static int
write_u8 (libusb_device_handle *h, unsigned char reg, unsigned char value)
{
  unsigned char request[] = { 'E', 'G', 'I', 'S', 0x61, reg, value };
  unsigned char response[7];
  return exchange (h, "write_u8", request, sizeof request,
                   response, sizeof response);
}

static int
wait_for_finger_poll (libusb_device_handle *h, unsigned int timeout_ms)
{
  unsigned char request[] = { 'E', 'G', 'I', 'S', 0x05, 0x00, 0x00 };
  unsigned char response[7];
  int rc;

  /* EGIS_WAIT_INTERRUPT in the Windows driver is a protocol command followed
   * by polling register 0x01.  Despite its name, it does not read either USB
   * interrupt endpoint.  Bit 2 of register 0x01 means finger down. */
  if ((rc = exchange (h, "arm_finger_detection", request, sizeof request,
                      response, sizeof response)) != 0)
    return rc;

  puts ("finger detection armed; place a finger on the reader now");
  fflush (stdout);
  return poll_u8 (h, 0x01, 0x04, 0x04, timeout_ms);
}

static int
write_buf (libusb_device_handle *h, unsigned char command,
           const unsigned char *payload, unsigned char payload_len)
{
  unsigned char request[7 + 255];
  /* The device echoes a payload-sized response for buffered operations. */
  unsigned char response[7 + 255];

  memcpy (request, "EGIS", 4);
  request[4] = 0x63;
  request[5] = command;
  request[6] = payload_len;
  memcpy (request + 7, payload, payload_len);
  return exchange (h, "write_buf", request, 7 + payload_len,
                   response, sizeof response);
}

static int
read_buf (libusb_device_handle *h, unsigned char command,
          unsigned char *payload, unsigned char payload_len)
{
  unsigned char request[] = {
    'E', 'G', 'I', 'S', 0x62, command, payload_len
  };
  unsigned char response[7 + 255];
  int rc = exchange (h, "read_buf", request, sizeof request,
                     response, 7 + payload_len);

  if (rc == 0)
    memcpy (payload, response + 7, payload_len);
  return rc;
}

static int
xfer_buf (libusb_device_handle *h, unsigned char command,
          unsigned char *payload, unsigned char payload_len)
{
  unsigned char request[7 + 255];
  unsigned char response[7 + 255];
  int rc;

  memcpy (request, "EGIS", 4);
  request[4] = 0x71;
  request[5] = command;
  request[6] = payload_len;
  memcpy (request + 7, payload, payload_len);
  rc = exchange (h, "xfer_buf", request, 7 + payload_len,
                 response, 7 + payload_len);
  if (rc == 0)
    memcpy (payload, response + 7, payload_len);
  return rc;
}

static int
poll_u8 (libusb_device_handle *h, unsigned char reg,
         unsigned char mask, unsigned char expected, unsigned int timeout_ms)
{
  unsigned int elapsed = 0;

  quiet_exchange = 1;
  while (elapsed <= timeout_ms)
    {
      unsigned char value;
      int rc = read_u8 (h, reg, &value);

      if (rc != 0)
        {
          quiet_exchange = 0;
          return rc;
        }
      if ((value & mask) == expected)
        {
          quiet_exchange = 0;
          printf ("register 0x%02x ready: 0x%02x after %u ms\n",
                  reg, value, elapsed);
          return 0;
        }
      usleep (1000);
      elapsed++;
    }
  quiet_exchange = 0;
  fprintf (stderr, "register 0x%02x poll timed out\n", reg);
  return LIBUSB_ERROR_TIMEOUT;
}

static int
recover_sensor (libusb_device_handle *h)
{
  unsigned char state;
  unsigned char payload[] = { 0x01, 0x0c };
  int rc;

  if ((rc = read_u8 (h, 0x40, &state)) != 0)
    return rc;
  if ((state & 0x81) != 0x81)
    return 0;

  printf ("sensor state 0x%02x requires vendor recovery\n", state);
  if ((rc = write_u8 (h, 0x0a, 0xf4)) != 0 ||
      (rc = write_u8 (h, 0x0c, 0x44)) != 0 ||
      (rc = write_u8 (h, 0x40, 0x00)) != 0 ||
      (rc = poll_u8 (h, 0x40, 0x80, 0x00, 3000)) != 0 ||
      (rc = xfer_buf (h, 0x02, payload, sizeof payload)) != 0)
    return rc;
  return 0;
}

static int
calibrate (libusb_device_handle *h, unsigned char config[14])
{
  static const unsigned char cmd11[] = { 0x01, 0x00, 0x72 };
  static const unsigned char cmd34[] = { 0x07, 0x01 };
  /* In 0x180008e90 the final argument is tested by CMOVNE: a nonzero mode
   * selects 0x57, while zero selects 0x15.  The gain loop at 0x180009079
   * passes 1, so it must use 0x57. */
  static const unsigned char cmd2c[] = { 0x00, 0x57 };
  unsigned char sample[3];
  int rc;

  memset (config, 0, 14);

  if ((rc = write_buf (h, 0x11, cmd11, sizeof cmd11)) != 0 ||
      (rc = write_buf (h, 0x34, cmd34, sizeof cmd34)) != 0 ||
      (rc = poll_u8 (h, 0x35, 0x80, 0x00, 3000)) != 0 ||
      (rc = read_buf (h, 0x0d, config + 6, 3)) != 0 ||
      (rc = write_u8 (h, 0x12, 0x0a)) != 0)
    return rc;

  config[5] = 0x0a;
  printf ("initial calibration bytes: %02x %02x %02x\n",
          config[6], config[7], config[8]);

  for (;;)
    {
      if ((rc = write_u8 (h, 0x0f, config[8])) != 0 ||
          (rc = write_buf (h, 0x2c, cmd2c, sizeof cmd2c)) != 0 ||
          (rc = poll_u8 (h, 0x2d, 0x80, 0x00, 3000)) != 0 ||
          (rc = read_buf (h, 0x67, sample, sizeof sample)) != 0)
        return rc;

      printf ("calibration gain 0x%02x produced 0x%02x\n",
              config[8], sample[0]);
      if (sample[0] < 0x80 || config[8] == 0)
        break;
      config[8]--;
    }

  config[11] = sample[0] + 0x32;
  config[12] = 0x00;
  config[13] = 0xff;
  printf ("calibration result: cfg5=%02x cfg6=%02x cfg7=%02x "
          "cfg8=%02x cfg11=%02x cfg12=%02x cfg13=%02x\n",
          config[5], config[6], config[7], config[8], config[11],
          config[12], config[13]);
  return 0;
}

static int
dynamic_init (libusb_device_handle *h)
{
  static const unsigned char cmd26[] = { 0x0e, 0x36, 0x04, 0x0a, 0x2e, 0x04 };
  static const unsigned char cmd09[] = {
    0x83, 0x24, 0x00, 0x44, 0x0f, 0x08, 0x20, 0x20, 0x01, 0x05, 0x12
  };
  static const unsigned char cmd01[] = { 0x0c, 0x03 };
  unsigned char config[14];
  unsigned char cmd45[6];
  int rc;

  if ((rc = recover_sensor (h)) != 0 ||
      (rc = calibrate (h, config)) != 0)
    return rc;
  memcpy (cmd45, (unsigned char[]) { config[12], config[11],
                                     0x87, 0x13, 0x00, 0x03 },
          sizeof cmd45);

  if ((rc = write_buf (h, 0x26, cmd26, sizeof cmd26)) != 0 ||
      (rc = write_buf (h, 0x09, cmd09, sizeof cmd09)) != 0 ||
      (rc = write_buf (h, 0x01, cmd01, sizeof cmd01)) != 0 ||
      (rc = write_u8 (h, 0x0c, 0x22)) != 0 ||
      (rc = write_u8 (h, 0x0b, 0x03)) != 0 ||
      (rc = write_u8 (h, 0x0a, 0xfc)) != 0 ||
      (rc = xfer_buf (h, 0x45, cmd45, sizeof cmd45)) != 0)
    return rc;
  dump_bytes ("xfer45 data", cmd45, sizeof cmd45);
  return 0;
}

static int
raw_read (libusb_device_handle *h, unsigned char *data, int length,
          unsigned int timeout_ms, int command64_mode)
{
  unsigned char request[] = {
    /* EH57E calibration uses command 0x72.  Its normal-capture virtual method
     * (0x18001c010 -> 0x180018720) uses command 0x64 and, on device revision
     * >= 3, encodes the exact byte length in big-endian order.  The older
     * revision path rounds to 512-byte blocks, but that is not EH57E's path. */
    'E', 'G', 'I', 'S', command64_mode ? 0x64 : 0x72,
    (unsigned char) ((unsigned int) length >> 8),
    (unsigned char) length
  };
  int transferred = 0;
  int rc;

  dump_bytes ("raw_read request", request, sizeof request);
  rc = libusb_bulk_transfer (h, EP_OUT, request, sizeof request,
                             &transferred, TIMEOUT_MS);
  if (rc != 0 || transferred != (int) sizeof request)
    {
      fprintf (stderr, "raw image request failed: %s (%d/%zu bytes)\n",
               libusb_error_name (rc), transferred, sizeof request);
      return rc ? rc : LIBUSB_ERROR_IO;
    }
  transferred = 0;
  rc = libusb_bulk_transfer (h, EP_IN, data, length, &transferred,
                             timeout_ms);

  if (rc != 0)
    {
      fprintf (stderr, "raw image read failed: %s\n", libusb_error_name (rc));
      return rc;
    }
  printf ("raw image read: %d/%d bytes (command 0x%02x)\n", transferred,
          length, request[4]);
  if (transferred != length)
    return LIBUSB_ERROR_IO;
  return 0;
}

/* The Windows driver runs this image-path calibration before its separate
 * detect/finger calibration.  Function 0x180009e1c selects the fixed sensor
 * geometry, invokes helper 0x180008e90 with type 0 (opcode 0x57), and consumes
 * one 70x57 raw frame.  The later Windows code analyses that frame to refine
 * gain and bad-pixel parameters; retaining it here also lets us determine
 * whether this stage is what enables the EH57E image stream. */
static int
sensor_image_calibration (libusb_device_handle *h,
                          unsigned char calibration_frame[0x0f96])
{
  static const unsigned char sensor09[] = {
    0x83, 0x24, 0x00, 0x44, 0x0f, 0x08, 0x20, 0x20, 0x00, 0x00, 0x52
  };
  static const unsigned char cmd2c[] = { 0x00, 0x57 };
  static const unsigned char cmd54[] = { 0x01, 0x22, 0x1c };
  unsigned char cmd33[] = { 0x00, 0x10, 0x01 };
  unsigned char sample[3];
  int rc;

  puts ("starting Windows sensor/image calibration stage");
  if ((rc = recover_sensor (h)) != 0 ||
      (rc = write_buf (h, 0x09, sensor09, sizeof sensor09)) != 0 ||
      (rc = write_buf (h, 0x2c, cmd2c, sizeof cmd2c)) != 0 ||
      (rc = poll_u8 (h, 0x2d, 0x80, 0x00, 3000)) != 0 ||
      (rc = read_buf (h, 0x67, sample, sizeof sample)) != 0)
    return rc;
  dump_bytes ("sensor calibration response", sample, sizeof sample);

  cmd33[0] = sample[0];
  if ((rc = write_buf (h, 0x33, cmd33, sizeof cmd33)) != 0 ||
      (rc = poll_u8 (h, 0x35, 0x80, 0x00, 3000)) != 0 ||
      (rc = write_u8 (h, 0x0a, 0xf4)) != 0 ||
      (rc = write_u8 (h, 0x0c, 0x44)) != 0 ||
      (rc = write_u8 (h, 0x50, 0x01)) != 0 ||
      (rc = poll_u8 (h, 0x50, 0x80, 0x80, 3000)) != 0 ||
      (rc = write_buf (h, 0x54, cmd54, sizeof cmd54)) != 0 ||
      (rc = raw_read (h, calibration_frame, 0x0f96, 5000, 0)) != 0 ||
      (rc = write_u8 (h, 0x54, 0x00)) != 0)
    return rc;
  {
    unsigned char seen[256] = { 0 };
    unsigned int unique = 0;
    unsigned char minimum = calibration_frame[0];
    unsigned char maximum = calibration_frame[0];
    for (size_t i = 0; i < 0x0f96; i++)
      {
        unsigned char value = calibration_frame[i];
        if (!seen[value])
          {
            seen[value] = 1;
            unique++;
          }
        if (value < minimum)
          minimum = value;
        if (value > maximum)
          maximum = value;
      }
    printf ("sensor calibration frame: min=%u max=%u unique=%u\n",
            minimum, maximum, unique);
  }
  printf ("sensor image calibration setting: 0x%02x\n", cmd33[0]);
  return 0;
}

static int
direct_image_capture (libusb_device_handle *h, const char *filename)
{
  static const unsigned char cmd54[] = { 0x01, 0x22, 0x1c };
  static const unsigned char cmd2c[] = { 0x00, 0x57 };
  unsigned char calibration_frame[0x0f96];
  unsigned char images[3][0x0f96];
  unsigned char sample[3];
  FILE *output;
  int rc;

  if ((rc = sensor_image_calibration (h, calibration_frame)) != 0)
    return rc;
  puts ("direct image path ready; press Enter after placing a finger");
  fflush (stdout);
  if (getchar () == EOF)
    return LIBUSB_ERROR_OTHER;

  for (unsigned int frame = 0; frame < 3; frame++)
    {
      if ((rc = write_u8 (h, 0x0a, 0xf4)) != 0 ||
          (rc = write_u8 (h, 0x0c, 0x44)) != 0 ||
          (rc = write_u8 (h, 0x50, 0x01)) != 0 ||
          (rc = poll_u8 (h, 0x50, 0x80, 0x80, 3000)) != 0 ||
          (rc = write_buf (h, 0x54, cmd54, sizeof cmd54)) != 0 ||
          (rc = write_buf (h, 0x2c, cmd2c, sizeof cmd2c)) != 0 ||
          (rc = poll_u8 (h, 0x2d, 0x80, 0x00, 3000)) != 0 ||
          (rc = read_buf (h, 0x67, sample, sizeof sample)) != 0 ||
          (rc = raw_read (h, images[frame], sizeof images[frame],
                          5000, 0)) != 0 ||
          (rc = write_u8 (h, 0x54, 0x00)) != 0)
        return rc;
      printf ("captured direct frame %u/3\n", frame + 1);
    }

  for (unsigned int frame = 0; frame < 3; frame++)
    {
      unsigned int changed = 0;
      unsigned int total_delta = 0;
      unsigned int maximum_delta = 0;
      for (size_t i = 0; i < sizeof images[frame]; i++)
        {
          unsigned int a = calibration_frame[i];
          unsigned int b = images[frame][i];
          unsigned int delta = a > b ? a - b : b - a;
          if (delta)
            changed++;
          total_delta += delta;
          if (delta > maximum_delta)
            maximum_delta = delta;
        }
      printf ("direct frame %u versus no-finger calibration: "
              "%u/%zu pixels changed, mean abs delta %.3f, max %u\n",
              frame + 1, changed, sizeof images[frame],
              (double) total_delta / sizeof images[frame], maximum_delta);
    }

  output = fopen (filename, "wb");
  if (!output)
    {
      fprintf (stderr, "cannot open %s: %s\n", filename, strerror (errno));
      return LIBUSB_ERROR_OTHER;
    }
  if (fwrite (images, sizeof images, 1, output) != 1 || fclose (output) != 0)
    {
      fprintf (stderr, "failed to write %s\n", filename);
      return LIBUSB_ERROR_OTHER;
    }
  printf ("saved three direct 70x57 frames to %s\n", filename);
  return 0;
}

static int
wait_for_interrupt (libusb_device_handle *h, unsigned int timeout_ms)
{
  unsigned char data[16];
  const unsigned char endpoints[] = { EP_INTERRUPT_1, EP_INTERRUPT_2 };
  unsigned int elapsed = 0;

  while (elapsed < timeout_ms)
    for (unsigned int i = 0; i < sizeof endpoints; i++)
      {
        int transferred = 0;
        int rc = libusb_interrupt_transfer (h, endpoints[i], data,
                                            sizeof data, &transferred, 100);
        elapsed += 100;
        if (rc == LIBUSB_ERROR_TIMEOUT)
          continue;
        if (rc != 0)
          {
            fprintf (stderr, "interrupt endpoint 0x%02x failed: %s\n",
                     endpoints[i], libusb_error_name (rc));
            return rc;
          }
        printf ("interrupt endpoint 0x%02x: ", endpoints[i]);
        dump_bytes ("notification", data, transferred);
        return 0;
      }

  fprintf (stderr, "timed out waiting for a fingerprint interrupt\n");
  return LIBUSB_ERROR_TIMEOUT;
}

static int
capture_image (libusb_device_handle *h, const char *filename, int block_mode,
               int interrupt_mode, int manual_mode, int burst_raw72_mode,
               int image_phase_only_mode)
{
  static const unsigned char capture09[] = {
    0x83, 0x24, 0x00, 0x44, 0x0f, 0x08, 0x20, 0x20, 0x01, 0x05, 0x12
  };
  static const unsigned char cmd26[] = { 0x0e, 0x36, 0x04, 0x0a, 0x2e, 0x04 };
  unsigned char images[3][0x0f96];
  unsigned char calibration_frame[0x0f96];
  FILE *output;
  int rc;

  if ((rc = sensor_image_calibration (h, calibration_frame)) != 0)
    return rc;
  /* Windows keeps image-path setup and the later detection/gain calibration
   * as separate state-machine phases.  The older probe always ran
   * dynamic_init here, which can reset the freshly calibrated image path. */
  if (!image_phase_only_mode && (rc = dynamic_init (h)) != 0)
    return rc;

  if ((rc = write_buf (h, 0x09, capture09, sizeof capture09)) != 0 ||
      (rc = write_buf (h, 0x26, cmd26, sizeof cmd26)) != 0 ||
      (rc = write_u8 (h, 0x23, 0x00)) != 0 ||
      (rc = write_u8 (h, 0x24, 0x38)) != 0 ||
      (rc = write_u8 (h, 0x20, 0x00)) != 0 ||
      (rc = write_u8 (h, 0x21, 0x45)) != 0)
    return rc;

  puts ("capture mode enabled; waiting for a finger");
  fflush (stdout);
  {
    /* Windows sensor-window calibration (0x18000935c) writes register 0x2c
     * once, then independently arms, polls, downloads, and completes each of
     * three frames.  Earlier Linux trials used this sequencing with the wrong
     * raw opcode 0x64; repeat it with EH57E's actual opcode 0x72. */
    if (!block_mode && (rc = write_u8 (h, 0x2c, 0x00)) != 0)
      return rc;
    if (manual_mode)
    {
      unsigned char before_placement;
      unsigned char before_finger_state;
      if ((rc = read_u8 (h, 0x00, &before_placement)) != 0)
        return rc;
      if ((rc = read_u8 (h, 0x01, &before_finger_state)) != 0)
        return rc;
      printf ("capture state before placement: reg00=0x%02x reg01=0x%02x "
              "(finger bits=0x%02x)\n", before_placement,
              before_finger_state, before_finger_state & 0x03);
      puts ("capture mode is active; press Enter only after placing a finger");
      fflush (stdout);
      if (getchar () == EOF)
        return LIBUSB_ERROR_OTHER;
      {
        unsigned char after_placement;
        unsigned char after_finger_state;
        if ((rc = read_u8 (h, 0x00, &after_placement)) != 0)
          return rc;
        if ((rc = read_u8 (h, 0x01, &after_finger_state)) != 0)
          return rc;
        printf ("capture state after placement: reg00=0x%02x reg01=0x%02x "
                "(finger bits=0x%02x)\n", after_placement,
                after_finger_state, after_finger_state & 0x03);
      }
    }

    if (block_mode)
      {
        /* Windows live acquisition (0x180009d20) arms all requested frames
         * in one buffered command: { frame_count - 1, 0x13 }.  It then calls
         * the normal-capture virtual method once per frame, which resolves to
         * command 0x64 on EH57E. */
        static const unsigned char burst2c[] = { 0x02, 0x13 };
        if ((rc = write_buf (h, 0x2c, burst2c, sizeof burst2c)) != 0 ||
            (rc = poll_u8 (h, 0x00, 0x01, 0x01, 15000)) != 0)
          return rc;
        for (unsigned int frame = 0; frame < 3; frame++)
          {
            if ((rc = raw_read (h, images[frame], sizeof images[frame],
                                5000, !burst_raw72_mode)) != 0)
              return rc;
            printf ("captured live burst frame %u/3\n", frame + 1);
          }
        if ((rc = write_u8 (h, 0x2d, 0x20)) != 0)
          return rc;
      }
    else for (unsigned int frame = 0; frame < 3; frame++)
      {
        if ((rc = write_u8 (h, 0x2d, 0x13)) != 0 ||
            (rc = interrupt_mode ? wait_for_interrupt (h, 60000) :
                                   poll_u8 (h, 0x00, 0x01, 0x01,
                                            15000)) != 0 ||
            (rc = raw_read (h, images[frame], sizeof images[frame], 5000,
                            block_mode)) != 0 ||
            (rc = write_u8 (h, 0x2d, 0x20)) != 0)
          return rc;
        printf ("captured frame %u/3\n", frame + 1);
      }
  }

  output = fopen (filename, "wb");
  if (!output)
    {
      fprintf (stderr, "cannot open %s: %s\n", filename, strerror (errno));
      return LIBUSB_ERROR_OTHER;
    }
  if (fwrite (images, sizeof images, 1, output) != 1 || fclose (output) != 0)
    {
      fprintf (stderr, "failed to write %s\n", filename);
      return LIBUSB_ERROR_OTHER;
    }
  printf ("saved three 70x57 frames to %s\n", filename);
  return 0;
}

/* This is the constant portion of the Windows detection sequence.  It omits
 * sensor calibration and capture until their dynamic values are understood. */
static int
constant_init (libusb_device_handle *h)
{
  static const unsigned char cmd26[] = { 0x0e, 0x36, 0x04, 0x0a, 0x2e, 0x04 };
  static const unsigned char cmd01[] = { 0x0c, 0x03 };
  int rc;

  if ((rc = write_buf (h, 0x26, cmd26, sizeof cmd26)) != 0 ||
      (rc = write_buf (h, 0x01, cmd01, sizeof cmd01)) != 0 ||
      (rc = write_u8 (h, 0x0c, 0x22)) != 0 ||
      (rc = write_u8 (h, 0x0b, 0x03)) != 0 ||
      (rc = write_u8 (h, 0x0a, 0xfc)) != 0)
    return rc;
  return 0;
}

static void
usage (const char *program)
{
  fprintf (stderr,
           "usage: %s [--read REG | --read-buf CMD LEN | --constant-init | --calibrate | --dynamic-init | --finger-wait | --capture FILE | --capture-manual FILE | --capture-direct FILE | --capture-interrupt FILE | --capture-block FILE | --capture-block-manual FILE | --capture-block-raw72-manual FILE | --capture-image-phase-manual FILE | --drain-image | --drain-pending-block]\n",
           program);
}

int
main (int argc, char **argv)
{
  libusb_context *ctx = NULL;
  libusb_device_handle *h = NULL;
  unsigned long reg = 0x40;
  unsigned long read_len = 0;
  int do_init = 0;
  int do_calibrate = 0;
  int do_dynamic_init = 0;
  int do_finger_wait = 0;
  int do_read_buf = 0;
  const char *capture_filename = NULL;
  int capture_block_mode = 0;
  int capture_interrupt_mode = 0;
  int capture_manual_mode = 0;
  int capture_burst_raw72_mode = 0;
  int capture_image_phase_only_mode = 0;
  int capture_direct_mode = 0;
  int do_drain_image = 0;
  int do_drain_pending_block = 0;
  int rc;

  if (argc == 3 && strcmp (argv[1], "--read") == 0)
    {
      char *end = NULL;
      errno = 0;
      reg = strtoul (argv[2], &end, 0);
      if (errno || !end || *end || reg > 0xff)
        {
          usage (argv[0]);
          return 2;
        }
    }
  else if (argc == 2 && strcmp (argv[1], "--constant-init") == 0)
    do_init = 1;
  else if (argc == 2 && strcmp (argv[1], "--calibrate") == 0)
    do_calibrate = 1;
  else if (argc == 2 && strcmp (argv[1], "--dynamic-init") == 0)
    do_dynamic_init = 1;
  else if (argc == 2 && strcmp (argv[1], "--finger-wait") == 0)
    do_finger_wait = 1;
  else if (argc == 3 && strcmp (argv[1], "--capture") == 0)
    capture_filename = argv[2];
  else if (argc == 3 && strcmp (argv[1], "--capture-block") == 0)
    {
      capture_filename = argv[2];
      capture_block_mode = 1;
    }
  else if (argc == 3 && strcmp (argv[1], "--capture-block-manual") == 0)
    {
      capture_filename = argv[2];
      capture_block_mode = 1;
      capture_manual_mode = 1;
    }
  else if (argc == 3 &&
           strcmp (argv[1], "--capture-block-raw72-manual") == 0)
    {
      capture_filename = argv[2];
      capture_block_mode = 1;
      capture_manual_mode = 1;
      capture_burst_raw72_mode = 1;
    }
  else if (argc == 3 &&
           strcmp (argv[1], "--capture-image-phase-manual") == 0)
    {
      capture_filename = argv[2];
      capture_block_mode = 1;
      capture_manual_mode = 1;
      capture_image_phase_only_mode = 1;
    }
  else if (argc == 3 && strcmp (argv[1], "--capture-interrupt") == 0)
    {
      capture_filename = argv[2];
      capture_interrupt_mode = 1;
    }
  else if (argc == 3 && strcmp (argv[1], "--capture-manual") == 0)
    {
      capture_filename = argv[2];
      capture_manual_mode = 1;
    }
  else if (argc == 3 && strcmp (argv[1], "--capture-direct") == 0)
    {
      capture_filename = argv[2];
      capture_direct_mode = 1;
    }
  else if (argc == 2 && strcmp (argv[1], "--drain-image") == 0)
    do_drain_image = 1;
  else if (argc == 2 && strcmp (argv[1], "--drain-pending-block") == 0)
    do_drain_pending_block = 1;
  else if (argc == 4 && strcmp (argv[1], "--read-buf") == 0)
    {
      char *reg_end = NULL;
      char *len_end = NULL;
      errno = 0;
      reg = strtoul (argv[2], &reg_end, 0);
      read_len = strtoul (argv[3], &len_end, 0);
      if (errno || !reg_end || *reg_end || !len_end || *len_end ||
          reg > 0xff || read_len > 0xff)
        {
          usage (argv[0]);
          return 2;
        }
      do_read_buf = 1;
    }
  else if (argc != 1)
    {
      usage (argv[0]);
      return 2;
    }

  if ((rc = libusb_init (&ctx)) != 0)
    return 1;
  h = libusb_open_device_with_vid_pid (ctx, VID, PID);
  if (!h)
    {
      fprintf (stderr, "device %04x:%04x not found or inaccessible\n", VID, PID);
      libusb_exit (ctx);
      return 1;
    }

  libusb_set_auto_detach_kernel_driver (h, 1);
  rc = libusb_claim_interface (h, 0);
  if (rc != 0)
    {
      fprintf (stderr, "claim failed: %s\n", libusb_error_name (rc));
      libusb_close (h);
      libusb_exit (ctx);
      return 1;
    }

  if (do_init)
    rc = constant_init (h);
  else if (do_calibrate)
    {
      unsigned char config[14];
      rc = calibrate (h, config);
    }
  else if (do_dynamic_init)
    rc = dynamic_init (h);
  else if (do_finger_wait)
    {
      rc = dynamic_init (h);
      if (rc == 0)
        rc = wait_for_finger_poll (h, 60000);
    }
  else if (capture_filename)
    rc = capture_direct_mode ? direct_image_capture (h, capture_filename) :
         capture_image (h, capture_filename, capture_block_mode,
                        capture_interrupt_mode, capture_manual_mode,
                        capture_burst_raw72_mode,
                        capture_image_phase_only_mode);
  else if (do_drain_image)
    {
      unsigned char discarded[0x0f96];
      rc = raw_read (h, discarded, sizeof discarded, 5000, 0);
      if (rc == 0)
        rc = write_u8 (h, 0x2d, 0x20);
    }
  else if (do_drain_pending_block)
    {
      unsigned char pending[0x1000];
      int transferred = 0;
      rc = libusb_bulk_transfer (h, EP_IN, pending, sizeof pending,
                                 &transferred, 5000);
      printf ("pending block read: %d/%zu bytes\n", transferred,
              sizeof pending);
      if (transferred > 0)
        dump_bytes ("pending block prefix", pending,
                    transferred < 32 ? transferred : 32);
      if (rc == 0 && transferred == (int) sizeof pending)
        rc = write_u8 (h, 0x2d, 0x20);
      else if (rc == 0)
        rc = LIBUSB_ERROR_IO;
    }
  else if (do_read_buf)
    {
      unsigned char data[255];
      rc = read_buf (h, (unsigned char) reg, data,
                     (unsigned char) read_len);
      if (rc == 0)
        dump_bytes ("buffer data", data, read_len);
    }
  else
    {
      unsigned char value = 0;
      rc = read_u8 (h, (unsigned char) reg, &value);
      if (rc == 0)
        printf ("register 0x%02lx = 0x%02x\n", reg, value);
    }

  libusb_release_interface (h, 0);
  libusb_close (h);
  libusb_exit (ctx);
  return rc == 0 ? 0 : 1;
}
