// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
//
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cstring>
#include <string>

extern "C" {
#include "py/misc.h"
}

#include "esp_vision_zxing.h"

#include "Barcode.h"
#include "BarcodeFormat.h"
#include "ImageView.h"
#include "ReadBarcode.h"
#include "ReaderOptions.h"

static uint16_t esp_vision_zxing_barcode_type(ZXing::BarcodeFormat format)
{
    switch (format) {
    case ZXing::BarcodeFormat::EAN8:
        return BARCODE_EAN8;
    case ZXing::BarcodeFormat::UPCE:
        return BARCODE_UPCE;
    case ZXing::BarcodeFormat::UPCA:
        return BARCODE_UPCA;
    case ZXing::BarcodeFormat::EAN13:
        return BARCODE_EAN13;
    case ZXing::BarcodeFormat::ISBN:
        return BARCODE_ISBN13;
    case ZXing::BarcodeFormat::ITF:
    case ZXing::BarcodeFormat::ITF14:
        return BARCODE_I25;
    case ZXing::BarcodeFormat::DataBar:
    case ZXing::BarcodeFormat::DataBarOmni:
    case ZXing::BarcodeFormat::DataBarStk:
    case ZXing::BarcodeFormat::DataBarStkOmni:
    case ZXing::BarcodeFormat::DataBarLtd:
        return BARCODE_DATABAR;
    case ZXing::BarcodeFormat::DataBarExp:
    case ZXing::BarcodeFormat::DataBarExpStk:
        return BARCODE_DATABAR_EXP;
    case ZXing::BarcodeFormat::Codabar:
        return BARCODE_CODABAR;
    case ZXing::BarcodeFormat::Code39:
    case ZXing::BarcodeFormat::Code39Std:
    case ZXing::BarcodeFormat::Code39Ext:
    case ZXing::BarcodeFormat::Code32:
    case ZXing::BarcodeFormat::PZN:
        return BARCODE_CODE39;
    case ZXing::BarcodeFormat::Code93:
        return BARCODE_CODE93;
    case ZXing::BarcodeFormat::Code128:
        return BARCODE_CODE128;
    default:
        return BARCODE_CODE128;
    }
}

static int esp_vision_zxing_min_x(const ZXing::Position &position)
{
    int value = position[0].x;
    for (int i = 1; i < 4; i++) {
        value = std::min(value, position[i].x);
    }
    return value;
}

static int esp_vision_zxing_min_y(const ZXing::Position &position)
{
    int value = position[0].y;
    for (int i = 1; i < 4; i++) {
        value = std::min(value, position[i].y);
    }
    return value;
}

static int esp_vision_zxing_max_x(const ZXing::Position &position)
{
    int value = position[0].x;
    for (int i = 1; i < 4; i++) {
        value = std::max(value, position[i].x);
    }
    return value;
}

static int esp_vision_zxing_max_y(const ZXing::Position &position)
{
    int value = position[0].y;
    for (int i = 1; i < 4; i++) {
        value = std::max(value, position[i].y);
    }
    return value;
}

void esp_vision_zxing_find_barcodes(list_t *out, image_t *img, rectangle_t *roi)
{
    list_init(out, sizeof(find_barcodes_list_lnk_data_t));

    if (!IM_IS_GS(img) || roi->w <= 0 || roi->h <= 0) {
        return;
    }

    const uint8_t *data = IMAGE_COMPUTE_GRAYSCALE_PIXEL_ROW_PTR(img, roi->y) + roi->x;
    ZXing::ImageView view(data, roi->w, roi->h, ZXing::ImageFormat::Lum, img->w);

    ZXing::ReaderOptions options;
    // Restrict to the symbologies we actually need. AllLinear also enables
    // ITF and Codabar, which lack strong start/stop guards and produce frequent
    // false positives (e.g. short ITF reads from unrelated bar patterns).
    options.setFormats(ZXing::BarcodeFormat::Code128 | ZXing::BarcodeFormat::EANUPC);
    options.setTryHarder(true);
    options.setTryRotate(true);
    options.setTryInvert(true);
    options.setTryDownscale(false);
    options.setMaxNumberOfSymbols(8);

    ZXing::Barcodes barcodes;
    try {
        barcodes = ZXing::ReadBarcodes(view, options);
    } catch (...) {
        return;
    }

    for (const ZXing::Barcode &barcode : barcodes) {
        if (!barcode.isValid()) {
            continue;
        }

        const ZXing::Position &position = barcode.position();
        std::string text = barcode.text();

        find_barcodes_list_lnk_data_t lnk_data = {};
        for (int i = 0; i < 4; i++) {
            lnk_data.corners[i].x = roi->x + position[i].x;
            lnk_data.corners[i].y = roi->y + position[i].y;
        }

        int min_x = esp_vision_zxing_min_x(position);
        int min_y = esp_vision_zxing_min_y(position);
        int max_x = esp_vision_zxing_max_x(position);
        int max_y = esp_vision_zxing_max_y(position);
        lnk_data.rect.x = roi->x + min_x;
        lnk_data.rect.y = roi->y + min_y;
        lnk_data.rect.w = std::max(0, max_x - min_x + 1);
        lnk_data.rect.h = std::max(0, max_y - min_y + 1);

        lnk_data.payload_len = text.size();
        lnk_data.payload = static_cast<char *>(m_malloc_maybe(lnk_data.payload_len + 1));
        if (lnk_data.payload == nullptr) {
            continue;
        }

        memcpy(lnk_data.payload, text.data(), lnk_data.payload_len);
        lnk_data.payload[lnk_data.payload_len] = '\0';
        lnk_data.type = esp_vision_zxing_barcode_type(barcode.format());
        // orientation() is signed degrees in [-180, 180]; normalize to [0, 360)
        // before storing into the unsigned rotation field.
        int orientation = barcode.orientation() % 360;
        if (orientation < 0) {
            orientation += 360;
        }
        lnk_data.rotation = static_cast<uint16_t>(orientation);
        lnk_data.quality = barcode.lineCount();

        list_push_back(out, &lnk_data);
    }
}
