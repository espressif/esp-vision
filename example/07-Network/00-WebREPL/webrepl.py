# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

# Traditional MicroPython WebREPL is not supported by the ESP-VISION EV-MUX
# runtime. Its dupterm terminal and binary file protocol do not use EV-MUX's
# single-owner session, lease and VM-epoch checks. Do not install the upstream
# service or mirror raw terminal input as a workaround. Network control uses
# Device Core's TCP transport with the same session authorization as USB/UART.
# This compatibility entry exits before importing network or opening a listener.
raise RuntimeError(
    "Traditional WebREPL is not supported by ESP-VISION EV-MUX; "
    "use Device Core over TCP for network control."
)
