/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>

void test_reg_set_bit(uint32_t reg, uint32_t mask);
void test_reg_clear_bit(uint32_t reg, uint32_t mask);

#define REG_SET_BIT(reg, mask) test_reg_set_bit((reg), (mask))
#define REG_CLR_BIT(reg, mask) test_reg_clear_bit((reg), (mask))
