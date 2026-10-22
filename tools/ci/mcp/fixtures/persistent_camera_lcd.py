# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Keep a camera image on the LCD until a host-written file requests stop."""

import display
import sensor
import time


CONTROL_PATH = "__CONTROL_PATH__"
OBSERVED_PATH = "__OBSERVED_PATH__"
BOOT_PATH = "__BOOT_PATH__"
PROGRESS_PATH = "__PROGRESS_PATH__"
DONE_PATH = "__DONE_PATH__"
CONTROL_TOKEN = "__CONTROL_TOKEN__"
STOP_GRACE_MS = 9000
RAINBOW = (
    (255, 0, 0),
    (255, 127, 0),
    (255, 255, 0),
    (0, 200, 0),
    (0, 100, 255),
    (75, 0, 130),
    (148, 0, 211),
)

lcd = None
sensor_ready = False
frame_count = 0
stop_frame = -1
cleanup_error = None
boot_count = 0


def draw_label(frame, label, x, y, color, scale=3):
    for index, char in enumerate(label):
        frame.draw_string(x + index * 20, y, char, color=color, scale=scale)


def draw_stop_marker(frame, width, height):
    frame.draw_rectangle(0, 0, width, height, color=(0, 0, 255), fill=True)
    draw_label(frame, "STOP", max(8, (width - 4 * 20) // 2), height // 3,
               color=(255, 255, 255))
    draw_label(frame, "SIGNAL RECEIVED", 8, height // 2,
               color=(255, 255, 0), scale=2)


try:
    try:
        with open(BOOT_PATH, "r") as boot_file:
            boot_count = int(boot_file.read().strip())
    except OSError:
        boot_count = 0
    boot_count += 1
    with open(BOOT_PATH, "w") as boot_file:
        boot_file.write(str(boot_count))
    with open(PROGRESS_PATH, "w") as progress_file:
        progress_file.write("0")

    lcd = display.Display(backlight=100)
    sensor_ready = True
    sensor.reset()
    sensor.set_pixformat(sensor.RGB565)
    sensor.set_framesize(sensor.QVGA)
    sensor.skip_frames(time=500)

    while True:
        frame = sensor.snapshot()
        width = frame.width()
        height = frame.height()

        if frame_count % 4 == 0:
            try:
                with open(CONTROL_PATH, "r") as control_file:
                    control = control_file.read().strip()
            except OSError:
                control = ""
            if control == CONTROL_TOKEN:
                stop_frame = frame_count

        if stop_frame >= 0:
            draw_stop_marker(frame, width, height)
        else:
            band_height = max(24, height // 8)
            for index, color in enumerate(RAINBOW):
                x0 = width * index // len(RAINBOW)
                x1 = width * (index + 1) // len(RAINBOW)
                frame.draw_rectangle(x0, 0, x1 - x0, band_height, color=color, fill=True)

            label = "esp-vision"
            label_x = (width - len(label) * 20) // 2
            label_y = max(band_height + 20, height // 3)
            frame.draw_rectangle(label_x - 8, label_y - 6, len(label) * 20 + 16, 44,
                                 color=(0, 0, 0), fill=True)
            draw_label(frame, label, label_x, label_y,
                       color=RAINBOW[frame_count % len(RAINBOW)])
            frame.draw_rectangle(2, height - 28, width - 4, 26,
                                 color=(0, 0, 0), fill=True)
            draw_label(frame, "FRAME %d" % frame_count, 8, height - 24,
                       color=(255, 255, 255), scale=2)
            frame.draw_rectangle(2, 2, width - 4, height - 4,
                                 color=(255, 255, 255), thickness=2)

        lcd.write(frame, fit=True)
        frame_count += 1

        if stop_frame >= 0:
            with open(OBSERVED_PATH, "w") as observed_file:
                observed_file.write("%s %d" % (CONTROL_TOKEN, stop_frame))
            stop_started = time.ticks_ms()
            while time.ticks_diff(time.ticks_ms(), stop_started) < STOP_GRACE_MS:
                stop_image = sensor.snapshot()
                draw_stop_marker(stop_image, stop_image.width(), stop_image.height())
                lcd.write(stop_image, fit=True)
                frame_count += 1
                if frame_count % 8 == 0:
                    with open(PROGRESS_PATH, "w") as progress_file:
                        progress_file.write(str(frame_count))
                time.sleep_ms(30)
            with open(PROGRESS_PATH, "w") as progress_file:
                progress_file.write(str(frame_count))
            break

        if frame_count % 8 == 0:
            with open(PROGRESS_PATH, "w") as progress_file:
                progress_file.write(str(frame_count))
        time.sleep_ms(30)
except BaseException:
    raise
finally:
    try:
        if sensor_ready:
            sensor.shutdown()
    except BaseException as error:
        cleanup_error = error
    try:
        if lcd is not None:
            lcd.deinit()
    except BaseException as error:
        if cleanup_error is None:
            cleanup_error = error
    with open(DONE_PATH, "w") as done_file:
        done_file.write("boot=%d\nframes=%d\nstop_frame=%d\ncleanup=%s\n" % (
            boot_count,
            frame_count,
            stop_frame,
            "ok" if cleanup_error is None else "error",
        ))
    if cleanup_error is not None:
        raise cleanup_error
