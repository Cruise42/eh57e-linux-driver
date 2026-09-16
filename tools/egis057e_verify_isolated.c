/* SPDX-License-Identifier: 0BSD */
/* Diagnostic only: opens USB directly, reads a template, reports a decision.
 * Does not call PAM, unlock a session, modify templates, or save images. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fprint.h>

typedef struct
{
  GCancellable *cancel;
  gboolean reported;
  gboolean matched;
  gboolean timed_out;
} Trial;

static gboolean
cancel_trial (gpointer data)
{
  Trial *trial = data;
  trial->timed_out = TRUE;
  g_cancellable_cancel (trial->cancel);
  return G_SOURCE_CONTINUE;
}

static void
report (FpDevice *device, FpPrint *match, FpPrint *print,
        gpointer data, GError *error)
{
  Trial *trial = data;
  (void) device;
  (void) print;
  trial->reported = TRUE;
  trial->matched = match != NULL && error == NULL;
  g_print ("Diagnostic result: %s%s\n",
           error ? "RETRY: " : trial->matched ? "MATCH" : "NO MATCH",
           error ? error->message : "");
  /* Mirror a fprintd client stopping after the early match report. This
   * also lets repeated trials exercise a still-held contact across actions. */
  g_cancellable_cancel (trial->cancel);
}

static void
driver_log (const gchar *domain, GLogLevelFlags level,
            const gchar *message, gpointer data)
{
  (void) domain; (void) level; (void) data;
  if (strstr (message, "ridge template") ||
      strstr (message, "small-area ridge scores") ||
      strstr (message, "verification image quality") ||
      strstr (message, "runtime image calibration") ||
      strstr (message, "verification restarted") ||
      strstr (message, "captured finger no longer") ||
      strstr (message, "initial retained-frame quality"))
    g_print ("%s\n", message);
}

int
main (int argc, char **argv)
{
  g_autoptr(GError) error = NULL;
  g_autofree gchar *bytes = NULL;
  gsize length = 0;
  g_autoptr(FpPrint) template = NULL;
  g_autoptr(FpContext) context = NULL;
  FpDevice *device = NULL;
  guint attempts = 1;
  int result = EXIT_SUCCESS;

  if (argc < 2 || argc > 3 ||
      (argc == 3 && !g_ascii_string_to_unsigned (argv[2], 10, 1, 3,
                                                &(guint64){0}, NULL)))
    {
      g_printerr ("Usage: %s TEMPLATE [1-3 attempts]\n", argv[0]);
      return EXIT_FAILURE;
    }
  if (argc == 3)
    attempts = (guint) atoi (argv[2]);
  g_log_set_handler ("libfprint-egis057e", G_LOG_LEVEL_DEBUG, driver_log, NULL);
  if (!g_file_get_contents (argv[1], &bytes, &length, &error))
    goto failed;
  template = fp_print_deserialize ((guchar *) bytes, length, &error);
  if (!template)
    goto failed;
  context = fp_context_new ();
  fp_context_enumerate (context);
  GPtrArray *devices = fp_context_get_devices (context);
  for (guint i = 0; i < devices->len; i++)
    if (g_strcmp0 (fp_device_get_driver (g_ptr_array_index (devices, i)),
                   "egis057e") == 0)
      {
        device = g_ptr_array_index (devices, i);
        break;
      }
  if (!device || !fp_print_compatible (template, device))
    {
      g_printerr ("No compatible EH57E device/template\n");
      return EXIT_FAILURE;
    }
  if (!fp_device_open_sync (device, NULL, &error))
    goto failed;
  g_print ("ISOLATED TEST: results grant no system access; no images are saved.\n");
  for (guint i = 0; i < attempts; i++)
    {
      g_autoptr(GCancellable) cancel = g_cancellable_new ();
      Trial trial = { .cancel = cancel };
      gboolean matched = FALSE;
      guint timer = g_timeout_add_seconds (60, cancel_trial, &trial);
      g_print ("Trial %u/%u started (60-second limit).\n", i + 1, attempts);
      fp_device_verify_sync (device, template, cancel, report, &trial,
                             &matched, NULL, &error);
      g_source_remove (timer);
      if (error && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          g_printerr ("Verification error: %s\n", error->message);
          result = EXIT_FAILURE;
        }
      g_clear_error (&error);
      if (trial.timed_out || !trial.reported)
        {
          g_print ("No decision: %s\n", trial.timed_out ? "timed out" : "cancelled");
          result = EXIT_FAILURE;
          break;
        }
    }
  if (!fp_device_close_sync (device, NULL, &error))
    goto failed;
  /* Exit status describes test execution, never authentication success. */
  return result;
failed:
  g_printerr ("Test error: %s\n", error ? error->message : "unknown error");
  return EXIT_FAILURE;
}
