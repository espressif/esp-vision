/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

// ESP-VISION user module qstrs that are only exposed by board feature flags.
// *FORMAT-OFF*

// image.find_qrcodes() result object
Q(payload)
Q(ecc_level)
Q(data_type)
Q(eci)
Q(is_numeric)
Q(is_alphanumeric)
Q(is_binary)
Q(is_kanji)
Q(qrcode)

// image.find_apriltags() result object and keyword arguments
Q(TAG16H5)
Q(TAG25H7)
Q(TAG25H9)
Q(TAG36H10)
Q(TAG36H11)
Q(apriltag)
Q(decision_margin)
Q(families)
Q(family)
Q(fx)
Q(fy)
Q(goodness)
Q(hamming)
Q(pose)
Q(x_rotation)
Q(x_translation)
Q(y_rotation)
Q(y_translation)
Q(z_rotation)
Q(z_translation)

// image.find_barcodes() result object and barcode type constants
Q(barcode)
Q(EAN2)
Q(EAN5)
Q(EAN8)
Q(UPCE)
Q(ISBN10)
Q(UPCA)
Q(EAN13)
Q(ISBN13)
Q(I25)
Q(DATABAR)
Q(DATABAR_EXP)
Q(CODABAR)
Q(CODE39)
Q(PDF417)
Q(CODE93)
Q(CODE128)
