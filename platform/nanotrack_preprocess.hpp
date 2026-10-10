/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "nanotrack_math.hpp"

namespace esp_vision {
namespace nanotrack {

struct SampleAxis {
    int low, high;
    float fraction;
};

// Both axes share the same mapping because the tracker samples square crops.
// Scratch storage belongs to the tracker, avoiding allocations on each frame.
template <typename PixelReader>
inline void resize_quantize(PixelReader pixel, int8_t *output, int size, float side_float,
                            float cx, float cy, float inverse_scale, bool half_up, SampleAxis *axis)
{
    int side = std::max(2, static_cast<int>(std::nearbyint(side_float)));
    int origin_x = static_cast<int>(std::floor(cx - (side + 1) * 0.5f + 0.5f));
    int origin_y = static_cast<int>(std::floor(cy - (side + 1) * 0.5f + 0.5f));
    float step = static_cast<float>(side) / size;
    for (int i = 0; i < size; ++i) {
        float position = std::max(0.0f, std::min(static_cast<float>(side - 1), (i + 0.5f) * step - 0.5f));
        int low = static_cast<int>(position);
        axis[i] = {low, std::min(low + 1, side - 1), position - low};
    }
    // Resizing produces a rounded uint8 pixel before model quantization.
    // Compute its 256 possible results once instead of for every channel.
    int8_t quantized[256];
    for (int i = 0; i < 256; ++i) {
        quantized[i] = quantize(i, inverse_scale, half_up);
    }
    for (int y = 0; y < size; ++y) {
        int y0 = origin_y + axis[y].low;
        int y1 = origin_y + axis[y].high;
        float fy = axis[y].fraction;
        for (int x = 0; x < size; ++x) {
            int x0 = origin_x + axis[x].low;
            int x1 = origin_x + axis[x].high;
            float fx = axis[x].fraction;
            float a[3], b[3], c[3], d[3];
            pixel(x0, y0, a);
            pixel(x1, y0, b);
            pixel(x0, y1, c);
            pixel(x1, y1, d);
            for (int channel = 0; channel < 3; ++channel) {
                float value = ((1 - fx) * a[channel] + fx * b[channel]) * (1 - fy)
                              + ((1 - fx) * c[channel] + fx * d[channel]) * fy;
                *output++ = quantized[static_cast<int>(std::nearbyint(value))];
            }
        }
    }
}

} // namespace nanotrack
} // namespace esp_vision
