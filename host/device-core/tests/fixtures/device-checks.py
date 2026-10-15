# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Device-side camera/LCD fixtures migrated from tools/ci/device_checks.py.

All transport, ownership and cleanup are supplied by the public JS SDK.
"""


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
        except Exception:
            results.append(dict(name=name, supported=False))
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
        except Exception:
            results.append(dict(name=name, supported=False))
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
    functions = dict(memory=memory, sensor_state=sensor_state, framesizes=framesizes,
                     pixformats=pixformats, orientation=orientation, ipa_stats=ipa_stats,
                     snapshot_rate=snapshot_rate, preview=preview, image_ops=image_ops,
                     display=display, display_stream=display_stream)
    try:
        data = functions[name](opts)
        result = dict(name=name, status='PASS', data=data)
    except Exception as exc:
        message = str(exc)
        unsupported = ('ESP_ERR_NOT_SUPPORTED', 'ESP_ERR_NOT_FOUND', 'no module named', 'ENODEV')
        status = 'SKIP' if any(token.lower() in message.lower() for token in unsupported) else 'FAIL'
        result = dict(name=name, status=status, error=dict(type=type(exc).__name__, message=message))
    print('__EVDEVICECHECK__' + json.dumps(result))
