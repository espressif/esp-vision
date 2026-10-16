# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Compile the production USB-bus CDC generation/ring helpers natively."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
CDC_SOURCE = ROOT / "overlay/micropython/shared/tinyusb/mp_usbd_cdc.c"
CONTROL_SOURCE = ROOT / "platform/ev_control_transport.c"


class CdcRxGenerationTests(unittest.TestCase):
    def test_native_generation_and_pending_fifo_cases(self) -> None:
        compiler = shutil.which("cc") or shutil.which("gcc")
        if compiler is None:
            self.skipTest("a native C compiler is unavailable")

        with tempfile.TemporaryDirectory(prefix="ev-cdc-rx-") as temporary:
            executable = Path(temporary) / "ev_cdc_rx_test"
            subprocess.run([
                compiler,
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{ROOT / 'platform'}",
                str(ROOT / "tools/tests/ev_cdc_rx_test.c"),
                "-o",
                str(executable),
            ], check=True, cwd=ROOT)
            subprocess.run([str(executable)], check=True, cwd=ROOT)

    def test_production_cdc_lifecycle_uses_usb_mount_not_dtr(self) -> None:
        cdc = CDC_SOURCE.read_text(encoding="utf-8")
        mount_start = cdc.index("void tud_mount_cb(void)")
        umount_start = cdc.index("void tud_umount_cb(void)", mount_start)
        endpoint_start = cdc.index("void mp_usbd_cdc_transport_endpoint_state(", umount_start)
        self.assertIn("ev_cdc_rx_bus_mount", cdc[mount_start:umount_start])
        self.assertIn("ev_cdc_rx_bus_unmount", cdc[umount_start:endpoint_start])

        line_start = cdc.index("void MICROPY_WRAP_TUD_CDC_LINE_STATE_CB(tud_cdc_line_state_cb)")
        line_end = cdc.index("\n}\n\n#endif", line_start)
        line_state = cdc[line_start:line_end]
        self.assertIn("ev_cdc_rx_dtr_update", line_state)
        self.assertIn("!ev_stdio_mux_enabled()", line_state)
        self.assertIn("esp_vision_usb_auto_download_line_state(dtr, rts)", line_state)
        self.assertNotIn("ev_cdc_rx_bus_", line_state)
        self.assertNotIn("ev_session_link_lost", line_state)
        self.assertNotIn("ev_transport_link_", line_state)

        rx_state_start = cdc.index("void mp_usbd_cdc_transport_rx_state(")
        rx_read_start = cdc.index("size_t mp_usbd_cdc_transport_rx_read(", rx_state_start)
        rx_state = cdc[rx_state_start:rx_read_start]
        self.assertIn("tud_mounted()", rx_state)
        self.assertNotIn("tud_cdc_connected()", rx_state)

        endpoint_start = cdc.index("void mp_usbd_cdc_transport_endpoint_state(")
        poll_start = cdc.index("uintptr_t mp_usbd_cdc_poll_interfaces(", endpoint_start)
        endpoint_state = cdc[endpoint_start:poll_start]
        self.assertIn("tud_mounted()", endpoint_state)
        self.assertIn("tud_cdc_ready()", endpoint_state)
        self.assertNotIn("tud_cdc_connected()", endpoint_state)

        tx_start = cdc.index("size_t mp_usbd_cdc_tx_some_anytask(")
        tx_end = cdc.index("void MICROPY_WRAP_TUD_SOF_CB", tx_start)
        tx = cdc[tx_start:tx_end]
        self.assertIn("tud_cdc_ready()", tx)
        self.assertNotIn("tud_cdc_connected()", tx)

        control = CONTROL_SOURCE.read_text(encoding="utf-8")
        open_start = control.index("static void ev_control_transport_cdc_open(void)")
        open_end = control.index("static void ev_control_transport_cdc_update(void)", open_start)
        cdc_open = control[open_start:open_end]
        self.assertNotIn("ev_control_send_hello", cdc_open)
        self.assertNotIn("ev_control_send_rpc", cdc_open)


if __name__ == "__main__":
    unittest.main()
