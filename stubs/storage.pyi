# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations


#: Return non-destructive status and recovery guidance for flash or sd.
#: Storage faults are represented in the result, not by rebooting the board.
#: volume: "flash" (default) or "sd".
def status(volume: str = "flash") -> dict: ...

#: Retry mounting without formatting or erasing any data, including when board policy disables automatic mounting.
#: Check ok and operation_error; after success, soft_reset_required requests an explicit soft reset to publish the Python mount.
#: volume: "flash" (default) or "sd".
def retry(volume: str = "flash") -> dict: ...

#: Explicitly initialize an unmounted flash data/FAT partition named ffat.
#: This destroys files and requires the literal confirm=True after user approval and any required backup.
#: Mounted volumes, legacy vfs, open handles, quarantined writers and online SD formatting are not supported.
#: Check ok, operation_error and soft_reset_required; failure never triggers automatic reboot or another format.
#: volume: "flash" (default); "sd" returns an unsupported operation result.
#: confirm: Must be the boolean True; omission, False and truthy strings are rejected before any write.
def format(volume: str = "flash", *, confirm: bool = False) -> dict: ...
