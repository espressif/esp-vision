# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

# Servo (SG90-style) control over a 50 Hz PWM signal.


import time
from machine import Pin, PWM


# PWM output pin for the servo signal wire.
SERVO_PIN = 10

# Pulse width (us) at the two angle limits. Tune per servo if travel falls short of a full 0-180 deg.
MIN_US = 500
MAX_US = 2500
FREQ = 50
PERIOD_US = 1_000_000 // FREQ  # 20000 us at 50 Hz


servo = PWM(Pin(SERVO_PIN), freq=FREQ)


def write_angle(angle):
    """Move the servo to `angle` degrees (0-180)."""
    angle = max(0, min(180, angle))
    pulse_us = MIN_US + (MAX_US - MIN_US) * angle // 180
    servo.duty_u16(pulse_us * 65535 // PERIOD_US)


try:
    while True:
        for angle in (0, 90, 180, 90):
            print("angle:", angle)
            write_angle(angle)
            time.sleep_ms(700)
finally:
    servo.deinit()
