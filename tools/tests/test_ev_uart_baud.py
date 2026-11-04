# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

import pathlib
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


class UartBaudTest(unittest.TestCase):
    def test_high_speed_ingress_drains_burst_without_starving_usb(self):
        compiler = shutil.which("cc") or shutil.which("gcc")
        if compiler is None:
            self.skipTest("native C compiler unavailable")
        source = (ROOT / "platform/ev_control_transport.c").read_text()
        start = source.index("static void ev_control_transport_pump(void)\n{")
        end = source.index("\nstatic void ev_control_transport_task(", start)
        defines = "\n".join(re.findall(
            r"^#define EV_CONTROL_(?:RX_CHUNK_MAX|UART_RX_CHUNKS) .*", source, re.MULTILINE))
        prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#define MICROPY_HW_USB_CDC 0
#define EV_TRANSPORT_PHYSICAL_SLOT 0
typedef enum {EV_TRANSPORT_USJ, EV_TRANSPORT_USB, EV_TRANSPORT_UART} ev_transport_id_t;
typedef struct {ev_transport_id_t transport_id;} ev_link_ref_t;
typedef enum {EV_IO_PROGRESS, EV_IO_WOULD_BLOCK} ev_io_status_t;
typedef struct {ev_io_status_t status; size_t transferred;} ev_io_result_t;
static size_t remaining[3], received[3], expired[3];
static void ev_control_transport_uart_baud_poll(void) {}
static void ev_control_transport_poll_serial(void) {}
static bool ev_transport_link_ref(ev_transport_id_t id, int slot, ev_link_ref_t *link) {
    (void)slot; link->transport_id = id; return true;
}
static ev_io_result_t ev_control_transport_read_link(const ev_link_ref_t *link, void *data, size_t capacity) {
    (void)data;
    size_t size = remaining[link->transport_id];
    if (size > capacity) size = capacity;
    remaining[link->transport_id] -= size;
    return (ev_io_result_t){size ? EV_IO_PROGRESS : EV_IO_WOULD_BLOCK, size};
}
static int ev_control_transport_rx(const ev_link_ref_t *link, const void *data, size_t size) {
    received[link->transport_id] += size;
    if (data == NULL) ++expired[link->transport_id];
    return 0;
}
'''
        main = (ROOT / "tools/tests/ev_uart_rx_pump_test.c").read_text()
        with tempfile.TemporaryDirectory(prefix="ev-uart-rx-") as directory:
            generated = pathlib.Path(directory) / "pump.c"
            binary = pathlib.Path(directory) / "pump_test"
            generated.write_text(prelude + defines + "\n" + source[start:end] + main)
            subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                            str(generated), "-o", str(binary)], check=True, cwd=ROOT)
            subprocess.run([str(binary)], check=True, cwd=ROOT)

    def test_native_transition_failures_and_recovery(self):
        compiler = shutil.which("cc") or shutil.which("gcc")
        if compiler is None:
            self.skipTest("native C compiler unavailable")
        with tempfile.TemporaryDirectory(prefix="ev-uart-baud-") as directory:
            binary = pathlib.Path(directory) / "ev_uart_baud_test"
            subprocess.run(
                [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                 f"-I{ROOT / 'platform'}", str(ROOT / "tools/tests/ev_uart_baud_test.c"),
                 "-o", str(binary)], check=True, cwd=ROOT,
            )
            subprocess.run([str(binary)], check=True, cwd=ROOT)


if __name__ == "__main__":
    unittest.main()
