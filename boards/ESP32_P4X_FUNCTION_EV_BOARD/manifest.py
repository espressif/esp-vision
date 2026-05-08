# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

freeze("$(PORT_DIR)/modules")
include("$(MPY_DIR)/extmod/asyncio")
require("umqtt.simple")
require("umqtt.robust")
