图像处理
========

:link_to_translation:`en:[English]`

:doc:`../api-reference/image` 模块中的视觉算法来自 OpenMV 的 ``imlib`` 库，在本仓库
中以 ``components/imlib`` IDF 组件的形式维护。它们大致分为几类：点变换与几何变换、
邻域滤波、统计分析，以及特征检测。本页说明每一类的作用与适用场景。

典型处理流水线
--------------

多数颜色跟踪或检测脚本都遵循相同结构：

#. 用 :py:func:`sensor.snapshot` **采集** 一帧。
#. **预处理** 以降噪与归一化：用 :py:meth:`image.Image.to_grayscale` 转换、用
   :py:meth:`image.Image.gaussian` 平滑，必要时用 :py:meth:`image.Image.histeq`
   校正光照。
#. 用 :py:meth:`image.Image.binary` 按颜色阈值 **分割** 出感兴趣像素。
#. **分析** 结果：:py:meth:`image.Image.find_blobs`\ 、
   :py:meth:`image.Image.get_statistics` 或某个特征检测器。
#. 用绘图方法 **标注/响应**\ 。

阈值化与分割
------------

:py:meth:`image.Image.binary` 通过把每个像素与一组颜色阈值比较，把图像转换为掩膜
（阈值为何用 LAB 表示见 :doc:`image-model`\ ）。只要像素落入 *任一* 阈值即为“有效”，
因此可同时跟踪多种颜色。配套的 :py:meth:`image.Image.find_blobs` 在相同阈值上运行
8 连通分量标记，为每个连通区域返回一个 :py:class:`image.blob`\ ，包含质心、外接框、
像素数、朝向，以及圆度、实心度等形状描述子。

邻域（卷积）滤波
----------------

这些方法用像素 ``(2*ksize+1)`` 方形邻域的某个函数来替换该像素：

- :py:meth:`image.Image.mean` —— 盒式平均；快速模糊。
- :py:meth:`image.Image.gaussian` —— 用帕斯卡三角（二项式）近似高斯核的加权平均；
  标准降噪模糊。设 ``unsharp=True`` 则改为锐化。
- :py:meth:`image.Image.laplacian` —— 二阶导数边缘响应；设 ``sharpen=True`` 可叠加
  回原图以增强边缘。
- :py:meth:`image.Image.morph` —— 应用任意整数卷积核。``mul`` 与 ``add`` 缩放并偏置
  结果；``mul`` 默认为 ``1/sum(kernel)``\ ，以保持图像亮度。

它们都带有 ``threshold``/``offset``/``invert`` 关键字，可在一次遍历中把滤波变成
自适应阈值化操作。

秩滤波与保边滤波
----------------

秩滤波对邻域排序而非求平均，能在较少模糊边缘的前提下去噪：

- :py:meth:`image.Image.median` —— 取百分位（默认 0.5）值；对椒盐噪声效果极佳。
- :py:meth:`image.Image.mode` —— 取最常见值。
- :py:meth:`image.Image.midpoint` —— 在最小值与最大值之间按 ``bias`` 混合。
- :py:meth:`image.Image.bilateral` —— 同时按空间距离（``space_sigma``\ ）与颜色相似度
  （``color_sigma``\ ）加权的高斯模糊，平滑平坦区域的同时保持边缘锐利。

形态学
------

:py:meth:`image.Image.erode` 与 :py:meth:`image.Image.dilate` 用方形结构元收缩或扩张
（通常为二值图像的）有效像素；:py:meth:`image.Image.open` 与
:py:meth:`image.Image.close` 组合二者以去除小噪点或填补小孔。``threshold`` 参数控制
需要多少个邻居为有效，从而推广了经典二值算子。

直方图与统计
------------

:py:meth:`image.Image.get_histogram` 按通道对像素值分箱；返回的
:py:class:`image.histogram` 可计算 Otsu 阈值（:py:meth:`image.histogram.get_threshold`\ ）、
百分位以及完整统计量。:py:meth:`image.Image.get_statistics` 一次返回均值、中位数、
众数、标准差、四分位数与最小/最大值，便于在运行时自动整定阈值。

特征检测
--------

更高层的检测器用于发现几何结构：

- :py:meth:`image.Image.find_lines` 与 :py:meth:`image.Image.find_circles` 使用霍夫
  变换；其 ``threshold`` 是最小累加器分数，``*_margin`` 关键字用于合并近似重复的结果。
- :py:meth:`image.Image.find_rects` 定位四边形（适用于基准标记与屏幕）。
- :py:meth:`image.Image.find_qrcodes` 与 :py:meth:`image.Image.find_apriltags`
  检测并解码相应的标记类型；在提供相机内参时，AprilTag 还能返回 6 自由度位姿。

.. only:: esp32p4

   当前 ESP32-P4 板级配置还通过 ZXing-C++ 后端提供
   :py:meth:`image.Image.find_barcodes`\ 。

.. note::

   本构建禁用了 OpenMV 中受 GPL 许可的代码路径（``OMV_NO_GPL=1``\ ），因此少量上游
   算法被有意排除。每块板子的 ``imlib_config.h`` 决定编译哪些可选算法；未启用的方法
   会在调用时抛出异常。
