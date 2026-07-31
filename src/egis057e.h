/*
 * Egis Technology EH57E / 057E libfprint prototype.
 *
 * USB ID: 1c7a:057e "EgisTec Touch Fingerprint Sensor"
 *
 * Confirmed endpoint layout from descriptors:
 *   bulk OUT      0x01
 *   bulk IN       0x82
 *   interrupt IN  0x83
 *   interrupt IN  0x84
 *
 * Important capture note:
 *   The previously extracted 53-step "RTCR" stream was later traced to the
 *   Realtek RTS5129 card reader (0bda:0129), not this fingerprint reader.
 *   Do not use that stream for EH57E initialization.
 *
 * SPDX-License-Identifier: 0BSD
 * Confirmed image geometry is 70x57 grayscale pixels (0x0f96 bytes).
 */

#ifndef __EGIS057E_H
#define __EGIS057E_H 1

#include "drivers_api.h"

#define EGIS057E_EP_OUT        0x01
#define EGIS057E_EP_IN         0x82
#define EGIS057E_EP_INT_FINGER 0x83
#define EGIS057E_EP_INT_ALT    0x84

#define EGIS057E_TIMEOUT_CMD   5000
#define EGIS057E_TIMEOUT_INT   1000
#define EGIS057E_RESP_LEN      7

/*
 * Static analysis and hardware captures confirmed an image payload length of
 * 0xf96 bytes and a 70x57 sensor frame.
 */
#define EGIS057E_IMAGE_LEN      0x0f96
#define EGIS057E_IMAGE_WIDTH    70
#define EGIS057E_IMAGE_HEIGHT   57

#define EGIS057E_PKT_LEN        7
#define EGIS057E_MAX_PKT_LEN    32

#endif /* __EGIS057E_H */
