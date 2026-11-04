// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0

export const OBSERVED_CAMERA_FEATURE = 'debug.capture_frame.observed.v1'

/** Older capture_frame implementations can overwrite an application's Image. */
export function requireObservedCameraCapture(session) {
  if (!session.hasFeature?.('debug.capture_frame') || !session.hasFeature?.(OBSERVED_CAMERA_FEATURE)) {
    throw Object.assign(new Error(`Safe camera observation requires firmware feature ${OBSERVED_CAMERA_FEATURE}; update ESP-VISION firmware before capturing camera frames`), { code: 'UNSUPPORTED_OPERATION' })
  }
}
