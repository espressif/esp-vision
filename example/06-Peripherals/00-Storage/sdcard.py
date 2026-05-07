# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0


import os


def print_listdir(path):
    try:
        print(path + ":", os.listdir(path))
    except UnicodeError:
        print(path + ": filename decode failed")


print_listdir("/")
print_listdir("/sdcard")

path = "/sdcard/esp_vision_example.txt"

with open(path, "w") as f:
    f.write("hello esp-vision\n")

with open(path, "r") as f:
    print("read:", f.read().strip())

print("done")
