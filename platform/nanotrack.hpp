/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "nanotrack_math.hpp"
#include "nanotrack_preprocess.hpp"

namespace dl {
class Model;
class TensorBase;
}

extern "C" {
    typedef struct image image_t;
    typedef struct rectangle rectangle_t;
}

namespace esp_vision {

// Model ownership stays with the binding. Template model is needed only by init().
class NanoTrack {
public:
    NanoTrack(dl::Model *search, dl::Model *head);
    bool valid() const
    {
        return m_valid;
    }
    bool ready() const
    {
        return m_ready;
    }
    esp_err_t init(const image_t *image, const rectangle_t &roi, dl::Model *model);
    esp_err_t track(const image_t *image, float threshold, nanotrack::Box &box, bool &found);

private:
    dl::Model *m_search;
    dl::Model *m_head;
    dl::TensorBase *m_search_input;
    dl::TensorBase *m_search_output;
    dl::TensorBase *m_head_template;
    dl::TensorBase *m_head_search;
    dl::TensorBase *m_cls;
    dl::TensorBase *m_loc;
    bool m_valid = false;
    bool m_ready = false;
    int m_width = 0;
    int m_height = 0;
    nanotrack::State m_state = {};
    float m_padding[3] = {};
    float m_window[256];
    int8_t m_template[8 * 8 * 48];
    nanotrack::SampleAxis m_sample_axis[255];

    void pixel(const image_t *image, int x, int y, float *bgr) const;
    void preprocess(const image_t *image, dl::TensorBase *input, int size, float side);
};

} // namespace esp_vision
