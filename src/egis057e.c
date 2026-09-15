/*
 * Egis Technology 057E driver for libfprint
 *
 * SPDX-License-Identifier: 0BSD
 * Copyright (C) 2026 EH57E Linux driver contributors
 *
 * Hardware: LighTuning Technology Inc. EgisTec EH57E
 * USB ID:   1c7a:057e
 * Chip:     ET5xx series (ET300/310/320/516 variants)
 *
 * Reverse-engineered from the Windows UMDF driver:
 *   EgisTouchFP057E.dll / EgisTouchFPSensor057E.dll
 *   Driver version 3.12.3.2 (2021-07-07)
 *   Source tree: C:\builds\...\ETU813\etu813.driver2\Main\WBF\source\UMDF\UMDFSource\ET5XX\
 *
 * Endpoint layout: bulk OUT 0x01, bulk IN 0x82, interrupt IN 0x83/0x84.
 *
 * A previous revision used a 53-step "RTCR" sequence mined from the Windows
 * VM usbmon capture.  Re-checking that capture against USB descriptors showed
 * those RTCR transfers belong to the Realtek RTS5129 card reader
 * (0bda:0129), not this fingerprint reader (1c7a:057e).  Do not send RTCR to
 * this device.
 *
 * Static analysis of the vendor UMDF component recovered the command framing.
 * Scalar read/write operations use opcodes 0x60/0x61 and buffered write/read
 * operations use 0x63/0x71. Responses start with "SIGE" and byte 6 is 1 on
 * success. Normal image capture uses opcode 0x64 with an exact 0x0f96-byte
 * transfer, producing a 70x57 grayscale frame.
 */

#define FP_COMPONENT "egis057e"

#include <math.h>

#include "egis057e.h"

#define EGIS057E_MATCH_THRESHOLD 0.30
#define EGIS057E_SECOND_MATCH_THRESHOLD 0.23
#define EGIS057E_ENROLL_DUPLICATE_THRESHOLD 0.92
#define EGIS057E_FRAME_CHANGE_MARGIN 0.10
#define EGIS057E_SETTLING_FRAMES 8
#define EGIS057E_RELEASE_MIN_DIFFERENCE 10.0
#define EGIS057E_RELEASE_THRESHOLD_MULTIPLIER 3.0
#define EGIS057E_RELEASE_FRAMES 2
#define EGIS057E_INITIAL_TOUCH_THRESHOLD 3.0
#define EGIS057E_TOUCH_STABLE_MAX_DIFFERENCE 2.0
#define EGIS057E_TOUCH_STABLE_FRAMES 3
#define EGIS057E_TOUCH_SETTLE_MAX_FRAMES 8
#define EGIS057E_MIN_IMAGE_STDDEV 15.0
#define EGIS057E_MIN_GRADIENT_RMS 30.0
#define EGIS057E_MIN_INTENSITY_RANGE 30
#define EGIS057E_INITIAL_FINGER_MIN_STDDEV 65.0
#define EGIS057E_INITIAL_FINGER_MIN_GRADIENT_RMS 38.0
#define EGIS057E_INITIAL_FINGER_MIN_INTENSITY_RANGE 200

/* -------------------------------------------------------------------------
 * Device state
 * ------------------------------------------------------------------------- */

struct _FpDeviceEgis057e
{
  FpImageDevice parent;

  gboolean      running;
  gboolean      stop;
  gboolean      activated;

  guint         init_step;        /* current index in egis057e_init[]   */
  guint         capture_step;     /* current index in capture sequence  */
  guint8        int_ep;           /* interrupt endpoint currently polled */
  guint8        previous_frame[EGIS057E_IMAGE_LEN];
  guint8        captured_frame[EGIS057E_IMAGE_LEN];
  guint8        clear_frame[EGIS057E_IMAGE_LEN];
  guint8        touch_best_frame[EGIS057E_IMAGE_LEN];
  gboolean      have_previous_frame;
  gboolean      have_captured_frame;
  gboolean      have_clear_frame;
  gboolean      have_touch_best_frame;
  gboolean      detection_armed;
  guint         stable_frames;
  guint         finger_frames;
  guint         release_frames;
  guint         touch_settle_frames;
  guint         touch_stable_frames;
  gboolean      touch_pending;
  double        baseline_sum;
  double        change_threshold;
  double        touch_best_difference;
  double        touch_best_quality;
  guint8        calibration_sample;
  gboolean      have_calibration_sample;
  gboolean      awaiting_release;
  gboolean      reported_image;
  gboolean      capture_protocol_error;
};

G_DECLARE_FINAL_TYPE (FpDeviceEgis057e, fpi_device_egis057e,
                      FPI, DEVICE_EGIS057E, FpImageDevice);
G_DEFINE_TYPE (FpDeviceEgis057e, fpi_device_egis057e, FP_TYPE_IMAGE_DEVICE);

/* -------------------------------------------------------------------------
 * SSM states
 * ------------------------------------------------------------------------- */

enum sm_states {
  /* Init SSM */
  SM_INIT_SEND,      /* send current init step command         */
  SM_INIT_RECV,      /* receive 4-byte response (conditional)  */
  SM_INIT_NEXT,      /* advance to next step or finish         */

  /* Detect-mode loop */
  SM_CAPTURE_DELAY,  /* interactive grace period after baseline */
  SM_DETECT_WAIT,    /* wait for finger interrupt on EP 0x83   */
  SM_CAPTURE_SEND,   /* send current capture trigger command   */
  SM_CAPTURE_RECV,   /* receive command response               */
  SM_CAPTURE_NEXT,   /* advance capture trigger command        */
  SM_CAPTURE_REQUEST,/* request one 0x0f96-byte image           */
  SM_CAPTURE_IMAGE,  /* receive provisional 0xf96 image frame  */
  SM_CAPTURE_FINISH, /* acknowledge completion                  */
  SM_CAPTURE_FINISH_RECV,
  SM_DETECT_DONE,    /* restart loop or stop                   */

  SM_STATES_NUM
};

typedef struct
{
  guint8 bytes[EGIS057E_MAX_PKT_LEN];
  guint len;
  const char *name;
  gboolean expect_response;
} Egis057ePacket;

/*
 * Confirmed ET5XX command packets recovered from EgisTouchFP057E.dll and
 * validated directly against an EH57E. Buffered-write responses echo the
 * request payload and are therefore as long as the request.
 */
static const Egis057ePacket egis057e_init_pkts[] = {
  /* A completed capture leaves 0x40 at 0x83.  The vendor recovery sequence
   * is harmless from the idle state and makes activation deterministic. */
  { { 'E', 'G', 'I', 'S', 0x61, 0x0a, 0xf4 }, 7, "recover 0x0a", TRUE },
  { { 'E', 'G', 'I', 'S', 0x61, 0x0c, 0x44 }, 7, "recover 0x0c", TRUE },
  { { 'E', 'G', 'I', 'S', 0x61, 0x40, 0x00 }, 7, "recover 0x40", TRUE },
  { { 'E', 'G', 'I', 'S', 0x71, 0x02, 0x02, 0x01, 0x0c },
    9, "recover xfer_buf 0x02", TRUE },
  { { 'E', 'G', 'I', 'S', 0x63, 0x09, 0x0b,
      0x83, 0x24, 0x00, 0x44, 0x0f, 0x08, 0x20, 0x20, 0x00, 0x00, 0x52 },
    18, "image calibration mode", TRUE },
  { { 'E', 'G', 'I', 'S', 0x63, 0x2c, 0x02, 0x00, 0x57 },
    9, "image calibration sample", TRUE },
  { { 'E', 'G', 'I', 'S', 0x60, 0x2d, 0x00 },
    7, "calibration poll 0x2d", TRUE },
  { { 'E', 'G', 'I', 'S', 0x62, 0x67, 0x03 },
    7, "calibration read_buf 0x67", TRUE },
  /* 0x6d is the stable value observed in the successful image-path trials.
   * A follow-up callback will substitute byte 7 of the preceding response. */
  { { 'E', 'G', 'I', 'S', 0x63, 0x33, 0x03, 0x6d, 0x10, 0x01 },
    10, "image calibration apply", TRUE },
  { { 'E', 'G', 'I', 'S', 0x60, 0x35, 0x00 }, 7, "poll calibration", TRUE },
  { { 'E', 'G', 'I', 'S', 0x61, 0x0a, 0xf4 }, 7, "image power 0x0a", TRUE },
  { { 'E', 'G', 'I', 'S', 0x61, 0x0c, 0x44 }, 7, "image power 0x0c", TRUE },
  { { 'E', 'G', 'I', 'S', 0x61, 0x50, 0x01 }, 7, "image power 0x50", TRUE },
  { { 'E', 'G', 'I', 'S', 0x60, 0x50, 0x00 }, 7, "poll image power", TRUE },
  { { 'E', 'G', 'I', 'S', 0x63, 0x54, 0x03, 0x01, 0x22, 0x1c },
    10, "enable calibration image", TRUE },
  /* Opcode 0x72 returns a raw 3990-byte calibration frame, not SIGE. */
  { { 'E', 'G', 'I', 'S', 0x72, 0x0f, 0x96 },
    7, "read calibration image", TRUE },
  { { 'E', 'G', 'I', 'S', 0x61, 0x54, 0x00 }, 7, "end calibration image", TRUE },
  { { 'E', 'G', 'I', 'S', 0x63, 0x09, 0x0b,
      0x83, 0x24, 0x00, 0x44, 0x0f, 0x08, 0x20, 0x20, 0x01, 0x05, 0x12 },
    18, "normal image mode", TRUE },
  { { 'E', 'G', 'I', 'S', 0x63, 0x26, 0x06,
      0x0e, 0x36, 0x04, 0x0a, 0x2e, 0x04 }, 13, "image window", TRUE },
  { { 'E', 'G', 'I', 'S', 0x61, 0x23, 0x00 }, 7, "image reg 0x23", TRUE },
  { { 'E', 'G', 'I', 'S', 0x61, 0x24, 0x38 }, 7, "image reg 0x24", TRUE },
  { { 'E', 'G', 'I', 'S', 0x61, 0x20, 0x00 }, 7, "image reg 0x20", TRUE },
  { { 'E', 'G', 'I', 'S', 0x61, 0x21, 0x45 }, 7, "image reg 0x21", TRUE },
};

static const Egis057ePacket egis057e_capture_pkts[] = {
  { { 'E', 'G', 'I', 'S', 0x63, 0x2c, 0x02, 0x00, 0x13 },
    9, "arm one live frame", TRUE },
  { { 'E', 'G', 'I', 'S', 0x60, 0x00, 0x00 },
    7, "poll live frame", TRUE },
};

static void
egis057e_log_bytes (const char *prefix, const guint8 *buf, gsize len)
{
  g_autoptr(GString) s = g_string_new (NULL);

  for (gsize i = 0; i < len; i++)
    g_string_append_printf (s, "%s%02x", i ? " " : "", buf[i]);

  fp_dbg ("%s%s", prefix, s->str);
}

/* -------------------------------------------------------------------------
 * USB helpers
 * ------------------------------------------------------------------------- */

static void
int_finger_cb (FpiUsbTransfer *transfer, FpDevice *dev,
               gpointer user_data, GError *error)
{
  FpImageDevice *img_dev = FP_IMAGE_DEVICE (dev);

  if (error)
    {
      if (g_error_matches (error, G_USB_DEVICE_ERROR,
                           G_USB_DEVICE_ERROR_CANCELLED))
        {
          g_error_free (error);
          fpi_ssm_mark_completed (transfer->ssm);
          return;
        }
      if (g_error_matches (error, G_USB_DEVICE_ERROR,
                           G_USB_DEVICE_ERROR_TIMED_OUT))
        {
          FpDeviceEgis057e *self = FPI_DEVICE_EGIS057E (dev);

          if (self->int_ep == EGIS057E_EP_INT_FINGER)
            {
              self->int_ep = EGIS057E_EP_INT_ALT;
              g_error_free (error);
              fpi_ssm_jump_to_state (transfer->ssm, SM_DETECT_WAIT);
              return;
            }

          self->int_ep = EGIS057E_EP_INT_FINGER;
          g_error_free (error);
          fp_dbg ("interrupt wait timed out; continuing finger wait");
          fpi_ssm_jump_to_state (transfer->ssm, SM_DETECT_WAIT);
          return;
        }
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  egis057e_log_bytes ("interrupt payload: ", transfer->buffer,
                      transfer->actual_length);
  fpi_image_device_report_finger_status (img_dev, TRUE);
  fpi_ssm_next_state (transfer->ssm);
}

static void
cmd_resp_cb (FpiUsbTransfer *transfer, FpDevice *dev,
             gpointer user_data, GError *error)
{
  FpDeviceEgis057e *self = FPI_DEVICE_EGIS057E (dev);
  guint expected_length = GPOINTER_TO_UINT (user_data);

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  if (transfer->actual_length != expected_length)
    {
      fpi_ssm_mark_failed (
        transfer->ssm,
        fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                  "Unexpected command response length %zd (expected %u)",
                                  transfer->actual_length, expected_length));
      return;
    }

  if (transfer->actual_length < EGIS057E_RESP_LEN ||
      memcmp (transfer->buffer, "SIGE", 4) != 0)
    {
      fpi_ssm_mark_failed (transfer->ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "Malformed command response"));
      return;
    }

  if (transfer->buffer[6] != 0x01)
    {
      fpi_ssm_mark_failed (
        transfer->ssm,
        fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                  "Device rejected command (status 0x%02x)",
                                  transfer->buffer[6]));
      return;
    }

  egis057e_log_bytes ("command response: ", transfer->buffer,
                      transfer->actual_length);

  /* read_buf(0x67, 3) returns the per-activation calibration sample as
   * the first payload byte.  Keep it per device instance and substitute it
   * into the following write_buf(0x33, ...) command. */
  if (self->init_step < G_N_ELEMENTS (egis057e_init_pkts) &&
      egis057e_init_pkts[self->init_step].bytes[4] == 0x62 &&
      egis057e_init_pkts[self->init_step].bytes[5] == 0x67 &&
      transfer->actual_length >= EGIS057E_RESP_LEN + 1)
    {
      self->calibration_sample = transfer->buffer[EGIS057E_RESP_LEN];
      self->have_calibration_sample = TRUE;
      fp_dbg ("runtime image calibration sample: 0x%02x",
              self->calibration_sample);
    }
  fpi_ssm_next_state (transfer->ssm);
}

static void
calibration_image_cb (FpiUsbTransfer *transfer, FpDevice *dev,
                      gpointer user_data, GError *error)
{
  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  fp_dbg ("calibration image read returned %zd/%d bytes",
          transfer->actual_length, EGIS057E_IMAGE_LEN);
  if (transfer->actual_length != EGIS057E_IMAGE_LEN)
    {
      fpi_ssm_mark_failed (transfer->ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "Short calibration image"));
      return;
    }
  fpi_ssm_next_state (transfer->ssm);
}

static void
frame_mean_difference (const guint8 *a, const guint8 *b, double *result)
{
  guint64 difference = 0;

  for (guint i = 0; i < EGIS057E_IMAGE_LEN; i++)
    difference += ABS ((gint) a[i] - (gint) b[i]);
  *result = (double) difference / EGIS057E_IMAGE_LEN;
}

typedef struct
{
  double standard_deviation;
  double gradient_rms;
  guint intensity_range;
  double score;
} Egis057eImageQuality;

static Egis057eImageQuality
image_quality (const guint8 *image)
{
  Egis057eImageQuality quality = { 0 };
  guint histogram[256] = { 0 };
  guint64 sum = 0, squared_sum = 0, gradient_squared_sum = 0;
  guint gradient_count = 0, below = 0;
  guint low = 0, high = 255;

  for (guint i = 0; i < EGIS057E_IMAGE_LEN; i++)
    {
      guint value = image[i];
      histogram[value]++;
      sum += value;
      squared_sum += value * value;
    }

  for (guint y = 1; y + 1 < EGIS057E_IMAGE_HEIGHT; y++)
    for (guint x = 1; x + 1 < EGIS057E_IMAGE_WIDTH; x++)
      {
        gint gx = image[y * EGIS057E_IMAGE_WIDTH + x + 1] -
                  image[y * EGIS057E_IMAGE_WIDTH + x - 1];
        gint gy = image[(y + 1) * EGIS057E_IMAGE_WIDTH + x] -
                  image[(y - 1) * EGIS057E_IMAGE_WIDTH + x];
        gradient_squared_sum += gx * gx + gy * gy;
        gradient_count++;
      }

  for (low = 0; low < 255; low++)
    {
      below += histogram[low];
      if (below >= EGIS057E_IMAGE_LEN / 20)
        break;
    }
  below = 0;
  for (high = 255; high > 0; high--)
    {
      below += histogram[high];
      if (below >= EGIS057E_IMAGE_LEN / 20)
        break;
    }

  {
    double mean = (double) sum / EGIS057E_IMAGE_LEN;
    double variance = (double) squared_sum / EGIS057E_IMAGE_LEN - mean * mean;

    quality.standard_deviation = sqrt (MAX (variance, 0.0));
  }
  quality.gradient_rms = sqrt ((double) gradient_squared_sum / gradient_count);
  quality.intensity_range = high > low ? high - low : 0;
  quality.score = quality.gradient_rms + quality.intensity_range;
  return quality;
}

static gboolean
image_quality_acceptable (const Egis057eImageQuality *quality)
{
  return quality->standard_deviation >= EGIS057E_MIN_IMAGE_STDDEV &&
         quality->gradient_rms >= EGIS057E_MIN_GRADIENT_RMS &&
         quality->intensity_range >= EGIS057E_MIN_INTENSITY_RANGE;
}

static gboolean
initial_frame_has_finger (const Egis057eImageQuality *quality)
{
  return quality->gradient_rms >= EGIS057E_INITIAL_FINGER_MIN_GRADIENT_RMS &&
         (quality->standard_deviation >= EGIS057E_INITIAL_FINGER_MIN_STDDEV ||
          quality->intensity_range >=
            EGIS057E_INITIAL_FINGER_MIN_INTENSITY_RANGE);
}

static void
image_recv_cb (FpiUsbTransfer *transfer, FpDevice *dev,
               gpointer user_data, GError *error)
{
  FpImageDevice *img_dev = FP_IMAGE_DEVICE (dev);
  FpDeviceEgis057e *self = FPI_DEVICE_EGIS057E (dev);

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  fp_dbg ("image read returned %zd/%d bytes", transfer->actual_length,
          EGIS057E_IMAGE_LEN);

  if (transfer->actual_length < EGIS057E_IMAGE_LEN)
    {
      fp_warn ("short image payload: received %zd/%d bytes",
               transfer->actual_length, EGIS057E_IMAGE_LEN);
      fpi_image_device_report_finger_status (img_dev, FALSE);
      self->capture_protocol_error = TRUE;
      fpi_ssm_jump_to_state (transfer->ssm, SM_CAPTURE_FINISH);
      return;
    }

  if (!self->have_previous_frame)
    {
      memcpy (self->previous_frame, transfer->buffer, EGIS057E_IMAGE_LEN);
      self->have_previous_frame = TRUE;
      fp_dbg ("stored initial retained frame; waiting for sensor update");
    }
  else
    {
      double mean_difference;

      frame_mean_difference (transfer->buffer, self->previous_frame,
                             &mean_difference);
      memcpy (self->previous_frame, transfer->buffer, EGIS057E_IMAGE_LEN);
      if (!self->detection_armed)
        {
          self->baseline_sum += mean_difference;
          self->stable_frames++;
          if (self->stable_frames >= EGIS057E_SETTLING_FRAMES)
            {
              double initial_activity =
                self->baseline_sum / self->stable_frames;
              Egis057eImageQuality initial_quality =
                image_quality (transfer->buffer);
              gboolean initial_finger =
                initial_activity >= EGIS057E_INITIAL_TOUCH_THRESHOLD ||
                initial_frame_has_finger (&initial_quality);

              self->detection_armed = TRUE;
              self->change_threshold = initial_activity +
                                       EGIS057E_FRAME_CHANGE_MARGIN;
              fp_dbg ("automatic finger detection armed: clear baseline %.4f, threshold %.4f",
                      initial_activity,
                      self->change_threshold);
              fp_dbg ("initial retained-frame quality: stddev %.2f, gradient %.2f, range %u, mean-independent score %.2f",
                      initial_quality.standard_deviation,
                      initial_quality.gradient_rms,
                      initial_quality.intensity_range,
                      initial_quality.score);

              if (!initial_finger)
                {
                  memcpy (self->clear_frame, transfer->buffer,
                          EGIS057E_IMAGE_LEN);
                  self->have_clear_frame = TRUE;
                }

              /* Lock screens may start PAM after the user has already put a
               * finger down. Detect that either from temporal activity or
               * from spatial ridge energy and contrast; a completely still
               * finger otherwise looks like a clear temporal baseline. */
              if (initial_finger)
                {
                  self->touch_settle_frames = 0;
                  self->touch_stable_frames = 0;
                  self->touch_best_difference = G_MAXDOUBLE;
                  self->touch_best_quality = -1.0;
                  self->have_touch_best_frame = FALSE;
                  self->touch_pending = TRUE;
                  fp_dbg ("initial temporal/spatial signature indicates finger already present; waiting for stable contact");
                }
            }
        }
      else if (self->awaiting_release)
        {
          double captured_difference;
          double clear_difference = G_MAXDOUBLE;
          double release_threshold =
            MAX (EGIS057E_RELEASE_MIN_DIFFERENCE,
                 self->change_threshold *
                 EGIS057E_RELEASE_THRESHOLD_MULTIPLIER);

          g_assert (self->have_captured_frame);
          frame_mean_difference (transfer->buffer, self->captured_frame,
                                 &captured_difference);
          if (self->have_clear_frame)
            frame_mean_difference (transfer->buffer, self->clear_frame,
                                   &clear_difference);

          /* A moved finger can differ greatly from the captured placement.
           * Prefer positive evidence that the sensor returned to its known
           * clear state; retain the old comparison only when activation began
           * with a finger already present and no clear reference exists. */
          if ((self->have_clear_frame && clear_difference <= release_threshold) ||
              (!self->have_clear_frame &&
               captured_difference >= release_threshold))
            self->release_frames++;
          else
            self->release_frames = 0;

          fp_dbg ("finger-release captured difference %.4f, clear difference %.4f, threshold %.4f, frames %u/%u",
                  captured_difference, clear_difference, release_threshold,
                  self->release_frames, EGIS057E_RELEASE_FRAMES);
          if (self->release_frames >= EGIS057E_RELEASE_FRAMES)
            {
              fp_dbg ("captured finger no longer present; reporting removal");
              self->awaiting_release = FALSE;
              self->release_frames = 0;
              self->have_captured_frame = FALSE;
              fpi_image_device_report_finger_status (img_dev, FALSE);
            }
        }
      else
        {
          if (self->touch_pending)
            {
              self->touch_settle_frames++;
              if (mean_difference <= EGIS057E_TOUCH_STABLE_MAX_DIFFERENCE)
                {
                  Egis057eImageQuality quality = image_quality (transfer->buffer);

                  self->touch_stable_frames++;
                  if (!self->have_touch_best_frame ||
                      quality.score > self->touch_best_quality)
                    {
                      self->touch_best_difference = mean_difference;
                      self->touch_best_quality = quality.score;
                      memcpy (self->touch_best_frame, transfer->buffer,
                              EGIS057E_IMAGE_LEN);
                      self->have_touch_best_frame = TRUE;
                    }
                }
              else
                {
                  self->touch_stable_frames = 0;
                  self->touch_best_difference = G_MAXDOUBLE;
                  self->touch_best_quality = -1.0;
                  self->have_touch_best_frame = FALSE;
                }
              fp_dbg ("finger contact settling: activity %.4f, stable %u/%u, elapsed %u/%u",
                      mean_difference, self->touch_stable_frames,
                      EGIS057E_TOUCH_STABLE_FRAMES,
                      self->touch_settle_frames,
                      EGIS057E_TOUCH_SETTLE_MAX_FRAMES);
            }
          else if (mean_difference > self->change_threshold)
            self->finger_frames++;
          else
            {
              double baseline = self->change_threshold -
                                EGIS057E_FRAME_CHANGE_MARGIN;
              self->finger_frames = 0;
              baseline = baseline * 0.98 + mean_difference * 0.02;
              self->change_threshold = baseline +
                                       EGIS057E_FRAME_CHANGE_MARGIN;
            }

          if (self->finger_frames >= 2)
            {
              self->finger_frames = 0;
              self->touch_settle_frames = 0;
              self->touch_stable_frames = 0;
              self->touch_best_difference = G_MAXDOUBLE;
              self->touch_best_quality = -1.0;
              self->have_touch_best_frame = FALSE;
              self->touch_pending = TRUE;
              fp_dbg ("finger transition detected; waiting for stable contact");
            }
          else if (self->touch_pending &&
                   (self->touch_stable_frames >= EGIS057E_TOUCH_STABLE_FRAMES ||
                    self->touch_settle_frames >= EGIS057E_TOUCH_SETTLE_MAX_FRAMES))
            {
              g_autoptr(FpImage) img = fp_image_new (EGIS057E_IMAGE_WIDTH,
                                                     EGIS057E_IMAGE_HEIGHT);
              const guint8 *selected_frame = self->have_touch_best_frame ?
                                               self->touch_best_frame :
                                               transfer->buffer;

              if (self->touch_stable_frames < EGIS057E_TOUCH_STABLE_FRAMES)
                {
                  fp_dbg ("contact did not stabilize in %u frames; requesting retry",
                          self->touch_settle_frames);
                  memcpy (self->captured_frame, transfer->buffer,
                          EGIS057E_IMAGE_LEN);
                  self->have_captured_frame = TRUE;
                  self->awaiting_release = TRUE;
                  self->touch_pending = FALSE;
                  self->touch_settle_frames = 0;
                  self->touch_stable_frames = 0;
                  self->have_touch_best_frame = FALSE;
                  fpi_image_device_report_finger_status (img_dev, TRUE);
                  fpi_image_device_retry_scan (img_dev, FP_DEVICE_RETRY_TOO_FAST);
                  goto image_done;
                }
              memcpy (img->data, selected_frame, EGIS057E_IMAGE_LEN);
              fp_dbg ("contact settled; reporting best-quality stable frame (quality %.4f, difference %.4f)",
                      self->touch_best_quality, self->touch_best_difference);
              self->touch_pending = FALSE;
              self->finger_frames = 0;
              self->touch_settle_frames = 0;
              self->touch_stable_frames = 0;
              memcpy (self->captured_frame, selected_frame,
                      EGIS057E_IMAGE_LEN);
              self->have_touch_best_frame = FALSE;
              self->have_captured_frame = TRUE;
              fpi_image_device_report_finger_status (img_dev, TRUE);
              self->reported_image = TRUE;
              /* Require physical removal after every capture. fprintd may
               * chain an IDENTIFY duplicate check directly into ENROLL; a
               * synthetic finger-off would let enrollment calibrate while
               * the finger is still on the sensor. */
              self->awaiting_release = TRUE;
              fpi_image_device_image_captured (img_dev, g_steal_pointer (&img));
            }
        }
    }

image_done:
  fpi_ssm_next_state (transfer->ssm);
}

static void
send_packet (FpiSsm *ssm, FpDevice *dev, const Egis057ePacket *pkt)
{
  FpiUsbTransfer *t = fpi_usb_transfer_new (dev);
  guint8 *buf = g_memdup2 (pkt->bytes, pkt->len);

  fp_dbg ("sending %s", pkt->name);
  egis057e_log_bytes ("packet: ", pkt->bytes, pkt->len);

  fpi_usb_transfer_fill_bulk_full (t, EGIS057E_EP_OUT, buf, pkt->len,
                                   g_free);
  t->ssm = ssm;
  t->short_is_error = TRUE;
  fpi_usb_transfer_submit (t, EGIS057E_TIMEOUT_CMD, NULL,
                           fpi_ssm_usb_transfer_cb, NULL);
}

static void
recv_command_response (FpiSsm *ssm, FpDevice *dev, guint length)
{
  FpiUsbTransfer *t = fpi_usb_transfer_new (dev);

  fpi_usb_transfer_fill_bulk (t, EGIS057E_EP_IN, length);
  t->ssm = ssm;
  t->short_is_error = TRUE;
  fpi_usb_transfer_submit (t, EGIS057E_TIMEOUT_CMD, NULL, cmd_resp_cb,
                           GUINT_TO_POINTER (length));
}

static void
recv_image (FpiSsm *ssm, FpDevice *dev)
{
  FpiUsbTransfer *t = fpi_usb_transfer_new (dev);

  fpi_usb_transfer_fill_bulk (t, EGIS057E_EP_IN, EGIS057E_IMAGE_LEN);
  t->ssm = ssm;
  t->short_is_error = FALSE;
  fpi_usb_transfer_submit (t, EGIS057E_TIMEOUT_CMD, NULL, image_recv_cb, NULL);
}

static void
recv_calibration_image (FpiSsm *ssm, FpDevice *dev)
{
  FpiUsbTransfer *t = fpi_usb_transfer_new (dev);

  fpi_usb_transfer_fill_bulk (t, EGIS057E_EP_IN, EGIS057E_IMAGE_LEN);
  t->ssm = ssm;
  t->short_is_error = FALSE;
  fpi_usb_transfer_submit (t, EGIS057E_TIMEOUT_CMD, NULL,
                           calibration_image_cb, NULL);
}

static void
capture_delay_done (FpDevice *dev, gpointer user_data)
{
  FpiSsm *ssm = user_data;
  FpDeviceEgis057e *self = FPI_DEVICE_EGIS057E (dev);

  if (self->stop)
    {
      fpi_ssm_mark_completed (ssm);
      fpi_image_device_deactivate_complete (FP_IMAGE_DEVICE (dev), NULL);
      return;
    }

  fp_dbg ("polling retained image for automatic finger detection");
  fpi_ssm_jump_to_state (ssm, SM_CAPTURE_SEND);
}

/* -------------------------------------------------------------------------
 * Main state machine
 * ------------------------------------------------------------------------- */

static void
ssm_run_state (FpiSsm *ssm, FpDevice *dev)
{
  FpDeviceEgis057e *self    = FPI_DEVICE_EGIS057E (dev);
  FpImageDevice    *img_dev = FP_IMAGE_DEVICE (dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    /* --- Init phase ---------------------------------------------------- */

    case SM_INIT_SEND:
      if (self->init_step >= G_N_ELEMENTS (egis057e_init_pkts))
        {
          fp_dbg ("image-path init complete; starting automatic frame-change detection");
          if (!self->activated)
            {
              self->activated = TRUE;
              fpi_image_device_activate_complete (img_dev, NULL);
            }
          fpi_ssm_jump_to_state (ssm, SM_CAPTURE_DELAY);
          break;
        }
      if (egis057e_init_pkts[self->init_step].bytes[4] == 0x63 &&
          egis057e_init_pkts[self->init_step].bytes[5] == 0x33 &&
          self->have_calibration_sample)
        {
          Egis057ePacket apply = egis057e_init_pkts[self->init_step];

          apply.bytes[7] = self->calibration_sample;
          send_packet (ssm, dev, &apply);
        }
      else
        send_packet (ssm, dev, &egis057e_init_pkts[self->init_step]);
      break;

    case SM_INIT_RECV:
      if (egis057e_init_pkts[self->init_step].bytes[4] == 0x72)
        {
          recv_calibration_image (ssm, dev);
          break;
        }
      recv_command_response (ssm, dev,
                             egis057e_init_pkts[self->init_step].bytes[4] == 0x63 ||
                             egis057e_init_pkts[self->init_step].bytes[4] == 0x71
                             ? egis057e_init_pkts[self->init_step].len
                             : egis057e_init_pkts[self->init_step].bytes[4] == 0x62
                               ? EGIS057E_RESP_LEN + egis057e_init_pkts[self->init_step].bytes[6]
                               : EGIS057E_RESP_LEN);
      break;

    case SM_INIT_NEXT:
      self->init_step++;
      fpi_ssm_jump_to_state (ssm, SM_INIT_SEND);
      break;

    /* --- Detect / capture loop ----------------------------------------- */

    case SM_CAPTURE_DELAY:
      fpi_device_add_timeout (dev, 250, capture_delay_done, ssm, NULL);
      break;

    case SM_DETECT_WAIT:
      if (self->stop)
        {
          fp_dbg ("Stop requested; deactivating");
          fpi_ssm_mark_completed (ssm);
          fpi_image_device_deactivate_complete (img_dev, NULL);
          break;
        }
      {
        /*
         * Wait for a finger-present interrupt on EP 0x83.
         * The interrupt payload length is vendor-defined; 8 bytes is a safe
         * upper bound for ET5xx devices — adjust once confirmed.
         */
        FpiUsbTransfer *t = fpi_usb_transfer_new (dev);

        fp_dbg ("Waiting for finger interrupt on endpoint 0x%02x", self->int_ep);
        fpi_usb_transfer_fill_interrupt (t, self->int_ep, 16);
        t->ssm            = ssm;
        t->short_is_error = FALSE;
        fpi_usb_transfer_submit (t, EGIS057E_TIMEOUT_INT, NULL,
                                 int_finger_cb, NULL);
      }
      break;

    case SM_CAPTURE_SEND:
      if (self->capture_step >= G_N_ELEMENTS (egis057e_capture_pkts))
        {
          self->capture_step = 0;
          fpi_ssm_jump_to_state (ssm, SM_CAPTURE_REQUEST);
          break;
        }
      send_packet (ssm, dev, &egis057e_capture_pkts[self->capture_step]);
      break;

    case SM_CAPTURE_RECV:
      if (!egis057e_capture_pkts[self->capture_step].expect_response)
        {
          fpi_ssm_jump_to_state (ssm, SM_CAPTURE_NEXT);
          break;
        }
      recv_command_response (ssm, dev,
                             egis057e_capture_pkts[self->capture_step].bytes[4] == 0x63
                             ? egis057e_capture_pkts[self->capture_step].len
                             : EGIS057E_RESP_LEN);
      break;

    case SM_CAPTURE_NEXT:
      self->capture_step++;
      fpi_ssm_jump_to_state (ssm, SM_CAPTURE_SEND);
      break;

    case SM_CAPTURE_IMAGE:
      recv_image (ssm, dev);
      break;

    case SM_CAPTURE_REQUEST:
      {
        static const Egis057ePacket request = {
          { 'E', 'G', 'I', 'S', 0x64, 0x0f, 0x96 },
          7, "request live image", FALSE
        };
        send_packet (ssm, dev, &request);
      }
      break;

    case SM_CAPTURE_FINISH:
      {
        static const Egis057ePacket finish = {
          { 'E', 'G', 'I', 'S', 0x61, 0x2d, 0x20 },
          7, "finish live image", TRUE
        };
        send_packet (ssm, dev, &finish);
      }
      break;

    case SM_CAPTURE_FINISH_RECV:
      recv_command_response (ssm, dev, EGIS057E_RESP_LEN);
      break;

    case SM_DETECT_DONE:
      self->capture_step = 0;
      if (self->capture_protocol_error)
        {
          self->capture_protocol_error = FALSE;
          fpi_ssm_mark_failed (
            ssm,
            fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                      "Short live image; capture transaction closed"));
          break;
        }
      self->reported_image = FALSE;
      fpi_ssm_jump_to_state (ssm, SM_CAPTURE_DELAY);
      break;

    default:
      g_assert_not_reached ();
    }
}

/* -------------------------------------------------------------------------
 * SSM completion callback
 * ------------------------------------------------------------------------- */

static void
loop_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpImageDevice    *img_dev = FP_IMAGE_DEVICE (dev);
  FpDeviceEgis057e *self    = FPI_DEVICE_EGIS057E (dev);

  self->running = FALSE;

  if (error)
    {
      if (!self->activated)
        {
          self->activated = TRUE;
          fpi_image_device_activate_complete (img_dev, error);
        }
      else
        {
          fpi_image_device_session_error (img_dev, error);
        }
    }
}

/* -------------------------------------------------------------------------
 * Device lifecycle
 * ------------------------------------------------------------------------- */

static void
dev_init (FpImageDevice *dev)
{
  GError           *error = NULL;
  g_usb_device_claim_interface (fpi_device_get_usb_device (FP_DEVICE (dev)),
                                0, 0, &error);

  fpi_image_device_open_complete (dev, error);
}

static void
dev_deinit (FpImageDevice *dev)
{
  GError           *error = NULL;
  g_usb_device_release_interface (fpi_device_get_usb_device (FP_DEVICE (dev)),
                                   0, 0, &error);

  fpi_image_device_close_complete (dev, error);
}

static void
dev_activate (FpImageDevice *dev)
{
  FpDeviceEgis057e *self = FPI_DEVICE_EGIS057E (dev);
  FpiSsm           *ssm;

  self->stop      = FALSE;
  self->init_step = 0;
  self->activated = FALSE;
  self->int_ep    = EGIS057E_EP_INT_FINGER;
  self->have_previous_frame = FALSE;
  self->have_captured_frame = FALSE;
  self->have_clear_frame = FALSE;
  self->have_touch_best_frame = FALSE;
  self->detection_armed = FALSE;
  self->stable_frames = 0;
  self->finger_frames = 0;
  self->release_frames = 0;
  self->touch_settle_frames = 0;
  self->touch_stable_frames = 0;
  self->touch_pending = FALSE;
  self->baseline_sum = 0.0;
  self->change_threshold = 0.0;
  self->touch_best_difference = G_MAXDOUBLE;
  self->touch_best_quality = -1.0;
  self->calibration_sample = 0x6d;
  self->have_calibration_sample = FALSE;
  self->awaiting_release = FALSE;
  self->reported_image = FALSE;
  self->capture_protocol_error = FALSE;

  ssm = fpi_ssm_new (FP_DEVICE (dev), ssm_run_state, SM_STATES_NUM);
  fpi_ssm_start (ssm, loop_complete);
  self->running = TRUE;
}

static void
dev_deactivate (FpImageDevice *dev)
{
  FpDeviceEgis057e *self = FPI_DEVICE_EGIS057E (dev);

  if (self->running)
    self->stop = TRUE;
  else
    fpi_image_device_deactivate_complete (dev, NULL);
}

/* -------------------------------------------------------------------------
 * Small-area ridge matcher
 * ------------------------------------------------------------------------- */

static double
ridge_score (const guint8 *a, const guint8 *b)
{
  double best = -1.0;

  for (gint dy = -12; dy <= 12; dy++)
    for (gint dx = -12; dx <= 12; dx++)
      {
        double energy_a = 0, energy_b = 0, dot_product = 0;

        for (gint y = 4; y < EGIS057E_IMAGE_HEIGHT - 4; y++)
          for (gint x = 4; x < EGIS057E_IMAGE_WIDTH - 4; x++)
            {
              gint bx = x + dx;
              gint by = y + dy;
              double ax, ay, bx_gradient, by_gradient;

              if (bx < 4 || bx >= EGIS057E_IMAGE_WIDTH - 4 ||
                  by < 4 || by >= EGIS057E_IMAGE_HEIGHT - 4)
                continue;

              ax = (gint) a[y * EGIS057E_IMAGE_WIDTH + x + 1] -
                   (gint) a[y * EGIS057E_IMAGE_WIDTH + x - 1];
              ay = (gint) a[(y + 1) * EGIS057E_IMAGE_WIDTH + x] -
                   (gint) a[(y - 1) * EGIS057E_IMAGE_WIDTH + x];
              bx_gradient =
                (gint) b[by * EGIS057E_IMAGE_WIDTH + bx + 1] -
                (gint) b[by * EGIS057E_IMAGE_WIDTH + bx - 1];
              by_gradient =
                (gint) b[(by + 1) * EGIS057E_IMAGE_WIDTH + bx] -
                (gint) b[(by - 1) * EGIS057E_IMAGE_WIDTH + bx];
              dot_product += ax * bx_gradient + ay * by_gradient;
              energy_a += ax * ax + ay * ay;
              energy_b += bx_gradient * bx_gradient +
                          by_gradient * by_gradient;
            }

        if (energy_a > 0 && energy_b > 0)
          {
            double denominator = sqrt (energy_a * energy_b);

            best = MAX (best, dot_product / denominator);
          }
      }

  return best;
}

static gboolean
egis057e_enroll_image (FpImageDevice *dev, FpPrint *print,
                       FpImage *image, GError **error)
{
  g_autoptr(GVariant) old_data = NULL;
  Egis057eImageQuality quality = image_quality (image->data);
  GVariantBuilder builder;
  GVariantIter iter;
  GVariant *sample;

  fp_dbg ("enrollment image quality: stddev %.2f, gradient %.2f, range %u",
          quality.standard_deviation, quality.gradient_rms,
          quality.intensity_range);
  if (!image_quality_acceptable (&quality))
    {
      g_set_error (error, FP_DEVICE_RETRY, FP_DEVICE_RETRY_CENTER_FINGER,
                   "Fingerprint image has insufficient ridge detail; reposition and retry");
      return FALSE;
    }

  g_object_get (print, "fpi-data", &old_data, NULL);
  g_variant_builder_init (&builder, G_VARIANT_TYPE ("aay"));
  if (old_data)
    {
      g_variant_iter_init (&iter, old_data);
      while ((sample = g_variant_iter_next_value (&iter)))
        {
          gsize length = 0;
          const guint8 *bytes = g_variant_get_fixed_array (sample, &length, 1);

          if (length == EGIS057E_IMAGE_LEN &&
              ridge_score (bytes, image->data) >=
                EGIS057E_ENROLL_DUPLICATE_THRESHOLD)
            {
              g_variant_unref (sample);
              g_variant_builder_clear (&builder);
              g_set_error (error, FP_DEVICE_RETRY,
                           FP_DEVICE_RETRY_CENTER_FINGER,
                           "Enrollment image duplicates an earlier placement; reposition and retry");
              return FALSE;
            }
          g_variant_builder_add_value (&builder, sample);
          g_variant_unref (sample);
        }
    }
  g_variant_builder_add_value (&builder,
                               g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE,
                                                          image->data,
                                                          EGIS057E_IMAGE_LEN, 1));
  g_object_set (print, "fpi-data", g_variant_builder_end (&builder), NULL);
  return TRUE;
}

static FpiMatchResult
egis057e_match_image (FpImageDevice *dev, FpPrint *print,
                      FpImage *image, GError **error)
{
  g_autoptr(GVariant) data = NULL;
  Egis057eImageQuality quality = image_quality (image->data);
  GVariantIter iter;
  GVariant *sample;
  double best = -1.0;
  double second = -1.0;
  guint sample_index = 0;

  fp_dbg ("verification image quality: stddev %.2f, gradient %.2f, range %u",
          quality.standard_deviation, quality.gradient_rms,
          quality.intensity_range);
  if (!image_quality_acceptable (&quality))
    {
      g_set_error (error, FP_DEVICE_RETRY, FP_DEVICE_RETRY_CENTER_FINGER,
                   "Fingerprint image has insufficient ridge detail; reposition and retry");
      return FPI_MATCH_ERROR;
    }

  g_object_get (print, "fpi-data", &data, NULL);
  if (!data || !g_variant_is_of_type (data, G_VARIANT_TYPE ("aay")))
    {
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_DATA_INVALID,
                   "Invalid EH57E ridge template");
      return FPI_MATCH_ERROR;
    }

  g_variant_iter_init (&iter, data);
  while ((sample = g_variant_iter_next_value (&iter)))
    {
      gsize length = 0;
      const guint8 *bytes = g_variant_get_fixed_array (sample, &length, 1);
      if (length == EGIS057E_IMAGE_LEN)
        {
          double score = ridge_score (bytes, image->data);
          fp_dbg ("ridge template %u score %.4f", sample_index, score);
          if (score > best)
            {
              second = best;
              best = score;
            }
          else if (score > second)
            second = score;
        }
      sample_index++;
      g_variant_unref (sample);
    }

  fp_dbg ("small-area ridge scores best %.4f second %.4f (thresholds %.2f/%.2f)",
          best, second, EGIS057E_MATCH_THRESHOLD,
          EGIS057E_SECOND_MATCH_THRESHOLD);
  return best >= EGIS057E_MATCH_THRESHOLD &&
         second >= EGIS057E_SECOND_MATCH_THRESHOLD ?
         FPI_MATCH_SUCCESS : FPI_MATCH_FAIL;
}

/* -------------------------------------------------------------------------
 * Driver registration
 * ------------------------------------------------------------------------- */

static const FpIdEntry id_table[] = {
  { .vid = 0x1c7a, .pid = 0x057e, .driver_data = 0 },
  { .vid = 0,      .pid = 0,      .driver_data = 0 },
};

static void
fpi_device_egis057e_init (FpDeviceEgis057e *self)
{
}

static void
fpi_device_egis057e_class_init (FpDeviceEgis057eClass *klass)
{
  FpDeviceClass      *dev_class = FP_DEVICE_CLASS (klass);
  FpImageDeviceClass *img_class = FP_IMAGE_DEVICE_CLASS (klass);

  dev_class->id        = "egis057e";
  dev_class->full_name = "Egis Technology EH57E Touch Fingerprint Sensor";
  dev_class->type      = FP_DEVICE_TYPE_USB;
  dev_class->id_table  = id_table;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;
  dev_class->nr_enroll_stages = 5;
  dev_class->temp_hot_seconds = -1;

  img_class->img_open   = dev_init;
  img_class->img_close  = dev_deinit;
  img_class->activate   = dev_activate;
  img_class->deactivate = dev_deactivate;
  img_class->enroll_image = egis057e_enroll_image;
  img_class->match_image  = egis057e_match_image;

  /* Confirmed by the exact 0x0f96-byte payload: 70 * 57 = 3990. */
  img_class->img_width  = EGIS057E_IMAGE_WIDTH;
  img_class->img_height = EGIS057E_IMAGE_HEIGHT;
}
