# Native UVC output proposal

Status: implemented and flashed to ESP32_P4X_EYE on 2026-11-03. IDF 5.5 build and 12 Linux hardware acceptance checks passed. Other board/host combinations and simultaneous MSC I/O stress are not yet hardware-qualified.

## Intended behavior

On boards with an enabled USB-OTG device port, enumerate CDC + MSC + UVC at boot. UVC must work before Python starts and without initializing the camera. The default source is a firmware-resident ESP-VISION logo JPEG. Opening a host camera application starts transmission; it must not acquire an EV-MUX session or change sensor configuration. Boards without an enabled USB device port do not advertise this capability.

## Upstream investigation

The upstream `usb_device_uvc` component is useful as a reference, but cannot be added unchanged. Its CMake injects its own TinyUSB configuration include path and `usb_descriptors.c`; its initialization creates the USB PHY, calls `tusb_init()`, and starts a `tud_task()` loop. Its global mount, suspend and resume callbacks also overlap the existing MicroPython integration. Its first video endpoint is `0x81`, already used by CDC notification here. These are stack ownership and descriptor conflicts, rather than an inherent incompatibility between MSC and UVC.

References inspected on 2026-11-03 (upstream master is mutable; implementation must pin the reference revision):

- https://github.com/espressif/esp-iot-solution/blob/master/components/usb/usb_device_uvc/usb_device_uvc.c
- https://github.com/espressif/esp-iot-solution/blob/master/components/usb/usb_device_uvc/CMakeLists.txt
- https://github.com/espressif/esp-iot-solution/blob/master/components/usb/usb_device_uvc/tusb/usb_descriptors.h
- https://docs.espressif.com/projects/esp-iot-solution/en/latest/usb/usb_device/usb_device_uvc.html

The driver source carries Apache-2.0; the inspected descriptor header and TinyUSB configuration carry MIT notices. Any copied source requires its original per-file notice and a README license-table entry. Prefer existing TinyUSB descriptor macros and original platform glue.

## USB integration

Extend the existing MicroPython overlay, with one PHY owner, one TinyUSB initialization path, one USB task and one composite configuration. Keep CDC at interfaces 0/1 and endpoints `0x81`, `0x02`, `0x82`; keep MSC at interface 2 and endpoints `0x03`, `0x83`. Proposed UVC allocation is interfaces 3/4 and streaming IN endpoint `0x84`, subject to controller endpoint/FIFO validation on each supported target. Compute interface/endpoint maxima for configurations where CDC or MSC is disabled.

Enable the TinyUSB video class and streaming instance in `overlay/micropython/shared/tinyusb/tusb_config.h`; extend configuration length and strings in `mp_usbd.h` and a new overlay copy of `mp_usbd_descriptor.c`. Audit runtime descriptor support and endpoint reservations in `mp_usbd_runtime.c`. Verify that the registry TinyUSB build actually compiles the video class with this configuration. Preserve full-speed fallback and add correct high-speed, qualifier and other-speed descriptors where required by the selected controller.

Start with MJPEG and one advertised 320×240 mode. The supported frame interval is 100 ms; Linux V4L2/FFmpeg accepted this profile and captured the resulting JPEG stream. The implemented first profile uses a 512-byte isochronous endpoint, providing explicit host start/stop alternate settings. Bulk remains a possible later profile, subject to Windows/Linux/macOS camera acceptance. Do not promise resolution or frame rates before simultaneous-load tests. Reject invalid probe/commit format, frame index and interval values. Do not advertise unsupported formats or silently transmit JPEG dimensions different from the negotiated dimensions.

## Frame publication and EV-MUX

The existing preview publication point now shares JPEG preparation between UVC and EV-MUX. `uvc.bind("preview")` subscribes UVC to the processed images passed to `Image.flush()`; the EV-MUX adapter retains dimensions, timestamp and sequence metadata, while UVC consumes the owned JPEG bytes in its negotiated fixed mode. This image source is separate from `EV_STREAM_USER` and `EV_STREAM_DEBUG`, which represent session-bound logical output routing.

Keep the EV-MUX v4 wire format and `preview.frame` metadata unchanged. Each adapter applies independent readiness, pacing and backpressure. UVC must remain usable without EV-MUX ownership, including while the IDE is disconnected. The preview publication path encodes once for both subscribers using the existing preview quality; direct UVC publication uses quality 50. Skip encoding when no subscriber can accept a frame.

Use two bounded PSRAM slots allocated lazily, with latest/pending and in-flight ownership protected by the USB lock. Pending frames may be replaced by newer frames; in-flight bytes cannot be modified until completion or confirmed USB cancellation. Never retain a camera framebuffer or the reusable JPEG encoder buffer after publication. The slow UVC consumer must not block EV-MUX, REPL or capture. Account for alignment, DMA-accessible memory and cache synchronization according to the selected TinyUSB controller path.

## Proposed Python contract

The names below are implemented APIs. The repository exposes `sensor.snapshot()` and `image.Image.flush()` but has no public pipeline object to bind; `uvc.bind("preview")` subscribes to the existing processed-image publication point.

```python
import uvc

uvc.write(img)           # Publish a processed frame; bool means accepted/dropped.
uvc.set_frame(img)       # Snapshot an owned fixed frame and repeat it.
uvc.reset()              # Restore the built-in logo source.
uvc.is_streaming()       # Host has opened streaming.
uvc.get_mode()           # Negotiated format, width, height and frame interval.
uvc.bind("preview")      # Subscribe to Image.flush() with shared JPEG encoding.
```

`write` must snapshot/encode synchronously into owned bounded storage before returning. Camera framebuffer reuse and Python GC therefore cannot corrupt USB transmission. Invalid format/dimensions raise an exception; no host or temporary congestion returns `False`. Fixed-frame updates are atomic and leave the previous source intact on failure. Fixed sources repeat at the host-negotiated interval. For a live stream, repeat the latest completed frame while the producer is idle; an explicit reset, stream teardown or VM soft reset restores the logo. Define and validate a bounded fixed-frame size before allocating.

Calling `bind` changes only the UVC source; it does not make USB re-enumerate. Stream producer code runs in the VM; USB callbacks never invoke arbitrary Python or `sensor.snapshot()`. `uvc.reset()` resets source state, not the shared USB stack. A later pipeline API can expose this same stream producer contract without coupling UVC to sensor capture.

Example of the first useful manual publishing flow:

```python
import sensor
import uvc

sensor.reset()
sensor.set_pixformat(sensor.RGB565)
sensor.set_framesize(sensor.QVGA)
while True:
    img = sensor.snapshot()
    # Run inference and draw results here.
    uvc.write(img)
    img.flush()  # Existing EV-MUX preview remains available independently.
```

This manual example may encode twice; shared-stream fan-out is the path for encoding once. Preserve existing `img.flush()` behavior while introducing explicit stream publication rather than silently redirecting all previews to UVC.

## Implementation and acceptance

1. On ESP32_P4X_EYE, add the composite descriptor and native logo transmission first. Verify enumeration and camera opening while MSC and CDC remain available.
2. Add owned frame publication, Python bindings, stubs, bilingual documentation and a processed-frame example. Verify fixed-frame replacement, framebuffer reuse, full queues, invalid inputs, allocation failures and source cleanup.
3. Add shared image stream fan-out with independent UVC and EV-MUX pacing. Verify one slow consumer cannot stall the other, and no VM access occurs from USB callbacks.
4. Verify host close/reopen, unplug/replug, suspend/resume, probe/commit renegotiation, Ctrl-C, soft reset, and bootloader entry. In-flight storage must be released only after transport cancellation is confirmed.
5. Exercise simultaneous UVC video, EV-MUX preview/REPL and MSC reads/writes, retaining existing storage-writer lease/quarantine rules. Record real dimensions, frame rate, drops and heap usage. Test Windows, Linux and macOS camera clients before claiming general compatibility.
6. Build only the target firmware during experimentation. After completion, run formatting/copyright/stub checks and use CI for the final supported-board/IDF matrix. Enable UVC by default on supported USB-device boards only after endpoint and memory checks; explicitly gate boards such as ESP32_S31_CHATBOT that disable USB device support.

Remaining validation covers endpoint/FIFO and DMA behavior on other boards, full-speed fallback through a physical FS host/hub, Windows/macOS camera applications, suspend/resume and cable reconnect, and simultaneous MSC read/write stress. A future bulk profile or arbitrary pipeline object would require a separate contract and validation; the first implementation exposes the named preview source.

## Implemented limits

The first public binding accepts the named `preview` stream or `None`; it does not introduce an arbitrary Python producer callback or a new general-purpose pipeline object. JPEG storage is two persistent 48 KiB PSRAM slots, allocated lazily and retained across VM resets; in-flight ownership survives source reset. No Python object is retained by the USB worker. `get_mode()` returns the only supported mode even before negotiation. Fixed-frame update errors preserve the old source. Preview subscription ignores wrong dimensions and drops oversized/allocation-failed UVC frames independently of EV-MUX.

The composite configuration has five interfaces and endpoints `0x81`, `0x02`, `0x82`, `0x03`, `0x83`, `0x84` on ESP32_P4X_EYE. Its final ELF configuration descriptor is 249 bytes and was checked for correct total length, descriptor boundaries, interface/alternate-setting layout and endpoint address uniqueness. Full-speed bulk endpoints are reduced to 64 bytes; high-speed qualifiers and other-speed configuration are available for the built-in configuration. Custom `machine.USBDevice` descriptors retain responsibility for their own speed contract.

## Validation scope

The experimental P4X-EYE build passed twelve Linux UVC checks and nine TCP observer checks before migration to the current master baseline. These results do not validate the rebased commit. The implementation retains the current shared restart path; bootloader entry and post-flash startup must be rechecked against the current firmware and Device Core candidate in MR !110.

The USB descriptors currently advertise one MJPEG output mode, 320×240 at 10 FPS. Runtime source geometry must match that mode. Stable multi-mode descriptors, host-selected output geometry and configurable source adaptation remain follow-up work.

Reproduce the checks with the configured board build and explicitly selected device nodes:

```sh
python tools/check_uvc_descriptors.py --build-dir BUILD_DIR --output-dir OUTPUT_DIR
python tools/test_uvc.py --port SERIAL_PORT --video VIDEO_NODE --device-cli INSTALLED_DEVICE_CLI --output-dir OUTPUT_DIR
python tools/test_preview_tcp.py --host DEVICE_IP --port SERIAL_PORT --device-cli INSTALLED_DEVICE_CLI --output-dir OUTPUT_DIR
```
