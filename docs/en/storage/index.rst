Storage, Filesystems and Data Protection
========================================

:link_to_translation:`zh_CN:[中文]`

This chapter defines the shared storage contract for firmware, Python programs, Device Core, IDE/Agent clients and recovery tools. Its purpose is to preserve user files and make destructive recovery a user decision. See :doc:`../api-reference/storage` for the runtime diagnostic and recovery API.

Media and filesystem ownership
-------------------------------

.. list-table:: Storage responsibilities
   :header-rows: 1
   :widths: 22 30 48

   * - Medium
     - Current use
     - Contract
   * - Internal Flash ``ffat``
     - FAT, exposed to Python at ``/`` and alias ``/flash``; native IDF path ``/flash``; MSC LUN 0 on supported USB-device boards
     - ESP-IDF owns the filesystem. The Python VFS and MSC share the same bytes, not independent copies. This named data/FAT partition may be initialized by the configured automatic policy or an explicitly confirmed recovery call.
   * - Legacy Flash ``vfs``
     - Mount existing FAT; may contain a filesystem from older firmware
     - Mount-only in automatic startup and online recovery. Do not reinterpret an unknown or LittleFS volume as disposable FAT.
   * - SD card
     - FAT at ``/sdcard``; MSC LUN 1 on SD-capable USB-device boards
     - ESP-IDF owns the filesystem. Missing/failed cards do not prevent Flash, the VM or communications from operating. The automatic policy may initialize an unrecognized card; explicit Python SD formatting remains unsupported. Use a selected host card for other recovery after backup.
   * - NVS
     - Network/settings, writer ownership and format-transaction records
     - Keep separate from user files. NVS errors do not authorize erasing NVS or formatting a data volume. A Flash-file backup does not include NVS.
   * - External NAND, where fitted
     - Not currently exposed as an ESP-VISION filesystem, including the onboard NAND on ESP-Mosaico
     - Report unsupported until a qualified backend exists. Do not treat it as SD or map the raw NOR ``ffat`` driver onto it; future support must account for NAND bad blocks, ECC and recovery.
   * - Frozen modules / read-only RAM VFS
     - Firmware modules or temporary model assets
     - Not a writable user volume or durable backup. RAM data disappears on restart; frozen content belongs to the firmware image.

Board partition tables are authoritative for offsets and sizes. Never infer a destructive range from a module revision, an old example, or a partition with the same label on another board. Board Manager drivers and camera/model consumers must use the common storage owner rather than mount a second filesystem over the same medium.

``/.esp_vision_disk`` is only a default-file setup marker. Neither this file nor a saved NVS state is a backup or a new permission to format. Formatting permission comes from the board policy below or an explicit recovery decision.

The Flash backend currently writes through the project's raw block driver with 512-byte logical sectors and 4096-byte erase-block read/modify/write. It is not a journaled filesystem or an ESP-IDF wear-levelling volume. Frequent logs, images and recordings should use an appropriate removable medium and retention limits; account for free space, erase wear and metadata overhead.

Optional volumes and automatic mounting
---------------------------------------

Flash and SD are independent optional volumes. By default, startup attempts to mount Flash and, on boards with SD support, an available SD card. A missing/failed Flash volume does not prevent SD from mounting; a missing/failed SD card does not prevent Flash from mounting. Initialization follows the separate policy below; generic mount errors do not trigger it. With neither volume available, the VM and control transports remain usable with a virtual root and diagnostics; startup scripts are not automatically relocated to SD.

Each board may override the following independent defaults in ``boards/<BOARD>/boardconfig.h``:

.. code-block:: c

   #define ESP_VISION_STORAGE_AUTO_MOUNT_FLASH (1)
   #define ESP_VISION_STORAGE_AUTO_MOUNT_SD    (1)
   #define ESP_VISION_STORAGE_FLASH_FORMAT_IF_NO_FILESYSTEM (1)
   #define ESP_VISION_STORAGE_SD_FORMAT_IF_NO_FILESYSTEM    (0)

Set either ``AUTO_MOUNT`` value to ``0`` to defer that medium's automatic mount. These are independent build-time policies, not persistent Python settings and not hardware capability switches. SD still requires ``MICROPY_HW_ENABLE_SDCARD`` and the supported SDMMC backend. An otherwise available medium with automatic mounting disabled reports ``unmounted`` / ``STORAGE_NOT_MOUNTED``; ``storage.retry("flash")`` or ``storage.retry("sd")`` can explicitly mount it without formatting, even when automatic initialization is enabled. Inspect the result and choose a soft reset to publish a newly mounted Python volume. An SD card that was unavailable at boot is retried on subsequent VM starts only when automatic SD mounting is enabled; already mounted physical filesystems are retained across soft resets.

``ESP_VISION_STORAGE_FLASH_FORMAT_IF_NO_FILESYSTEM`` defaults to ``1``; ``ESP_VISION_STORAGE_SD_FORMAT_IF_NO_FILESYSTEM`` defaults to ``0``. During automatic mounting, exact FatFs ``FR_NO_FILESYSTEM`` authorizes formatting and remounting only when that medium's policy is enabled. Internal ``ffat`` therefore initializes by default, while an unrecognized SD card is preserved unless its policy is explicitly enabled. A nonblank factory image, a foreign filesystem or a damaged FAT may produce this result: it does not prove that the region contains no valuable data. Enabling either policy accepts erasing those contents without runtime confirmation. Keep the relevant policy at ``0`` where unrecognized data must survive. Legacy ``vfs`` remains mount-only regardless of either option.

An existing mountable FAT is preserved. ``FR_INT_ERR``, I/O/not-ready/write-protection failures, missing media, outstanding handles and denied/quarantined writers do not authorize automatic formatting. Disabled automatic mounts never initialize that medium. ``storage.retry()`` and writer-handoff refreshes always remain mount-only. Logs identify policy-triggered initialization before destructive writes; failed initialization reports an error without rebooting or crashing.

Internal Flash persists PENDING/READY format transactions in NVS; an interrupted transaction blocks further automatic formatting even when its policy is enabled and needs a new explicit recovery decision. SD currently has no persistent format journal or equivalent interrupted-format guard. If SD automatic initialization is explicitly enabled, a power cut can be followed by another initialization when the next mount returns ``FR_NO_FILESYSTEM``. With either policy setting, a partially created FAT may appear readable; that does not prove power-loss recovery. Keep verified off-device backups; the default SD policy preserves unrecognized contents instead of formatting them.

Flash paths ``/main.py`` and ``/flash/main.py`` refer to the same file. The alias is a second Python VFS bridge to the existing IDF filesystem, not another FAT mount, copy or USB disk. Relative paths follow the selected bridge's current directory. Use one consistent prefix for both arguments of ``os.rename()``: MicroPython rejects a rename between distinct VFS mount points, including ``/`` and ``/flash``. ``/flash`` is reserved as a mount point; a pre-existing physical ``flash`` directory is retained and can be accessed at ``/flash/flash``. ``/flash/sdcard`` refers to a directory on Flash, not the separate SD volume at ``/sdcard``.

Backup and restore tools must identify volumes rather than count path prefixes: do not back up ``/`` and ``/flash`` as separate copies or recursively traverse synthetic mount points. Prefer ``/flash`` as the explicit Flash backup root, and back up ``/sdcard`` separately.

USB with two volumes
--------------------

Default labels are ``ESP_FLASH`` and ``ESP_SDCARD``. Automatic mounts name only unlabeled volumes and preserve custom labels; label errors never authorize formatting. See :doc:`../communication/usb` for USB configuration, naming limits, interface strings and host drive letters.

On boards/ports with native USB device MSC enabled, one MSC interface exports fixed logical-unit slots: LUN 0 is internal Flash and LUN 1 is SD when the board supports it. If both volumes are mounted, healthy and admitted to the host by the writer gate, the host can enumerate two separate media. Drive letters, mount paths and empty-slot presentation are controlled by the host OS. A board without SD support exports only the Flash slot. The USB Serial/JTAG debug interface does not provide these MSC disks.

An absent, unmounted or unavailable medium reports not ready for that slot; SD never shifts to LUN 0 when Flash is unavailable. Turning off automatic mounting defers that slot's readiness, not the other volume or the entire USB interface. ``/flash`` creates no additional LUN. Ejection and writer handoff are per volume: safely eject the selected OS volume and verify its ownership before Python/IDE writes; two visible disks do not authorize simultaneous host and VM writes to either shared filesystem.

Failure policy: preserve data and keep control available
--------------------------------------------------------

A mount, allocation, NVS, write, close, sync or card-readiness error is a recoverable storage condition. Storage error paths must return status/errors, close admission when necessary, and keep the REPL, session management and network/control transports available. They must not call a fatal assertion, abort, crash or reboot to hide the failure. Python argument exceptions and an operation returning ``ok: false`` are not a device crash.

Mount failure alone is not formatting permission: the automatic exception is the per-medium board policy for exact ``FR_NO_FILESYSTEM`` described above, enabled for Flash and disabled for SD by default. When the relevant policy is disabled, blank, factory/unknown and damaged volumes remain untouched. Missing NVS records never independently authorize formatting, and NVS errors do not bypass the Flash transaction guard. ``storage.retry()`` never creates a filesystem, erases a region or resets the device.

.. list-table:: Diagnosis and choices
   :header-rows: 1
   :widths: 24 34 42

   * - State
     - Meaning
     - User-directed next step
   * - ``ready``
     - Filesystem mounted; writer permission remains a separate condition
     - Use files under the writer rules and maintain a verified off-device backup.
   * - ``blank``
     - The whole partition was read and found erased
     - Confirm initialization if this is the intended empty volume.
   * - ``unmounted``
     - Automatic mounting is disabled for this otherwise available medium
     - Use ``storage.retry()`` if mounting is wanted; this is not a format request.
   * - ``unrecognized``
     - No mountable FAT; contents may be factory assets, another filesystem or damaged user data
     - Preserve/export required data, inspect the layout, then choose retry, offline recovery or confirmed formatting.
   * - ``absent`` / ``unsupported``
     - Medium missing or operation unavailable
     - Check the card/board capability. Do not propose formatting absent hardware.
   * - ``io_error`` / ``unavailable``
     - Driver, mount or resource failure
     - Inspect the error code, power, connection and free resources; retry only when the cause is addressed.
   * - ``state_error``
     - NVS format-transaction metadata cannot be read or validated
     - Preserve NVS and the data volume for diagnosis; do not clear history to bypass the guard.
   * - ``format_interrupted``
     - Formatting failed before completion
     - Flash persists this guard and requires a new decision; SD has no equivalent persistent journal. Inspect the medium and the board policy before restarting.
   * - Writer held / quarantined
     - Another writer owns the medium, or a handoff outcome is uncertain
     - Finish the verified handoff or investigate quarantine. A format request does not override ownership.

``storage.status()`` reports the state, stable code, human-readable message, mount/presence/capability flags, native error, FatFs result when known, and Flash geometry. Device Core ``inspect({ fields: ['storage'] })`` retains the writer fields and also returns ``fs_state``, ``fs_error``, ``fatfs_result`` and ``format_supported``. These fields describe a snapshot, not permission to destroy data. Logs retain actionable storage messages after transport startup, and native uploads to an unmounted volume return ``STORAGE_UNAVAILABLE`` with recovery guidance instead of an opaque file-open failure.

A frontend must show the affected volume, reason and data impact; offer inspect/retry/export before an explicit destructive choice; and leave manual formatting unselected until a deliberate user confirmation. Agents and unattended jobs must not infer new runtime confirmation from a timeout, reconnect, generic request to fix an error, or factory data. The firmware's configured automatic initialization is a separate boot policy. This change supplies status, logs, errors and an explicit REPL API; it does not claim that every existing IDE/Web UI already has a backup wizard or format button.

One writer, safe publication and power loss
-------------------------------------------

EV-MUX session ownership is not filesystem write ownership. Before Python/IDE writes, backup, restore or unmount, coordinate the volume with the OS MSC host and the storage-writer handoff described in :doc:`../communication/python-files`. Eject the OS volume, stop application/background writers, drain handles and writes, and check the actual writer state. An uncertain handoff remains quarantined across reboot. Legacy compatibility mode is not a qualified concurrent-writer guarantee; merely connecting the SDK does not make simultaneous OS and Python writes safe.

Unmounted Flash is not ready for MSC reads or writes and is not published until its format transaction completes. This transaction guard does not preserve future files if FAT later becomes unrecognizable while the automatic initialization policy remains enabled. SD readiness is independent and its format has no persistent transaction guard. Do not bypass a failed close, outstanding upload descriptor or writer quarantine to make a format succeed.

Native binary upload uses a temporary file, expected length/SHA-256, sync/close and final publication. Ordinary replacement failure attempts rollback from an old-file backup. This protects against transport errors and ordinary publication errors, not every power cut between FAT directory updates. The staging/rollback file on the same medium is not an independent backup. ``fsync`` and a successful transfer response do not turn FAT or the raw Flash erase-block update into a journaled, power-loss-atomic transaction. After a lost final response, inspect the destination before retrying.

Write important application data to a new file, validate and close/sync it, and keep an earlier verified generation until publication is checked. Use bounded logs and reserve free space for both the new and old generation. If a write or close fails, stop further writes and report the original error; repeatedly retrying or deleting unrelated files is not recovery.

Backup and restore policy
--------------------------

A backup must live on a different failure domain: normally the host computer or another independently retained medium. A directory or renamed old file on the same Flash/card does not protect against corruption, accidental formatting or media failure.

1. Identify the physical device, board, firmware and selected volume. Stop scripts and every background writer; safely eject MSC and verify ownership before taking a filesystem-consistent copy. If writers cannot be stopped, label the result as an unverified copy rather than a consistent backup.
2. Export user scripts, hidden configuration, required packages, model sidecars and other required files. Record paths, sizes and SHA-256 hashes in a host-side manifest along with device identity and filesystem/layout metadata. Preserve previous backup generations instead of overwriting the last known good copy.
3. Verify the downloaded bytes against the manifest and check that expected files are present. A completed download alone is not proof of a usable backup. Periodically test restoration on a suitable spare medium/device.
4. Restore only after verifying the target identity, compatible layout and enough free space. Use the normal verified upload/ownership path, inspect length/hash/readback, and retain the backup until validation succeeds. Do not automatically execute restored startup scripts before the user has checked them.

Current file APIs allow selected-file export/import through Device Core or host copying after a safe MSC handoff. A complete manifest-based backup/restore wizard is not implemented by this firmware API. Hosts implementing one must follow the rules above and distinguish successful, partial, failed and unverified copies; they must never call a partial copy a verified backup.

If the filesystem cannot mount, a selected raw partition image can preserve evidence for offline recovery. Entering ROM mode interrupts runtime; a user-requested read may use the actual offset and length from the installed partition table:

.. code-block:: console

   esptool --chip <CHIP> --port <PORT> read-flash <OFFSET> <LENGTH> <HOST_BACKUP.bin>

A raw image is not a verified file-level backup and may require the matching layout and encryption context to be usable. NVS/system-state backups are a separate explicit scope and may include credentials. Protect those backup files appropriately. Routine development flashing does not automatically back up/restore firmware or read the whole chip; such operations run only when the user requests them.

Explicit recovery of an unmounted Flash volume
----------------------------------------------

Inspect without writing:

.. code-block:: python

   import storage
   print(storage.status("flash"))
   print(storage.status("sd"))
   print(storage.retry("flash"))  # mount retry only; never formats

If ``retry`` returns ``ok: True`` and ``soft_reset_required: True``, the user can issue a soft reset to publish the new Python mount. Failures leave the runtime available and retain the diagnostic state.

Only after the user has selected the correct Flash volume, preserved any needed files and agreed to erase it, run the following separate destructive operation:

.. code-block:: python

   result = storage.format("flash", confirm=True)
   print(result)

The literal boolean ``True`` is required; omitted/false/truthy-string confirmation is rejected. Recovery only formats an unmounted data/FAT ``ffat`` after open handles and writer admission are checked. It refuses a mounted/readable volume, legacy ``vfs``, online SD formatting and an unavailable/quarantined writer. Hardware I/O failures do not trigger formatting. The range comes from the device's partition table; NVS, firmware and SD are not erased.

Before formatting, a partition-bound NVS ``PENDING`` record is committed. Completion requires successful format, remount and a durable ``READY`` record before publishing the volume. A failed commit, format or remount leaves the volume unavailable; an interrupted ``PENDING`` state never resumes on its own, even if a partial FAT header looks readable. A new explicit user decision is required. Clearing NVS is not an approved way to bypass this state.

Check ``ok`` and ``operation_error``. If success requests ``soft_reset_required``, explicitly soft reset to publish the Python mount and create missing default files:

.. code-block:: python

   import machine
   machine.soft_reset()

A reset is a user-selected follow-up, never an automatic response to a storage fault. Reinspect storage and verify normal file creation, upload, readback and persistence across restart before restoring the application. A failure or lost response has uncertain completion; inspect state before repeating a destructive operation.

Firmware installation and acceptance
------------------------------------

Ordinary firmware updates preserve user files and NVS. Full installation/layout changes and whole-chip erase have different data effects; hosts must describe those effects before the user selects them. Never silently switch an update to an installation or format a volume because its layout differs. See :doc:`../device-core/api` for the update/install contract.

Acceptance must cover blank and residue-filled media, valid existing FAT, damaged FAT, missing/removed SD, NVS failures, full media, write/close/sync faults, interrupted formatting and uncertain writer handoff. Verify zero automatic erase/format/NVS writes for mount failures outside the enabled, exact ``FR_NO_FILESYSTEM`` initialization policy. For policy-authorized initialization, verify its medium-specific defaults and Flash PENDING/READY transaction guards. Also verify continued session/REPL operation, useful status/errors, explicit confirmation guards, and isolation of unrelated volumes. Host fault-injection tests do not establish real power-loss or affected-module qualification; report the exact commit, medium, board, interface and actual hardware evidence.
