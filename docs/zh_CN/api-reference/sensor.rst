sensor -- 相机
==============

:link_to_translation:`en:[English]`

.. py:module:: sensor

``sensor`` 模块用于控制相机并采集帧，与 OpenMV ``sensor`` API 对齐。本页内容以
``stubs/sensor.pyi`` 为准。

示例
----

.. code-block:: python

   import sensor

   sensor.reset()
   sensor.set_pixformat(sensor.RGB565)
   sensor.set_framesize(sensor.QVGA)
   sensor.skip_frames(time=2000)

   img = sensor.snapshot()

常量
----

.. py:data:: GRAYSCALE
.. py:data:: RGB565

   传给 :py:func:`set_pixformat` 的像素格式常量。

.. py:data:: QQVGA
.. py:data:: QVGA

   传给 :py:func:`set_framesize` 的帧尺寸常量。

函数
----

.. py:function:: reset()

   使用板级默认传感器配置复位并初始化相机。

.. py:function:: shutdown(enable=True)

   停止或重启相机数据流。

   :param bool enable: ``True`` 关闭相机；``False`` 重新启动。

.. py:function:: set_pixformat(pixformat)

   选择输出像素格式。

   :param int pixformat: :py:data:`GRAYSCALE` 或 :py:data:`RGB565`。

.. py:function:: get_pixformat()

   :return: 当前像素格式常量。
   :rtype: int

.. py:function:: set_framesize(framesize)

   选择输出帧尺寸。

   :param int framesize: :py:data:`QQVGA` 或 :py:data:`QVGA`。

.. py:function:: get_framesize()

   :return: 当前帧尺寸常量。
   :rtype: int

.. py:function:: width()

   :return: 当前输出图像宽度（像素）。
   :rtype: int

.. py:function:: height()

   :return: 当前输出图像高度（像素）。
   :rtype: int

.. py:function:: get_id()

   :return: 相机传感器 ID。
   :rtype: int

.. py:function:: set_hmirror(enable)

   水平镜像相机图像。

   :param bool enable: ``True`` 启用水平镜像。

.. py:function:: get_hmirror()

   :return: 是否已启用水平镜像。
   :rtype: bool

.. py:function:: set_vflip(enable)

   垂直翻转相机图像。

   :param bool enable: ``True`` 启用垂直翻转。

.. py:function:: get_vflip()

   :return: 是否已启用垂直翻转。
   :rtype: bool

.. py:function:: skip_frames(time=0, n=0)

   在相机曝光与处理稳定期间丢弃若干帧。

   :param int time: 等待的毫秒数。
   :param int n: 跳过的帧数。

.. py:function:: snapshot(buffer=None)

   从相机采集一帧图像。

   :param buffer: 为兼容性保留；当前版本请传 ``None``。
   :return: 采集到的图像。
   :rtype: image.Image

.. py:function:: status()

   :return: 相机就绪状态、尺寸、格式、镜像、翻转与裁剪信息，返回包含
            ``ready``、``id``、``width``、``height``、``pixformat``、``hmirror``、
            ``vflip``、``raw_width``、``raw_height``、``active_width``、
            ``active_height`` 等键的字典。
   :rtype: dict
