编解码与推流
============

:link_to_translation:`en:[English]`

拿到图像帧后，常常需要把它 *送出开发板*\ ：在主机上预览、录制，或实时推流。
ESP-VISION 提供多种编解码与传输方式，各自适用于不同场景。

静态图像编码：JPEG 与 PNG
-------------------------

:py:meth:`image.Image.to_jpeg`\ （及其别名 :py:meth:`image.Image.compress`\ ）把一帧
编码为 JPEG。JPEG 有损：``quality`` 关键字（1-100）在大小与画质之间权衡，
``subsampling`` 控制色度被压缩的程度（``JPEG_SUBSAMPLING_420`` 最小，``444`` 画质
最好）。带硬件 JPEG 编码器的板会进行硬件卸载，否则使用软件编码器
（``esp_new_jpeg``\ ）。:py:meth:`image.Image.to_png` 产生无损 PNG，体积更大但精确，
适合保存参考帧。

录制序列：ImageIO
-----------------

:py:class:`imageio.ImageIO` 录制一 *序列* 帧并保留帧间时间，从而以原始速度回放。
**文件流** 在存储上写容器文件；**内存流** 把帧保存在预分配的 PSRAM 缓冲中，以便快速
采集与回放。详见 :doc:`../api-reference/imageio`\ 。

.. only:: esp32p4

   视频编码：H.264
   ---------------

   对于连续视频，逐帧 JPEG 很浪费，因为它无法利用相邻帧之间的相似性。
   :py:class:`h264.H264Encoder` 产生 H.264 流，其中偶发的 **关键帧**\ （IDR/I）
   是自包含的，而其间的帧只编码发生变化的部分。编码器参数用于调节这一权衡：

   - ``gop`` —— 关键帧间隔。越短则恢复越快、越易跳转，但输出越大；``0`` 表示
     每秒一个关键帧。
   - ``bitrate`` —— 目标每秒比特数；大小/画质的主要控制项。
   - ``qp_min`` / ``qp_max`` —— 限定每帧量化参数的范围。

   丢失一个 P 帧会导致画面损坏，直到下一个关键帧为止，这对下面的推流传输很重要。

   网络推流：RTSP
   --------------

   :py:class:`rtsp.RTSPServer` 通过 RTSP 发送编码后的 H.264 帧，使 VLC、ffplay 等
   客户端能在 ``rtsp://<board-ip>:8554/`` 观看摄像头画面。典型循环是
   *采集 → 编码 → 发送*\ ：把每个 :py:meth:`h264.H264Encoder.encode` 的结果交给
   :py:meth:`rtsp.RTSPServer.send`\ 。服务器维护一个短小且有序的帧队列；若客户端
   缓慢或缺席，它会整帧丢弃，而不是阻塞采集循环或发送会破坏码流的半帧。

主机预览：USB CDC
-----------------

在开发期间，*看到* 摄像头画面最快的方式是 :py:meth:`image.Image.flush`\ ，它通过
USB CDC 把当前帧作为 EVFRAME JPEG 预览推送到主机，由配套的主机工具解码并显示。该通路
无需 Wi-Fi、无需 SD 卡，是脚本迭代时默认的反馈回路。
