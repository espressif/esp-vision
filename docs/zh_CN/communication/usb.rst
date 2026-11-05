USB 接口、配置与磁盘命名
========================

:link_to_translation:`en:[English]`

本章说明 USB 配置和主机侧可见名称。EV-MUX 连接见 :doc:`interfaces`，恢复、备份和写入所有权见 :doc:`../storage/index`。

物理接口与功能
----------------

.. list-table:: USB 功能
   :header-rows: 1
   :widths: 22 38 40

   * - 接口
     - 主机功能
     - 配置与限制
   * - Serial/JTAG（``usj``）
     - 串口／调试及 ROM 下载入口。
     - 由 SoC 及 ``MICROPY_HW_ESP_USB_SERIAL_JTAG`` 控制，不提供 MSC 磁盘。TinyUSB 字符串不能重命名这个硬件接口。
   * - OTG CDC（``usb``）
     - 用于 EV-MUX、Python 和原生文件传输的串口。
     - 需要原生 USB device 连接、``MICROPY_HW_ENABLE_USBDEV`` 和 ``MICROPY_HW_USB_CDC``。USJ 可连接不代表 OTG 已连接。
   * - OTG MSC
     - 一个大容量存储接口，提供一个或两个 LUN。
     - 需要 ``MICROPY_HW_USB_MSC`` 和可用介质；与 CDC 及启用的 UVC 共用现有 TinyUSB device，不另挂载一套文件系统。
   * - OTG UVC
     - 受支持构建中的摄像头／视频功能。
     - 由 ``MICROPY_HW_USB_UVC`` 及现有描述符／流实现控制。枚举成功不代表相机已初始化或正在产帧。

根据板级原理图选择插座，枚举描述符、明确选择端口，再确认板型和固件身份。P4 板可能同时提供 USJ 与 OTG；复位或进入 ROM 后 COM／ttyACM 编号可能改变。下载模式交接见 :doc:`interfaces`。

配置位置
----------

.. list-table:: 配置归属
   :header-rows: 1
   :widths: 35 65

   * - 位置
     - 配置内容
   * - ``boards/<BOARD>/port/mpconfigboard.h``
     - Device／CDC／MSC／UVC 功能宏、USJ 开关、厂商／产品／接口字符串，以及板级 VID／PID 覆盖。
   * - ``boards/<BOARD>/boardconfig.h``
     - EV-MUX 默认／回退 sink、存储自动挂载／初始化策略。选择 sink 不会启用 USB 类或物理插座。
   * - 板级 ``sdkconfig.*`` 与共享 port 默认配置
     - IDF console、PHY／驱动和 FatFs 设置。``CONFIG_ESP_CONSOLE_*`` 不选择 EV-MUX owner，也不会让 USJ 提供 MSC。``CONFIG_FATFS_USE_LABEL`` 启用共享卷标 API。
   * - ``overlay/micropython/shared/tinyusb/``
     - 共用复合设备描述符和 class／endpoint 配置。保持唯一 TinyUSB 驱动／描述符所有者，不为磁盘另装 USB 栈。
   * - ``platform/usb_msc.c`` 与 ``platform/storage.h``
     - 固定 LUN 映射和介质名称。MSC inquiry product 为 ``ESP_FLASH``／``ESP_SDCARD``；``MICROPY_HW_USB_MSC_INTERFACE_STRING`` 保留各板原有名称，不强制统一前缀。

修改板级／overlay 源文件，通过仓库根目录的 ``idf.py --board <BOARD> ...`` 重新构建，不修改生成的 MicroPython 副本。VID／PID 必须符合实际 USB 功能和主机驱动约定；复制 Serial/JTAG PID 不会带来 JTAG 硬件能力。

卷标、LUN 与盘符
------------------

.. list-table:: 磁盘身份
   :header-rows: 1
   :widths: 20 30 25 25

   * - 介质
     - Python 挂载位置
     - 默认 FAT 卷标／inquiry product
     - USB LUN
   * - 内部 Flash
     - ``/`` 及别名 ``/flash``
     - ``ESP_FLASH``
     - 0
   * - SD 卡
     - 默认 ``/sdcard``
     - ``ESP_SDCARD``
     - 编译支持 SD 时为 1

FAT 卷标保存在介质上，通常由主机文件管理器显示。固件在自动挂载时，只为卷标为空的已挂载卷设置默认名；保留已有非空卷标，包括用户自定义 SD 卡名称。改名不会格式化磁盘。写入方准入拒绝或卷标 I/O 失败会记录警告，不导致 crash，也不把成功挂载转变为格式化请求。显式 ``storage.retry()`` 和写入方交接刷新仍只挂载，不修改卷标。

FAT 卷标最多 11 个 OEM 字节、不允许点号，小写转换为大写；长文件名不能放宽卷标限制。因此 ``esp-vision.flash``／``esp-vision.sdcard`` 不能作为 FAT 卷标，实际使用 ``ESP_FLASH``／``ESP_SDCARD``。参见 `FatFs 卷标 API <https://elm-chan.org/fsw/ff/doc/setlabel.html>`_。

USB 厂商／产品／接口字符串标识设备或功能；SCSI inquiry product 标识 LUN，字段为 16 字节。它们均不会设置 FAT 卷标。Windows 自行分配 ``D:``／``E:`` 盘符，其他系统自行选择挂载路径，固件不能预留这些盘符。识别磁盘应综合物理设备、LUN 和卷元数据，不能只记盘符或仅凭卷名判断目标。

Windows 资源管理器中的磁盘名称通常来自文件系统卷标。原先 Function EV 的 Flash 卷标为空，因此显示“U 盘”；``ESP32-P4X-Function-EV-Board Flash`` 是 USB MSC 接口字符串，描述同一个包含两个 LUN 的接口，不会成为每个卷的名称。SCSI inquiry product 可用于磁盘设备身份识别，同样不设置卷标。现在为无标签的已挂载卷设置 ``ESP_FLASH``／``ESP_SDCARD``，即可区分两种介质；已有自定义卷标仍保留。无卡的 SD 槽没有可读取的 FAT 卷标，Windows 仍可能显示通用“U 盘”，不能仅靠接口字符串为该空槽指定资源管理器名称。参见 Microsoft 的 `卷命名 <https://learn.microsoft.com/en-us/windows/win32/fileio/naming-a-volume>`_ 与 `USB 接口描述符 <https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/usbspec/ns-usbspec-_usb_interface_descriptor>`_。

不可用介质在其固定槽位报告未就绪；Flash 失败不会把 SD 改为 LUN 0，``/flash`` 不增加磁盘。不支持 SD 的板型只提供 Flash。空槽是否显示及盘符分配由主机决定。

挂载、所有权与验证
--------------------

Flash 和 SD 自动挂载互不依赖，默认均开启。无法识别文件系统时是否格式化属于另一套策略：默认 ``ESP_VISION_STORAGE_FLASH_FORMAT_IF_NO_FILESYSTEM=1``、``ESP_VISION_STORAGE_SD_FORMAT_IF_NO_FILESYSTEM=0``。磁盘命名不覆盖这些策略，也不授权格式化，参见 :doc:`../storage/index`。

EV-MUX owner 不自动拥有 MSC 写权限。Python／IDE 写入或备份前，应安全弹出对应操作系统卷，并按 :doc:`python-files` 完成逐卷交接。双盘在线不允许主机／VM 同时写入任一共享 FAT 文件系统；重要文件须保留经校验的设备外备份。

1. 检查选定设备及 ``capabilities.sinks``。MSC 验收需要连接原生 USB device 端口；格式化卡不能解决 OTG 未连接。
2. 检查 ``storage.status("flash")``／``storage.status("sd")``，卷已挂载且主机写入方准入成功后，对应 LUN 才可就绪。
3. 分别检查 USB 功能与逻辑磁盘，记录物理序列号、固件、LUN、实际卷标及系统盘符。仅看到 COM 口或产品字符串不能证明磁盘已挂载。
4. 用独立测试文件／哈希验证读写和复位后持久化，保留既有数据及自定义卷标。不可用介质应返回诊断，另一卷／控制连接仍可使用。
5. 分别报告 CDC、实际双 LUN 枚举和健康 SD 读写结果；仅在 USJ 上通过不能覆盖原生 USB MSC 或双盘。
