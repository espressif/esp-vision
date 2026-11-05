USB Interfaces, Configuration and Disk Names
============================================

:link_to_translation:`zh_CN:[中文]`

This chapter describes USB configuration and the names visible on the host. See :doc:`interfaces` for EV-MUX connections and :doc:`../storage/index` for recovery, backups and write ownership.

Physical interfaces and functions
---------------------------------

.. list-table:: USB functions
   :header-rows: 1
   :widths: 22 38 40

   * - Interface
     - Host function
     - Configuration and limitations
   * - Serial/JTAG (``usj``)
     - Serial/debug and ROM download access.
     - Controlled by the SoC and ``MICROPY_HW_ESP_USB_SERIAL_JTAG``. It provides no MSC disks. TinyUSB strings do not rename this hardware interface.
   * - OTG CDC (``usb``)
     - Serial interface for EV-MUX, Python and native file transfers.
     - Requires the native USB device connection, ``MICROPY_HW_ENABLE_USBDEV`` and ``MICROPY_HW_USB_CDC``. A working USJ connection does not establish an OTG connection.
   * - OTG MSC
     - One mass-storage interface with one or two LUNs.
     - Requires ``MICROPY_HW_USB_MSC`` and usable storage; shares the existing TinyUSB device with CDC and enabled UVC. It does not mount a second filesystem.
   * - OTG UVC
     - Camera/video function on supported builds.
     - Controlled by ``MICROPY_HW_USB_UVC`` and the existing descriptors/stream. Enumeration does not establish camera initialization or frame delivery.

Select the physical connector using the board schematic. Enumerate descriptors, select an explicit port and confirm board/firmware identity. P4 boards may expose USJ and OTG simultaneously; COM/ttyACM numbers can change after reset or ROM entry. Download-mode handoff is described in :doc:`interfaces`.

Configuration locations
-----------------------

.. list-table:: Configuration ownership
   :header-rows: 1
   :widths: 35 65

   * - Location
     - Settings
   * - ``boards/<BOARD>/port/mpconfigboard.h``
     - Device/CDC/MSC/UVC feature macros, USJ enablement, manufacturer/product/interface strings and board VID/PID overrides.
   * - ``boards/<BOARD>/boardconfig.h``
     - EV-MUX default/fallback sink and storage automatic-mount/initialization policies. Choosing a sink does not enable a USB class or connector.
   * - Board ``sdkconfig.*`` and shared port defaults
     - IDF console, PHY/driver and FatFs settings. ``CONFIG_ESP_CONSOLE_*`` neither selects an EV-MUX owner nor provides MSC on USJ. ``CONFIG_FATFS_USE_LABEL`` enables the shared volume-label API.
   * - ``overlay/micropython/shared/tinyusb/``
     - Shared composite descriptors and class/endpoint configuration. Keep one TinyUSB driver/descriptor owner instead of adding another stack for disks.
   * - ``platform/usb_msc.c`` and ``platform/storage.h``
     - Fixed LUN mapping and medium names. MSC inquiry products are ``ESP_FLASH`` / ``ESP_SDCARD``; ``MICROPY_HW_USB_MSC_INTERFACE_STRING`` retains each board's original name without enforcing a common prefix.

Edit board/overlay sources, not the generated MicroPython copy, and rebuild with the repository-root ``idf.py --board <BOARD> ...`` entry point. VID/PID changes must match the actual USB functions and host-driver contract; copying a Serial/JTAG PID does not add JTAG hardware.

Volume labels, LUNs and drive letters
-------------------------------------

.. list-table:: Disk identity
   :header-rows: 1
   :widths: 20 30 25 25

   * - Medium
     - Python mount
     - Default FAT label / inquiry product
     - USB LUN
   * - Internal Flash
     - ``/`` and alias ``/flash``
     - ``ESP_FLASH``
     - 0
   * - SD card
     - ``/sdcard`` by default
     - ``ESP_SDCARD``
     - 1, when SD support is compiled

The FAT label is stored on the medium and is normally displayed by the host file manager. During automatic mounting, firmware assigns a default only to an already mounted volume with an empty label. Existing nonempty labels, including custom SD labels, are preserved. Renaming never formats the disk. Writer denial or label I/O failure produces a warning without crashing or converting a successful mount into a format request. Explicit ``storage.retry()`` and writer-handoff refresh remain mount-only and do not change labels.

FAT labels have at most 11 OEM bytes, exclude dots and convert lowercase to uppercase; long filenames do not extend labels. Therefore ``esp-vision.flash`` / ``esp-vision.sdcard`` cannot be FAT labels; use ``ESP_FLASH`` / ``ESP_SDCARD``. See the `FatFs label API <https://elm-chan.org/fsw/ff/doc/setlabel.html>`_.

USB manufacturer/product/interface strings identify the device or function. A SCSI inquiry product identifies a LUN and has a 16-byte field. Neither sets the FAT label. Windows assigns ``D:``/``E:`` drive letters; other systems choose their own mount paths. Firmware cannot reserve these letters. Identify disks using the physical device, LUN and volume metadata, rather than a remembered letter or label alone.

Windows File Explorer normally names a volume using its filesystem label. The Function EV Flash volume previously had an empty label, so Explorer displayed the generic USB-drive name. ``ESP32-P4X-Function-EV-Board Flash`` is a USB MSC interface string describing the single interface containing both LUNs; it does not name each volume. SCSI inquiry products identify disk devices and do not set volume labels either. Assigning ``ESP_FLASH`` / ``ESP_SDCARD`` to mounted, unlabeled volumes distinguishes the media while retaining existing custom labels. An empty SD slot has no readable FAT label, so Windows may still display a generic USB-drive name; an interface string alone cannot assign an Explorer volume name to that slot. See Microsoft's `Naming a Volume <https://learn.microsoft.com/en-us/windows/win32/fileio/naming-a-volume>`_ and `USB interface descriptor <https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/usbspec/ns-usbspec-_usb_interface_descriptor>`_.

Unavailable media report not ready in their fixed slots. SD does not become LUN 0 if Flash fails, and ``/flash`` adds no disk. Boards without SD support expose only Flash. Empty-slot visibility and drive-letter allocation are host behavior.

Mounting, ownership and validation
----------------------------------

Flash and SD automatic mounting are independent and enabled by default. Formatting an unrecognized filesystem is separate: the default board policies are ``ESP_VISION_STORAGE_FLASH_FORMAT_IF_NO_FILESYSTEM=1`` and ``ESP_VISION_STORAGE_SD_FORMAT_IF_NO_FILESYSTEM=0``. Disk naming never overrides these policies or authorizes formatting; see :doc:`../storage/index`.

An EV-MUX owner does not automatically own MSC writes. Safely eject the selected OS volume and complete the per-volume handoff in :doc:`python-files` before Python/IDE writes or backup. Two disks do not permit simultaneous host/VM writes to either shared FAT filesystem. Keep verified backups outside the device.

1. Inspect the selected device and ``capabilities.sinks``. Connect the native USB device port for MSC acceptance; formatting a card cannot fix an absent OTG connection.
2. Check ``storage.status("flash")`` / ``storage.status("sd")``. Mount and host writer admission must both succeed before the corresponding LUN is ready.
3. Inspect USB functions and logical disks separately. Record physical serial, firmware, LUN, actual volume label and OS drive letter. A COM port or product string alone is not evidence of a mounted disk.
4. Verify isolated test files/hashes and persistence, preserving existing data and custom labels. Unavailable media should return diagnostics while the other volume/control link remains usable.
5. Report CDC, actual dual-LUN enumeration and healthy SD read/write separately. USJ-only acceptance does not cover native USB MSC or both disks.
