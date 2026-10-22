Device Core 主机 SDK
====================================

:link_to_translation:`en:[English]`

``@esp-vision/device-core`` 是 ESP-VISION EV-MUX v4 固件对应的无 UI JavaScript 主机 SDK。IDE、Device CLI 和自定义主机应用共用它的字节协议、会话所有权、保活、Python、文件与图像能力。Agent 工作流通过 CLI 控制设备；官方 Knowledge MCP 通过 HTTP 提供文档。SDK 源码在固件仓库 ``host/device-core`` 维护。

.. toctree::
   :maxdepth: 2

   api
   advanced
   integration
   cli

安装与兼容性
--------------------------------

.. code-block:: console

   npm install --save-exact @esp-vision/device-core@0.5.1
   # Optional Node LAN discovery adapter:
   npm install multicast-dns

本页示例面向 SDK ``0.5.1``；安装前请使用此精确版本并确认 registry 可用。Node 主机基线为 Node.js 22；浏览器需要打包器/import map，并由宿主提供 Web Serial 或二进制 WebSocket 字节连接。仅支持 EV-MUX v4，v3 设备必须升级固件。包版本、固件版本与 IDE 版本相互独立。

架构与使用入口
--------------------------------

.. figure:: ../../_static/esp-vision-architecture-adaptive.svg
   :align: center
   :alt: 主机应用、Device Core、EV-MUX 与固件

   IDE / Device CLI / 自定义主机应用 → Device Core → EV-MUX → 固件；Knowledge MCP → HTTP 文档

本图与项目 README、系统架构章节复用同一 SVG。主机应用负责设备选择及界面/流程策略，Device Core 负责共享 API、协议和会话状态，EV-MUX 定义与固件的运行时通信；字节适配器提供串口/socket I/O，不维护另一套协议栈。图中代表性 API 不替代完整参考；发现与 ROM 烧写各有独立生命周期，ROM loader 不通过 EV-MUX 写镜像。

根入口只导出 ``createDevice``，返回设备对象，不提供公开的 ``Device`` 构造函数。所有应用方法、选项、返回值和事件见 :doc:`api`；其余包导出及公开类方法完整列于 :doc:`advanced`，供集成维护者查阅。Agent 工作流通过 Device CLI 操作设备；Knowledge MCP 是独立的文档接口。

.. code-block:: javascript

   import { createDevice } from '@esp-vision/device-core'

   // transport is supplied by the host; its complete contract is in api.
   const device = createDevice({ transport })
   const off = device.on('python.output', text => console.log(text))
   try {
     await device.connect({ role: 'usb' })
     console.log(await device.python.execute({ source: 'print(1 + 2)' }))
   } finally {
     try { await device.disconnect() } finally { off() }
   }

可直接运行的 Node TCP 与浏览器 Web Serial 适配示例见 :doc:`integration`。帧格式、路由、所有权见 :doc:`index <../communication/index>`，统一真机测试入口见 :doc:`validation <../communication/validation>`。

许可证
----------------------------------------

npm 包及 ESP-VISION 自有贡献采用 Apache-2.0，与固件一致。包内包含 LICENSE 与 NOTICE；NOTICE 保留历史 MIT 版权与许可原文，第三方依赖仍遵守各自许可证。项目清单见 :doc:`../license/index`。

SDK 0.5.1 包含 Node 传输、串口/网络设备枚举、ROM 烧写、不可变发布快照和单 BIN 应用升级。``@esp-vision/device-core/node`` 导出这些宿主适配器及打包器所需的资产 URL。安装后可执行 ``npm --prefix node_modules/@esp-vision/device-core test`` 和 ``npm --prefix node_modules/@esp-vision/device-core run test:hardware -- --help``；缺少测试或空测试集将失败。应用 ``discover()`` 与 provider ``list()`` 的关系见 :doc:`integration`。
