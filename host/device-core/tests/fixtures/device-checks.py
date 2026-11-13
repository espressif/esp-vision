# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Device-side camera/LCD fixtures migrated from tools/ci/device_checks.py.

All transport, ownership and cleanup are supplied by the public JS SDK.
"""


def is_unsupported(exc):
    message = str(exc).lower()
    return any(token in message for token in ('esp_err_not_supported', 'esp_err_not_found', 'no module named', 'enodev'))


def camera(opts):
    """Explicitly publish an application snapshot before passive observation."""
    import sensor
    sensor.reset()
    sensor.set_pixformat(getattr(sensor, opts['pixformat']))
    sensor.set_framesize(getattr(sensor, opts['framesize']))
    frame = sensor.snapshot()
    return dict(width=frame.width(), height=frame.height())


def framesizes(opts):
    import sensor
    sensor.reset()
    sensor.set_pixformat(sensor.RGB565)
    results = []
    for name in ('QQVGA', 'QVGA', 'VGA'):
        try:
            sensor.set_framesize(getattr(sensor, name))
            sensor.skip_frames(n=2)
            img = sensor.snapshot()
            results.append(dict(name=name, supported=True, width=img.width(), height=img.height(),
                                reportedWidth=sensor.width(), reportedHeight=sensor.height()))
        except Exception as exc:
            if not is_unsupported(exc):
                raise
            results.append(dict(name=name, supported=False, reason=str(exc)))
    working = [item for item in results if item['supported']]
    assert working, 'no framesize produced a frame'
    for item in working:
        assert item['width'] == item['reportedWidth'] and item['height'] == item['reportedHeight'], 'frame size differs from sensor-reported size: ' + item['name']
    return dict(results=results)


def pixformats(opts):
    import sensor
    sensor.reset()
    sensor.set_framesize(getattr(sensor, opts['framesize']))
    results = []
    for name, depth in (('RGB565', 2), ('GRAYSCALE', 1)):
        try:
            sensor.set_pixformat(getattr(sensor, name))
            sensor.skip_frames(n=2)
            img = sensor.snapshot()
            results.append(dict(name=name, supported=True, width=img.width(), height=img.height(),
                                actualBytes=img.size(), expectedBytes=img.width() * img.height() * depth))
        except Exception as exc:
            if not is_unsupported(exc):
                raise
            results.append(dict(name=name, supported=False, reason=str(exc)))
    working = [item for item in results if item['supported']]
    assert working, 'no pixel format produced a frame'
    for item in working:
        assert item['actualBytes'] == item['expectedBytes'], 'pixel buffer size mismatch: ' + item['name']
    return dict(results=results)


def orientation(opts):
    import sensor
    sensor.reset()
    sensor.set_pixformat(getattr(sensor, opts['pixformat']))
    sensor.set_framesize(getattr(sensor, opts['framesize']))
    sensor.skip_frames(n=3)
    sensor.set_hmirror(True)
    sensor.set_vflip(True)
    sensor.skip_frames(n=2)
    img = sensor.snapshot()
    mirrored = (int(sensor.get_hmirror()), int(sensor.get_vflip()))
    sensor.set_hmirror(False)
    sensor.set_vflip(False)
    sensor.skip_frames(n=2)
    sensor.snapshot()
    assert mirrored == (1, 1), 'mirror or flip did not turn on'
    assert not sensor.get_hmirror() and not sensor.get_vflip(), 'mirror or flip did not turn off'
    return dict(width=img.width(), height=img.height(), mirrored=list(mirrored))


def ipa_stats(opts):
    import sensor
    sensor.reset()
    sensor.set_pixformat(getattr(sensor, opts['pixformat']))
    sensor.set_framesize(getattr(sensor, opts['framesize']))
    sensor.skip_frames(n=3)
    import json
    if not hasattr(sensor, 'get_ipa_stats'):
        raise RuntimeError('ESP_ERR_NOT_SUPPORTED: IPA statistics API absent')
    original = sensor.get_ipa()
    if original is None:
        raise RuntimeError('ESP_ERR_NOT_SUPPORTED: runtime IPA unavailable')

    def uint(value, bits):
        assert type(value) is int and 0 <= value < 1 << bits, 'invalid unsigned statistic'

    def awb(value):
        assert isinstance(value, dict), 'AWB must be a dictionary'
        assert set(value) == set(('counted', 'sum_r', 'sum_g', 'sum_b')), 'AWB fields'
        for number in value.values():
            uint(number, 32)

    def grid(value, validate):
        assert isinstance(value, list) and len(value) == 5, 'grid X size'
        for column in value:
            assert isinstance(column, list) and len(column) == 5, 'grid Y size'
            for cell in column:
                validate(cell)

    def validate(stats):
        assert isinstance(stats, dict), 'statistics must be a dictionary'
        keys = ('seq', 'flags', 'ae', 'awb', 'awb_subwin', 'hist', 'sharpen', 'af')
        assert set(stats) == set(keys), 'statistics fields'
        uint(stats['seq'], 64)
        uint(stats['flags'], 6)
        assert stats['flags'] != 0, 'no valid statistics blocks'
        for name, flag in (('awb', 1), ('ae', 2), ('hist', 4), ('sharpen', 8), ('af', 16), ('awb_subwin', 32)):
            assert (stats[name] is not None) == bool(stats['flags'] & flag), 'validity mismatch: ' + name
        if stats['ae'] is not None:
            grid(stats['ae'], lambda value: uint(value, 32))
        if stats['awb'] is not None:
            awb(stats['awb'])
        if stats['awb_subwin'] is not None:
            assert stats['awb'] is not None, 'subwindows without global AWB'
            grid(stats['awb_subwin'], awb)
        if stats['hist'] is not None:
            assert isinstance(stats['hist'], list) and len(stats['hist']) == 16, 'histogram size'
            for value in stats['hist']:
                uint(value, 32)
        if stats['sharpen'] is not None:
            uint(stats['sharpen'], 8)
        if stats['af'] is not None:
            assert isinstance(stats['af'], list) and len(stats['af']) == 3, 'AF window count'
            for window in stats['af']:
                assert isinstance(window, dict), 'AF must be a dictionary'
                assert set(window) == set(('definition', 'luminance')), 'AF fields'
                uint(window['definition'], 32)
                uint(window['luminance'], 32)

    def sample():
        for _ in range(5):
            sensor.skip_frames(n=2)
            stats = sensor.get_ipa_stats(timeout=200)
            if stats is not None:
                validate(stats)
                return stats
        raise RuntimeError('no IPA statistics after five reads')
    sequences = []
    flags = []
    try:
        sensor.set_ipa(True)
        assert sensor.get_ipa() is True, 'IPA did not start'
        for timeout in (-1, 0, 1001):
            try:
                sensor.get_ipa_stats(timeout=timeout)
            except ValueError:
                pass
            else:
                raise RuntimeError('invalid timeout accepted')
        for _ in range(3):
            stats = sample()
            if sequences:
                assert stats['seq'] > sequences[-1], 'stale statistics sequence'
            sequences.append(stats['seq'])
            flags.append(stats['flags'])
        sensor.set_ipa(False)
        assert sensor.get_ipa() is False, 'IPA did not stop'
        sensor.snapshot()
        try:
            sensor.get_ipa_stats(timeout=200)
        except OSError as exc:
            assert 'ESP_ERR_INVALID_STATE' in str(exc), 'wrong disabled-IPA error'
        else:
            raise RuntimeError('statistics read accepted while IPA disabled')
        sensor.set_ipa(True)
        assert sensor.get_ipa() is True, 'IPA did not restart'
        stats = sample()
        restarted_seq = stats['seq']
        sensor.snapshot()
    finally:
        sensor.set_ipa(original)
    assert sensor.get_ipa() is original, 'IPA state was not restored'
    return dict(seq=sequences, flags=flags, restarted_seq=restarted_seq)


def snapshot_rate(opts):
    import sensor
    sensor.reset()
    sensor.set_pixformat(getattr(sensor, opts['pixformat']))
    sensor.set_framesize(getattr(sensor, opts['framesize']))
    sensor.skip_frames(n=3)
    import time
    img = sensor.snapshot()
    start = time.ticks_ms()
    for _ in range(opts['frames']):
        img = sensor.snapshot()
    elapsed = time.ticks_diff(time.ticks_ms(), start)
    assert elapsed > 0, 'implausible capture time'
    return dict(width=img.width(), height=img.height(), elapsedMs=elapsed, frames=opts['frames'])


def preview(opts):
    import sensor
    sensor.reset()
    sensor.set_pixformat(getattr(sensor, opts['pixformat']))
    sensor.set_framesize(getattr(sensor, opts['framesize']))
    sensor.skip_frames(n=3)
    for _ in range(opts['frames']):
        sensor.snapshot().flush()
    return dict(flushed=opts['frames'])


def image_ops(opts):
    import sensor
    sensor.reset()
    sensor.set_pixformat(getattr(sensor, opts['pixformat']))
    sensor.set_framesize(getattr(sensor, opts['framesize']))
    sensor.skip_frames(n=3)
    img = sensor.snapshot()
    width, height = (img.width(), img.height())
    img.draw_rectangle(4, 4, 40, 30, color=(255, 0, 0), thickness=2)
    img.draw_line(0, 0, width - 1, height - 1, color=(0, 255, 0), thickness=2)
    img.draw_string(4, 40, 'CI', color=(255, 255, 255), scale=2)
    half = img.copy(x_scale=0.5, y_scale=0.5)
    mean = img.get_statistics().mean()
    jpeg = img.compress(quality=60)
    assert half.width() == width // 2 and half.height() == height // 2, 'incorrect half-scale image'
    assert jpeg.size() > 0, 'empty compressed JPEG'
    return dict(width=width, height=height, halfWidth=half.width(), halfHeight=half.height(), mean=mean, jpegBytes=jpeg.size())


_lcd = None


def close_display():
    global _lcd
    lcd, _lcd = _lcd, None
    if lcd is not None:
        lcd.deinit()


def display(opts):
    global _lcd
    import display, image, time
    close_display()
    lcd = _lcd = display.Display(backlight=100)
    try:
        frame = image.Image(lcd.width(), lcd.height(), image.RGB565)
        colors = ((255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 255))
        bar = frame.width() // len(colors)
        for index, color in enumerate(colors):
            frame.draw_rectangle(index * bar, 0, bar, frame.height(), color=color, fill=True)
        frame.draw_string(8, 8, 'ESP-VISION CI', color=(0, 0, 0), scale=2)
        lcd.write(frame, fit=False)
        time.sleep(opts['displayHold'])
        lcd.backlight(40)
        dimmed = lcd.backlight()
        time.sleep(opts['displayHold'])
        lcd.backlight(100)
        time.sleep(opts['displayHold'])
        assert dimmed == 40 and lcd.backlight() == 100, 'backlight did not follow requested levels'
        return dict(width=lcd.width(), height=lcd.height(), dimmed=dimmed, restored=lcd.backlight())
    finally:
        if not opts.get('screenCapture'):
            close_display()


def display_stream(opts):
    import sensor
    sensor.reset()
    sensor.set_pixformat(getattr(sensor, opts['pixformat']))
    sensor.set_framesize(getattr(sensor, opts['framesize']))
    sensor.skip_frames(n=3)
    import display, time
    lcd = display.Display(backlight=100)
    try:
        img = sensor.snapshot()
        lcd.write(img)
        start = time.ticks_ms()
        for _ in range(max(10, opts['frames'] // 2)):
            lcd.write(sensor.snapshot())
        elapsed = time.ticks_diff(time.ticks_ms(), start)
        lcd.clear()
        assert elapsed > 0, 'implausible display stream time'
        return dict(width=lcd.width(), height=lcd.height(), cameraWidth=img.width(), cameraHeight=img.height(), elapsedMs=elapsed, frames=max(10, opts['frames'] // 2))
    finally:
        lcd.deinit()


def camera_display_lifecycle(opts):
    """Exercise camera/LCD coexistence, camera shutdown recovery, and restart."""
    import gc, esp32, sensor, display, image

    def dma_heaps():
        return esp32.idf_heap_info(1 << 3)  # ESP-IDF MALLOC_CAP_DMA.

    lcd = None
    frame = None
    canvas = None
    try:
        sensor.shutdown()
        gc.collect()
        lcd = display.Display(backlight=100)
        panel_width, panel_height = lcd.width(), lcd.height()
        lcd.deinit()
        lcd = None
        gc.collect()

        sensor.reset()
        sensor.set_pixformat(sensor.RGB565)
        sensor.set_framesize(sensor.QVGA)
        sensor.skip_frames(n=3)
        frame = sensor.snapshot()
        dma_before = dma_heaps()
        lcd = display.Display(backlight=100)
        lcd.write(frame, fit=True)
        dma_active = dma_heaps()
        lcd.deinit()
        lcd = None
        frame = None
        gc.collect()

        sensor.shutdown()
        gc.collect()
        dma_shutdown = dma_heaps()
        lcd = display.Display(backlight=100)
        canvas = image.Image(32, 32, image.RGB565)
        lcd.write(canvas, fit=True)
        lcd.deinit()
        lcd = None
        canvas = None
        gc.collect()

        sensor.reset()
        sensor.set_pixformat(sensor.RGB565)
        sensor.set_framesize(sensor.QVGA)
        sensor.skip_frames(n=3)
        frame = sensor.snapshot()
        restarted = (frame.width(), frame.height())
        dma_restarted = dma_heaps()
        return dict(width=panel_width, height=panel_height,
                    cameraWidth=restarted[0], cameraHeight=restarted[1],
                    dmaBefore=dma_before, dmaActive=dma_active,
                    dmaAfterShutdown=dma_shutdown, dmaAfterRestart=dma_restarted)
    finally:
        if lcd is not None:
            lcd.deinit()
        frame = None
        canvas = None
        sensor.shutdown()
        gc.collect()


def image_copy_to_fb(opts):
    """The unsupported copy_to_fb keyword must be rejected consistently."""
    import gc, image

    canvas = None
    try:
        try:
            image.Image(32, 32, image.RGB565, copy_to_fb=True)
        except TypeError:
            pass
        else:
            raise AssertionError('Image accepted unsupported copy_to_fb')

        canvas = image.Image(32, 32, image.RGB565)
        try:
            canvas.to_rgb565(copy_to_fb=True)
        except TypeError:
            pass
        else:
            raise AssertionError('to_rgb565 accepted unsupported copy_to_fb')
        canvas.draw_rectangle(0, 0, 32, 32, color=(255, 0, 0), fill=True)
        assert canvas.size() == 32 * 32 * 2, 'unexpected RGB565 buffer size'
        return dict(width=canvas.width(), height=canvas.height(), size=canvas.size())
    finally:
        canvas = None
        gc.collect()


def image_transform_argument(opts):
    """Transform is not part of the ESP-VISION Image API."""
    import image

    canvas = image.Image(32, 32, image.RGB565)
    source = image.Image(8, 8, image.RGB565)
    matrix = ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0))
    for operation in (
        lambda: canvas.copy(transform=matrix),
        lambda: canvas.draw_image(source, transform=matrix),
    ):
        try:
            operation()
        except TypeError as exc:
            assert 'keyword' in str(exc), 'transform is accepted but unsupported'
        else:
            raise AssertionError('Image accepted unsupported transform keyword')
    return dict(operations=2)


def sensor_snapshot_buffer(opts):
    """Validate the caller-owned writable buffer contract for sensor.snapshot."""
    import gc, sensor

    buffer = None
    frame = None
    try:
        sensor.shutdown()
        sensor.reset()
        sensor.set_pixformat(sensor.RGB565)
        sensor.set_framesize(sensor.QVGA)
        sensor.skip_frames(n=3)
        try:
            sensor.snapshot(buffer=bytearray(16))
        except TypeError:
            pass
        else:
            raise AssertionError('sensor.snapshot unexpectedly accepted buffer keyword')
        try:
            sensor.snapshot(bytearray(16))
        except ValueError as exc:
            assert 'snapshot buffer too small' in str(exc), 'unexpected undersized buffer error: ' + str(exc)
        else:
            raise AssertionError('sensor.snapshot accepted an undersized buffer')
        frame_bytes = 320 * 240 * 2
        buffer = bytearray(frame_bytes)
        frame = sensor.snapshot(buffer)
        assert (frame.width(), frame.height(), frame.size()) == (320, 240, frame_bytes), 'unexpected caller-buffer frame'
        buffer = None
        gc.collect()
        assert (frame.width(), frame.height()) == (320, 240), 'Image did not retain caller buffer after GC'
        return dict(width=frame.width(), height=frame.height(), size=frame.size())
    finally:
        frame = None
        buffer = None
        sensor.shutdown()
        gc.collect()


def memory_policy(opts):
    """Large Image storage follows MicroPython GC; camera snapshots use PSRAM."""
    import gc, esp32, image, sensor

    def free_bytes(caps):
        return sum(heap[1] for heap in esp32.idf_heap_info(caps))

    def measure_image(width, height):
        gc.collect()
        before = gc.mem_alloc()
        canvas = image.Image(width, height, image.RGB565)
        gc.collect()
        allocated = gc.mem_alloc() - before
        canvas = None
        gc.collect()
        return width * height * 2, allocated

    frame = None
    try:
        small_bytes, small_allocated = measure_image(32, 32)
        assert small_bytes - 512 <= small_allocated <= small_bytes + 8192, 'small Image allocation did not use GC heap'
        large_bytes, large_allocated = measure_image(640, 480)
        assert large_bytes - 4096 <= large_allocated <= large_bytes + 16384, 'large Image allocation did not use GC heap'

        sensor.shutdown()
        gc.collect()
        sensor.reset()
        sensor.set_pixformat(sensor.RGB565)
        sensor.set_framesize(sensor.QVGA)
        psram_before = free_bytes(1 << 10)  # ESP-IDF MALLOC_CAP_SPIRAM.
        if psram_before <= 0:
            raise RuntimeError('ESP_ERR_NOT_SUPPORTED: PSRAM heap unavailable')
        frame = sensor.snapshot()
        gc.collect()
        psram_after = free_bytes(1 << 10)
        frame_bytes = frame.width() * frame.height() * 2
        psram_used = psram_before - psram_after
        assert psram_used >= frame_bytes - 4096, 'camera snapshot buffer was not allocated from PSRAM'
        return dict(smallBytes=small_bytes, smallAllocated=small_allocated,
                    largeBytes=large_bytes, largeAllocated=large_allocated,
                    width=frame.width(), height=frame.height(), frameBytes=frame_bytes,
                    psramUsed=psram_used)
    finally:
        frame = None
        sensor.shutdown()
        gc.collect()



_model_bytes = None
_model_offset = 0
_model_path = None
_model_vfs = None
_model_nonce = None


class ModelVfs:
    """A single read-only RAM file; no Flash/SD volume or writer is touched."""
    def mount(self, readonly, mkfs):
        pass

    def umount(self):
        pass

    def stat(self, path):
        if path not in ('/', '/model.espdl'):
            raise OSError(2)
        return (0x4000 if path == '/' else 0x8000, 0, 0, 0, 0, 0, len(_model_bytes), 0, 0, 0)

    def open(self, path, mode):
        import io
        if path != '/model.espdl' or mode not in ('r', 'rb'):
            raise OSError(13)
        return io.BytesIO(_model_bytes)


def begin_model(nonce, size, path):
    global _model_bytes, _model_offset, _model_path, _model_nonce
    assert _model_bytes is None and _model_path is None, 'model already active'
    assert isinstance(nonce, str) and nonce, 'invalid model nonce'
    assert 0 < size <= 2 * 1024 * 1024, 'model size out of bounds'
    _model_nonce = nonce
    _model_bytes = bytearray(size)
    _model_offset = 0
    _model_path = path
    return dict(size=len(_model_bytes), offset=_model_offset, path=_model_path)


def append_model(offset, encoded):
    import binascii
    global _model_offset
    chunk = binascii.a2b_base64(encoded)
    end = offset + len(chunk)
    assert isinstance(offset, int) and offset >= 0 and chunk, 'invalid model chunk'
    assert _model_bytes is not None and end <= len(_model_bytes), 'model upload overflow'
    if offset == _model_offset:
        # An interrupt between copying and committing leaves the same offset;
        # replay safely overwrites the same slice before committing it.
        _model_bytes[offset:end] = chunk
        _model_offset = end
    else:
        assert _model_offset == end, 'model chunk out of order'
        assert _model_bytes[offset:end] == chunk, 'model chunk conflict'
    return dict(offset=offset, end=end, bytes=len(chunk), current=_model_offset)


def mount_model(expected):
    import binascii, hashlib, vfs
    global _model_vfs
    assert _model_bytes is not None and _model_offset == len(_model_bytes), 'incomplete model upload'
    actual = binascii.hexlify(hashlib.sha256(_model_bytes).digest()).decode()
    assert actual == expected, 'model SHA-256 mismatch'
    # MicroPython refuses an already mounted path; never unmount another VFS.
    _model_vfs = ModelVfs()
    vfs.mount(_model_vfs, _model_path, readonly=True)
    assert any(filesystem is _model_vfs and path == _model_path for filesystem, path in vfs.mount()), 'model mount missing'
    return dict(path=_model_path, bytes=len(_model_bytes), sha256=actual)


def close_model():
    import gc, vfs
    global _model_bytes, _model_offset, _model_path, _model_vfs, _model_nonce
    # A cancelled mount may have completed before the host observed its result.
    # Match object identity, so an uncertain call never unmounts another VFS.
    if _model_vfs is not None:
        for filesystem, _ in vfs.mount():
            if filesystem is _model_vfs:
                vfs.umount(_model_vfs)
        assert not any(filesystem is _model_vfs for filesystem, _ in vfs.mount()), 'model still mounted'
        _model_vfs = None
    _model_bytes = None
    _model_offset = 0
    _model_path = None
    _model_nonce = None
    gc.collect()
    return dict(bytes=0, offset=_model_offset, mounted=False)


def model_command(operation, *args):
    import json
    assert operation in ('begin', 'append', 'mount', 'close'), 'invalid model operation'
    nonce = globals()['_fixture_nonce']
    assert _model_nonce is None or _model_nonce == nonce, 'model nonce conflict'
    if operation == 'begin':
        assert args[0] == nonce, 'model nonce conflict'
    result = globals()[operation + '_model'](*args)
    # A caught/printed Python exception must never look like a semantic ACK.
    print('__EVHILACK__' + json.dumps([nonce, operation, result]))


def espdl_inference(opts):
    """Load the SHA-256-verified RAM model and run real camera inference."""
    import espdl, sensor, time
    det = None
    frame = None
    try:
        sensor.reset()
        sensor.set_pixformat(sensor.RGB565)
        sensor.set_framesize(getattr(sensor, opts['framesize']))
        sensor.skip_frames(n=3)
        started = time.ticks_ms()
        det = espdl.ESPDet(opts['modelPath'], score=0.3, nms=0.7)
        load_ms = time.ticks_diff(time.ticks_ms(), started)
        frame = sensor.snapshot().copy()
        width, height = frame.width(), frame.height()

        def checked(results):
            assert isinstance(results, list), 'detect() did not return a list'
            for item in results:
                assert len(item) == 6, 'malformed detection tuple'
                x, y, w, h, score, category = item
                assert w > 0 and h > 0, 'empty detection box'
                assert x < width and y < height and x + w > 0 and y + h > 0, 'detection box outside the frame'
                assert 0.0 <= score <= 1.0, 'detection score outside 0..1'
                assert category >= 0, 'negative detection category'
            return results

        first = checked(det.detect(frame))
        assert first == checked(det.detect(frame)), 'inference differs for the same input frame'
        detections, total_us, worst_us = 0, 0, 0
        for _ in range(opts['frames']):
            img = sensor.snapshot()
            start = time.ticks_us()
            results = det.detect(img)
            elapsed = time.ticks_diff(time.ticks_us(), start)
            detections += len(checked(results))
            total_us += elapsed
            worst_us = max(worst_us, elapsed)
        assert total_us > 0, 'implausible inference time'
        return dict(width=width, height=height, loadMs=load_ms, frames=opts['frames'], detections=detections,
                    meanMs=total_us // opts['frames'] / 1000, worstMs=worst_us / 1000)
    finally:
        try:
            if det is not None:
                det.deinit()
        finally:
            frame = None
            sensor.shutdown()


def isp_frame_sample(frame):
    """Measure rendered RGB565 pixels, independently of ISP configuration."""
    saturation = 0.0
    bright = 0
    rgb = [0, 0, 0]
    for y in range(8, frame.height(), 16):
        for x in range(8, frame.width(), 16):
            pixel = frame.get_pixel(x, y)
            maximum = max(pixel)
            if maximum >= 16:
                saturation += (maximum - min(pixel)) / maximum
                bright += 1
            for channel in range(3):
                rgb[channel] += pixel[channel]
    if bright < 16:
        raise RuntimeError('ISP image effect unobservable: illuminate a stationary target (at least 16 bright samples required)')
    return dict(saturation=saturation / bright, brightSamples=bright, rgb=rgb)


def isp_ccm_frames(opts):
    """One write per stage; consume old frames instead of repeating the write."""
    import sensor, time
    stages = []
    identity = [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0]
    grey = [0.33] * 9
    red = [1.0, 1.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0]

    def stage(name, matrix, delay=0):
        sensor.set_isp('ccm', enable=True, matrix=matrix)
        # Deliberately leave completed buffers queued; elapsed time is no flush.
        if delay:
            time.sleep_ms(delay)
        started = time.ticks_ms()
        frames = []
        for index in range(8):
            sample = isp_frame_sample(sensor.snapshot())
            sample.update(frame=index + 1, ms=time.ticks_diff(time.ticks_ms(), started))
            frames.append(sample)
        result = dict(name=name, writes=1, frames=frames, readback=sensor.get_isp('ccm'))
        stages.append(result)
        return frames

    try:
        sensor.reset()
        sensor.set_pixformat(sensor.RGB565)
        sensor.set_framesize(sensor.QVGA)
        sensor.skip_frames(time=1000)
        if sensor.get_ipa() is None or sensor.get_isp('ccm') is None:
            raise RuntimeError('ESP_ERR_NOT_SUPPORTED: manual ISP CCM unavailable')
        # Automatic controller ownership must reject manual writes explicitly.
        sensor.set_ipa(True)
        try:
            sensor.set_isp('ccm', enable=True, matrix=grey)
        except OSError as exc:
            ownership_error = str(exc)
            assert 'disable IPA before setting ISP parameters' in ownership_error, 'unexpected manual-write ownership error: ' + ownership_error
        else:
            raise RuntimeError('manual CCM write accepted while IPA owns tuning')
        sensor.set_ipa(False)
        sensor.set_isp('wb', enable=False)
        sensor.skip_frames(n=3)
        for name, matrix, delay, expect_grey in (('red', red, 0, False), ('grey_after_sleep', grey, 2000, True), ('red_again', red, 0, False), ('grey_again', grey, 0, True)):
            frames = stage(name, matrix, delay)
            # Never require the first captured frame to be from the new tuning.
            for sample in frames[-3:]:
                if expect_grey:
                    assert sample['saturation'] < 0.15, 'single CCM write did not produce grey frames after consumption'
                else:
                    assert sample['saturation'] > 0.8, 'single CCM write did not produce red frames after consumption'
                    assert sample['rgb'][0] > 4 * max(1, sample['rgb'][1], sample['rgb'][2]), 'CCM red output has wrong channels'
        sensor.set_isp('wb', enable=True, rg=1.5, bg=0.75)
        before = stage('explicit_wb', identity)
        wb_before = sensor.get_isp('wb')
        after = stage('ccm_preserves_explicit_wb', identity)
        wb_after = sensor.get_isp('wb')
        assert wb_before == wb_after, 'CCM write changed explicit WB driver configuration'
        for channel in (0, 2):
            ratios = [sum(s['rgb'][channel] for s in samples[-3:]) / max(1, sum(s['rgb'][1] for s in samples[-3:])) for samples in (before, after)]
            assert abs(ratios[0] - ratios[1]) < 0.2, 'CCM write changed rendered explicit WB on stationary target'
        return dict(sensorId=sensor.get_id(), stages=stages, wbBefore=wb_before, wbAfter=wb_after,
                    frameBudget=8, ownershipError=ownership_error,
                    note='Driver readback is not a frame acknowledgement; sensor-internal AWB is not measured')
    finally:
        sensor.shutdown()


def isp_heap_sample():
    import gc, esp32
    gc.collect()
    result = {}
    for name, caps in (('dma', 1 << 3), ('psram', 1 << 10), ('internal', (1 << 11) | (1 << 2))):
        heaps = esp32.idf_heap_info(caps)
        result[name] = dict(free=sum(item[1] for item in heaps), largest=max([item[2] for item in heaps] or [0]))
    return result


def isp_release_camera_lcd(lcd):
    import sensor, gc
    # A display cleanup failure must not strand the camera's DMA/LSC resources.
    try:
        if lcd is not None:
            lcd.deinit()
    finally:
        try:
            sensor.shutdown()
        finally:
            gc.collect()


def isp_lsc_display(opts):
    """LSC changes and CCM bursts during the original canvas/LCD preview path."""
    import sensor, display, image, time, gc
    lcd = None
    canvas = None
    frame = None
    sensor.shutdown()
    before = isp_heap_sample()
    result = {}
    try:
        sensor.reset()
        sensor.set_pixformat(sensor.RGB565)
        sensor.set_framesize(sensor.QVGA)
        sensor.skip_frames(n=3)
        if sensor.get_ipa() is None or sensor.get_isp('lsc') is None:
            raise RuntimeError('ESP_ERR_NOT_SUPPORTED: manual ISP LSC unavailable')
        sensor.set_ipa(False)
        lsc = sensor.get_isp('lsc')
        count = lsc['lsc_gain_size']
        assert count > 1, 'invalid LSC table size'
        ramp = [1.0 - 0.5 * (i / (count - 1)) ** 2 for i in range(count)]
        matrix = sensor.get_isp('ccm')['matrix']
        lcd = display.Display()
        canvas = image.Image(lcd.width(), lcd.height(), image.RGB565)
        scale = min(lcd.width() / 320, lcd.height() / 240)
        x = int((lcd.width() - 320 * scale) / 2)
        y = int((lcd.height() - 240 * scale) / 2)

        def write_frame():
            frame = sensor.snapshot()
            canvas.draw_image(frame, x, y, x_scale=scale, y_scale=scale)
            lcd.write(canvas)

        # Warm both persistent LSC banks and the display's reusable DMA staging.
        for _ in range(2):
            sensor.set_isp('lsc', enable=True, gain_r=ramp, gain_gr=ramp, gain_gb=ramp, gain_b=ramp)
            write_frame()
            sensor.set_isp('lsc', enable=False)
            write_frame()
        active = isp_heap_sample()
        samples = []
        frames = max(80, opts['frames'])
        sample_every = max(40, (frames + 9) // 10)
        started = time.ticks_ms()
        switches = 0
        for index in range(frames):
            if index % 20 == 0:
                sensor.set_isp('lsc', enable=(switches % 2 == 0), gain_r=ramp, gain_gr=ramp, gain_gb=ramp, gain_b=ramp)
                switches += 1
            sensor.set_isp('ccm', enable=True, matrix=matrix)
            sensor.set_isp('ccm', enable=True, matrix=matrix)
            write_frame()
            if (index + 1) % sample_every == 0 or index + 1 == frames:
                samples.append(dict(frame=index + 1, heap=isp_heap_sample()))
        after = isp_heap_sample()
        for name in active:
            assert after[name]['free'] >= active[name]['free'] - 4096, 'ISP/LCD pressure lost ' + name + ' heap'
            assert after[name]['largest'] >= active[name]['largest'] - 4096, 'ISP/LCD pressure fragmented ' + name + ' heap'
        # Rejected parameters must leave preview usable; do not induce OOM.
        try:
            sensor.set_isp('lsc', enable=True, gain_r=[1.0], gain_gr=ramp, gain_gb=ramp, gain_b=ramp)
        except ValueError:
            pass
        else:
            raise RuntimeError('invalid LSC table accepted')
        write_frame()
        result = dict(sensorId=sensor.get_id(), frames=frames, ccmWrites=2 * frames, lscSwitches=switches,
                      gainCount=count, elapsedMs=time.ticks_diff(time.ticks_ms(), started),
                      before=before, active=active, samples=samples, after=after,
                      rejectedWriteRecovered=True, note='Software preview and allocation evidence; physical LCD requires observation')
    finally:
        try:
            isp_release_camera_lcd(lcd)
        finally:
            lcd = None
            canvas = None
            frame = None
            gc.collect()
    result['released'] = isp_heap_sample()
    for name in before:
        assert result['released'][name]['free'] >= before[name]['free'] - 4096, 'ISP/LCD cleanup leaked ' + name + ' heap'
    # Prove that normal cleanup permits another camera/LCD lifetime.
    try:
        sensor.reset()
        sensor.set_pixformat(sensor.RGB565)
        sensor.set_framesize(sensor.QVGA)
        lcd = display.Display()
        lcd.write(sensor.snapshot(), fit=True)
        result['restartRecovered'] = True
    finally:
        isp_release_camera_lcd(lcd)
    result['afterRestartRelease'] = isp_heap_sample()
    return result


def modules(opts):
    """Known-answer module operations on the target, without storage writes."""
    import json
    import binascii
    import hashlib
    import struct
    import io
    import gc
    payload = bytes(range(256))
    assert binascii.unhexlify(binascii.hexlify(payload)) == payload, 'hex roundtrip'
    assert binascii.a2b_base64(binascii.b2a_base64(payload)) == payload, 'base64 roundtrip'
    digest = binascii.hexlify(hashlib.sha256(b'abc').digest()).decode()
    assert digest == 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad', 'SHA-256 known answer'
    packed = struct.pack('<Ih', 0x12345678, -1234)
    assert packed == b'\x78\x56\x34\x12\x2e\xfb', 'struct byte order'
    assert struct.unpack('<Ih', packed) == (0x12345678, -1234), 'struct unpack'
    value = dict(text='Vision-视觉', values=[0, -1, 65535], enabled=True)
    assert json.loads(json.dumps(value)) == value, 'JSON Unicode roundtrip'
    stream = io.BytesIO()
    try:
        assert stream.write(payload) == len(payload), 'BytesIO short write'
        stream.seek(17)
        assert stream.read(31) == payload[17:48], 'BytesIO seek/read'
    finally:
        stream.close()
    gc.collect()
    assert gc.mem_free() > 0, 'GC free heap'
    return dict(modules=['json', 'binascii', 'hashlib', 'struct', 'io', 'gc'], sha256=digest, binaryBytes=len(payload))


def ulab(opts):
    from ulab import numpy as np
    values = np.array([1.0, 2.0, 3.0, 4.0])
    assert abs(float(np.mean(values)) - 2.5) < 0.0001, 'ulab mean'
    assert abs(float(np.sum(values * values)) - 30.0) < 0.0001, 'ulab multiply/sum'
    assert abs(float(np.sqrt(values)[3]) - 2.0) < 0.0001, 'ulab sqrt'
    assert list(np.sort(np.array([3, 1, 4, 2]))) == [1, 2, 3, 4], 'ulab sort'
    return dict(elements=4, mean=2.5, squaredSum=30.0, sqrtLast=2.0)


def memory(opts):
    import esp32
    heaps = esp32.idf_heap_info(esp32.HEAP_DATA)
    psram = esp32.idf_heap_info(1 << 10)  # ESP-IDF MALLOC_CAP_SPIRAM.
    free = sum(item[1] for item in heaps)
    assert free > 0, 'no heap left'
    return dict(heapFree=free, heapLargest=max([item[2] for item in heaps] or [0]),
                psramFree=sum(item[1] for item in psram),
                psramLargest=max([item[2] for item in psram] or [0]))


def sensor_state(opts):
    import sensor
    width, height = sensor.width(), sensor.height()
    assert width > 0 and height > 0, 'sensor not ready after capture'
    return dict(width=width, height=height, id=sensor.get_id(),
                pixformat=sensor.get_pixformat(), hmirror=sensor.get_hmirror(),
                vflip=sensor.get_vflip())


def run_check(name, opts):
    import json
    if isinstance(opts, str):
        opts = json.loads(opts)
    functions = dict(modules=modules, ulab=ulab, memory=memory, camera=camera, sensor_state=sensor_state, framesizes=framesizes,
                     pixformats=pixformats, orientation=orientation, ipa_stats=ipa_stats,
                     snapshot_rate=snapshot_rate, preview=preview, image_ops=image_ops,
                     display=display, display_stream=display_stream,
                     camera_display_lifecycle=camera_display_lifecycle,
                     image_copy_to_fb=image_copy_to_fb,
                     image_transform_argument=image_transform_argument,
                     sensor_snapshot_buffer=sensor_snapshot_buffer,
                     memory_policy=memory_policy, espdl_inference=espdl_inference,
                     isp_ccm_frames=isp_ccm_frames, isp_lsc_display=isp_lsc_display)
    try:
        data = functions[name](opts)
        result = dict(name=name, status='PASS', data=data)
    except Exception as exc:
        message = str(exc)
        status = 'SKIP' if is_unsupported(exc) else 'FAIL'
        result = dict(name=name, status=status, error=dict(type=type(exc).__name__, message=message))
    print('__EVDEVICECHECK__' + json.dumps(result))
