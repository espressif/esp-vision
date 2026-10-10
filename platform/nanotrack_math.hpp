/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace esp_vision {
namespace nanotrack {

struct Box {
    float x, y, w, h, score;
};

struct State {
    float cx, cy, w, h;
};

inline float extent(float w, float h)
{
    float pad = (w + h) * 0.5f;
    return std::sqrt((w + pad) * (h + pad));
}

inline int8_t quantize(float value, float inverse_scale, bool half_up = false)
{
    float q = half_up ? std::floor(value * inverse_scale + 0.5f) : std::nearbyint(value * inverse_scale);
    return static_cast<int8_t>(std::max(-128.0f, std::min(127.0f, q)));
}

// Class and box tensors use ESP-DL's NHWC layout, not the ONNX NCHW layout.
// On rejection, leave the previous search center and size unchanged.
inline bool decode(const int8_t *cls, float cls_scale, const int8_t *loc, float loc_scale,
                   const float *window, int width, int height, float threshold,
                   State &state, Box &box)
{
    const float scale = 127.0f / extent(state.w, state.h);
    float best = -1.0f;
    float next_cx = 0, next_cy = 0, next_w = 0, next_h = 0, confidence = 0, rate = 0;
    for (int i = 0; i < 256; ++i) {
        float score = 1.0f / (1.0f + std::exp((cls[2 * i] - cls[2 * i + 1]) * cls_scale));
        float left = loc[4 * i] * loc_scale;
        float top = loc[4 * i + 1] * loc_scale;
        float right = loc[4 * i + 2] * loc_scale;
        float bottom = loc[4 * i + 3] * loc_scale;
        if ((left < 0) || (top < 0) || (right < 0) || (bottom < 0)
                || (left + right <= 0) || (top + bottom <= 0)) {
            continue;
        }
        float w = left + right;
        float h = top + bottom;
        float size_change = extent(w, h) / (extent(state.w, state.h) * scale);
        float ratio_change = (state.w / state.h) / (w / h);
        size_change = std::max(size_change, 1.0f / size_change);
        ratio_change = std::max(ratio_change, 1.0f / ratio_change);
        float penalty = std::exp(-(size_change * ratio_change - 1.0f) * 0.150f);
        float weighted = penalty * score * (1.0f - 0.490f) + window[i] * 0.490f;
        if (weighted > best) {
            best = weighted;
            confidence = score;
            rate = penalty * score * 0.385f;
            next_cx = state.cx + ((i % 16 - 8) * 16 + (right - left) * 0.5f) / scale;
            next_cy = state.cy + ((i / 16 - 8) * 16 + (bottom - top) * 0.5f) / scale;
            next_w = w / scale;
            next_h = h / scale;
        }
    }
    if ((best < 0) || (confidence < threshold)) {
        return false;
    }
    state.cx = std::max(0.0f, std::min(static_cast<float>(width), next_cx));
    state.cy = std::max(0.0f, std::min(static_cast<float>(height), next_cy));
    state.w = std::max(10.0f, std::min(static_cast<float>(width), state.w * (1 - rate) + next_w * rate));
    state.h = std::max(10.0f, std::min(static_cast<float>(height), state.h * (1 - rate) + next_h * rate));
    // Clip the returned rectangle, but preserve the continuous tracking state.
    box.x = std::max(0.0f, state.cx - state.w * 0.5f);
    box.y = std::max(0.0f, state.cy - state.h * 0.5f);
    box.w = std::min(static_cast<float>(width), state.cx + state.w * 0.5f) - box.x;
    box.h = std::min(static_cast<float>(height), state.cy + state.h * 0.5f) - box.y;
    box.score = confidence;
    return true;
}

} // namespace nanotrack
} // namespace esp_vision
