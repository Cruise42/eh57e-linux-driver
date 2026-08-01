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

#define EGIS057E_MATCH_THRESHOLD 0.34
#define EGIS057E_SECOND_MATCH_THRESHOLD 0.27
#define EGIS057E_FRAME_CHANGE_MARGIN 0.10
#define EGIS057E_SETTLING_FRAMES 8
#define EGIS057E_RECOVERY_PKT_COUNT 4

/* -------------------------------------------------------------------------
 * Device state
 * ------------------------------------------------------------------------- */

struct _FpDeviceEgis057e
{
  FpImageDevice parent;

  gboolean      running;
  gboolean      stop;
  gboolean      activated;
  gboolean      image_initialized;

  guint         init_step;        /* current index in egis057e_init[]   */
  guint         capture_step;     /* current index in capture sequence  */
  guint8        int_ep;           /* interrupt endpoint currently polled */
  guint8        previous_frame[EGIS057E_IMAGE_LEN];
  gboolean      have_previous_frame;
  gboolean      detection_armed;
  guint         stable_frames;
  guint         finger_frames;
  guint         clear_frames;
  guint         touch_settle_frames;
  gboolean      touch_pending;
  double        baseline_sum;
  double        change_threshold;
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
      guint64 difference = 0;
      double mean_difference;

      for (guint i = 0; i < EGIS057E_IMAGE_LEN; i++)
        difference += ABS ((gint) transfer->buffer[i] -
                           (gint) self->previous_frame[i]);
      mean_difference = (double) difference / EGIS057E_IMAGE_LEN;
      memcpy (self->previous_frame, transfer->buffer, EGIS057E_IMAGE_LEN);
      if (!self->detection_armed)
        {
          self->baseline_sum += mean_difference;
          self->stable_frames++;
          if (self->stable_frames >= EGIS057E_SETTLING_FRAMES)
            {
              self->detection_armed = TRUE;
              self->change_threshold =
                self->baseline_sum / self->stable_frames +
                EGIS057E_FRAME_CHANGE_MARGIN;
              fp_dbg ("automatic finger detection armed: clear baseline %.4f, threshold %.4f",
                      self->baseline_sum / self->stable_frames,
                      self->change_threshold);
            }
        }
      else if (self->awaiting_release)
        {
          if (mean_difference <= self->change_threshold)
            self->clear_frames++;
          else
            self->clear_frames = 0;

          if (self->clear_frames >= 3)
            {
              fp_dbg ("temporal activity stayed below threshold; finger removed");
              self->awaiting_release = FALSE;
              self->clear_frames = 0;
              fpi_image_device_report_finger_status (img_dev, FALSE);
            }
        }
      else
        {
          if (self->touch_pending)
            {
              if (self->touch_settle_frames > 0)
                self->touch_settle_frames--;
              fp_dbg ("finger contact settling, %u frames remaining",
                      self->touch_settle_frames);
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
              self->touch_settle_frames = 3;
              self->touch_pending = TRUE;
              fp_dbg ("finger transition detected; waiting for stable contact");
            }
          else if (self->touch_pending && self->touch_settle_frames == 0)
            {
              g_autoptr(FpImage) img = fp_image_new (EGIS057E_IMAGE_WIDTH,
                                                     EGIS057E_IMAGE_HEIGHT);
              memcpy (img->data, transfer->buffer, EGIS057E_IMAGE_LEN);
              fp_dbg ("temporal activity exceeded threshold; reporting automatic finger placement");
              self->touch_pending = FALSE;
              self->finger_frames = 0;
              fpi_image_device_report_finger_status (img_dev, TRUE);
              self->reported_image = TRUE;
              self->awaiting_release =
                fpi_device_get_current_action (dev) == FPI_DEVICE_ACTION_ENROLL;
              fpi_image_device_image_captured (img_dev, g_steal_pointer (&img));
            }
        }
    }

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
      if (self->image_initialized &&
          self->init_step == EGIS057E_RECOVERY_PKT_COUNT)
        {
          fp_dbg ("recovery complete; reusing calibrated image path");
          if (!self->activated)
            {
              self->activated = TRUE;
              fpi_image_device_activate_complete (img_dev, NULL);
            }
          fpi_ssm_jump_to_state (ssm, SM_CAPTURE_DELAY);
          break;
        }
      if (self->init_step >= G_N_ELEMENTS (egis057e_init_pkts))
        {
          fp_dbg ("image-path init complete; starting automatic frame-change detection");
          self->image_initialized = TRUE;
          if (!self->activated)
            {
              self->activated = TRUE;
              fpi_image_device_activate_complete (img_dev, NULL);
            }
          fpi_ssm_jump_to_state (ssm, SM_CAPTURE_DELAY);
          break;
        }
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
      if (self->reported_image &&
          fpi_device_get_current_action (dev) != FPI_DEVICE_ACTION_ENROLL)
        fpi_image_device_report_finger_status (img_dev, FALSE);
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
      self->image_initialized = FALSE;
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
  FpDeviceEgis057e *self  = FPI_DEVICE_EGIS057E (dev);

  g_usb_device_claim_interface (fpi_device_get_usb_device (FP_DEVICE (dev)),
                                0, 0, &error);

  self->image_initialized = FALSE;

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
  self->detection_armed = FALSE;
  self->stable_frames = 0;
  self->finger_frames = 0;
  self->clear_frames = 0;
  self->touch_settle_frames = 0;
  self->touch_pending = FALSE;
  self->baseline_sum = 0.0;
  self->change_threshold = 0.0;
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
        double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
        guint count = 0;

        for (gint y = 4; y < EGIS057E_IMAGE_HEIGHT - 4; y++)
          for (gint x = 4; x < EGIS057E_IMAGE_WIDTH - 4; x++)
            {
              gint bx = x + dx;
              gint by = y + dy;
              double va, vb;

              if (bx < 4 || bx >= EGIS057E_IMAGE_WIDTH - 4 ||
                  by < 4 || by >= EGIS057E_IMAGE_HEIGHT - 4)
                continue;

              va = (gint) a[y * EGIS057E_IMAGE_WIDTH + x + 1] -
                   (gint) a[y * EGIS057E_IMAGE_WIDTH + x - 1] +
                   (gint) a[(y + 1) * EGIS057E_IMAGE_WIDTH + x] -
                   (gint) a[(y - 1) * EGIS057E_IMAGE_WIDTH + x];
              vb = (gint) b[by * EGIS057E_IMAGE_WIDTH + bx + 1] -
                   (gint) b[by * EGIS057E_IMAGE_WIDTH + bx - 1] +
                   (gint) b[(by + 1) * EGIS057E_IMAGE_WIDTH + bx] -
                   (gint) b[(by - 1) * EGIS057E_IMAGE_WIDTH + bx];
              sa += va; sb += vb; saa += va * va; sbb += vb * vb;
              sab += va * vb; count++;
            }

        if (count)
          {
            double numerator = sab - sa * sb / count;
            double denominator = sqrt ((saa - sa * sa / count) *
                                       (sbb - sb * sb / count));
            if (denominator > 0)
              best = MAX (best, numerator / denominator);
          }
      }

  return best;
}

static gboolean
egis057e_enroll_image (FpImageDevice *dev, FpPrint *print,
                       FpImage *image, GError **error)
{
  g_autoptr(GVariant) old_data = NULL;
  GVariantBuilder builder;
  GVariantIter iter;
  GVariant *sample;

  g_object_get (print, "fpi-data", &old_data, NULL);
  g_variant_builder_init (&builder, G_VARIANT_TYPE ("aay"));
  if (old_data)
    {
      g_variant_iter_init (&iter, old_data);
      while ((sample = g_variant_iter_next_value (&iter)))
        g_variant_builder_add_value (&builder, sample);
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
  GVariantIter iter;
  GVariant *sample;
  double best = -1.0;
  double second = -1.0;
  guint sample_index = 0;

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
