通信验收、闭环测试与故障定位
============================

:link_to_translation:`en:[English]`

验收证据的四个层级
------------------

.. blockdiag::

   blockdiag {
     node_width = 200;
     node_height = 60;
     orientation = portrait;
     link [label = "1. physical link opened"];
     owner [label = "2. correlated acquire\ncurrent owner / limits"];
     command [label = "3. accepted / queued / complete ACK"];
     app [label = "4. application evidence\nnew marker / file hash / JPEG"];
     link -> owner -> command -> app;
   }

**图 24：下一级成功不能替代上一级的应用证据。** 打开串口、接管成功、上传 ACK 与摄像头真的拍到一张图，分别是四种结论。

只证明控制面能响应，不足以报告文件、脚本、preview 或完整硬件矩阵通过。真实验收记录应包括板卡/物理身份、安装固件提交、所用端点、当前 session/epoch、请求阶段、时间预算和应用级证据。

建议的验收矩阵
--------------

.. list-table:: 按功能与边界验证
   :header-rows: 1
   :widths: 26 44 30

   * - 类别
     - 操作
     - 成功依据
   * - framing
     - 分片 metadata/payload，连续粘包，CRC 错误，payload 含 SOF/EOF，UTF-8 跨帧。
     - 仅完整合法帧交付，错误有界重同步，正文没有被分隔字节截断。
   * - 会话
     - 两连接争用、同 nonce acquire 重试、release 重接管、租约到期、TCP 槽位复用。
     - 唯一 owner，正确错误码，旧 link/session 输入输出不进入新连接。
   * - VM
     - 当前脚本占用、Ctrl-C、软复位、新 epoch ready、旧输出延迟到达。
     - 真实 REPL 新标记，旧 epoch 不能完成新请求。
   * - 文件
     - 空文件、中文/二进制、分块、64 KiB、取消、发布结果未知。
     - bytes/文件大小/内容 hash 校验，原文件及保护文件不被破坏。
   * - preview
     - 持续 JPEG、慢读者、串口切换/重连、与 RPC 交错。
     - 完整图像、正确尺寸和当前范围；允许 preview 丢帧但不接受旧 owner 数据。
   * - 网络
     - 配置 accepted 与最终 IP、关闭/恢复、FIN/RST、静默客户端。
     - 最终事务状态、owner/slot 分别回收，新连接可恢复；未知业务不重放。
   * - 下载/复位
     - Python 请求、USB control 请求、ROM 枚举、写入/读回、运行态重接管。
     - 分别证明模式切换、镜像校验与新运行态身份，而非仅观察端口消失。

使用真实安装固件的版本记录，不把“从源码推断”“CI 编译通过”“在另一块板运行过”写成当前板卡已经通过真机验收。浏览器字节桥测试也应说明是否覆盖原生串口 chooser/DTR。

统一 Core 真机检查
------------------

设备协议与业务真机测试统一运行 ``host/device-core`` 中的 ``npm run test:hardware``，直接调用 ``createDevice()`` 的公开 API。摄像头、文件和 LCD 均走同一个 Core，不再维护独立的主机协议客户端或 Agent smoke 入口。串口适配器只传字节，TCP 使用 Node socket；无需安装 Agent。

使用 Node.js 22。串口模式需要 Python 与 ``pyserial``；``--python`` 可指定解释器。所有输出放在项目目录中；以下从仓库根目录执行：

.. code-block:: console

   npm --prefix host/device-core test
   npm --prefix host/device-core run check:schema
   npm --prefix host/device-core run check:package
   npm --prefix host/device-core run test:hardware -- --serial <USJ-port> --role usj --checks all --timeout 600000
   npm --prefix host/device-core run test:hardware -- --serial <USB-OTG-port> --role usb --checks identity,camera,display
   npm --prefix host/device-core run test:hardware -- --tcp <device-ipv4>:3232 --checks identity,memory,camera

``--checks`` 接受 ``all`` 或逗号分隔的检查名。保留的摄像头/显示检查为 ``identity``、``memory``、``camera``、``framesizes``、``pixformats``、``orientation``、``ipa_stats``、``snapshot_rate``、``preview``、``image_ops``、``display``、``display_stream``。``all`` 选择完整检查集，不表示所有板卡都具备对应硬件；报告应明确区分通过、失败与缺少能力的跳过，不能将跳过算作已通过。初次实验只测试当前硬件，最终提交才由 CI 运行完整固件矩阵。

摄像头闭环要求当前会话中新的完整 stdout marker 或经校验的 JPEG。Core 管理上传、执行和关联，测试继续检查像素格式、尺寸、方向、IPA 状态、快照速率、preview、图像处理及显示。握手、上传 ACK、执行排队或 REPL 提示符本身都不能替代业务完成证据。长期运行时持续接收数据并处理 ``VM_BUSY`` / ``VM_TIMEOUT``。

LCD 抓屏与图案核验
------------------

``--screen-output`` 显示已知测试图案，通过 ``device.capture({ source: 'screen', format: 'jpeg' })`` 读取 LCD 最后一帧 RGB565 的 JPEG，再保存到指定路径。预先创建项目内输出目录：

.. code-block:: console

   mkdir -p /home/libo/project/.esp-vision/hardware-validation
   npm --prefix host/device-core run test:hardware -- --serial <USJ-port> --role usj --checks display --screen-output /home/libo/project/.esp-vision/hardware-validation/lcd.jpg

抓屏调用固件 ``debug.capture_screen``，在 VM 任务中读取显示缓冲区，不调用摄像头。应核验 JPEG 可解码、尺寸与图案符合预期，并对照设备真实屏幕观察。LCD 未初始化返回 ``DISPLAY_NOT_READY``；无 LCD 或该固件不支持时返回 ``NOT_SUPPORTED``，不得改用摄像头掩盖失败。抓屏反映软件提交的最后显示帧，不证明物理面板、电缆或背光正常。

这些检查会停止当前 Python 程序，并可能改变显示内容。安排独占设备时段，不与 IDE/其他测试同时持有设备。设备有有效 owner 时应释放或等待租约，不通过换接口抢占。测试使用独立临时文件并清理自己的文件；保存报告、JPEG 和日志时记录实际固件、板卡与接口。复位、掉线和取消后保留未知业务状态，不自动重放写入。

测试入口与职责
--------------

.. list-table:: 一套协议实现，分层验证
   :header-rows: 1
   :widths: 40 60

   * - 入口
     - 覆盖范围
   * - ``host/device-core: npm test``
     - Core 公开 API、协议、会话、发现及烧写契约的确定性主机测试。
   * - ``host/device-core: npm run test:hardware``
     - 串口/TCP 实际连接、摄像头及显示闭环，同一个公开 Core。
   * - ``tools/ci/flash_and_test.py``
     - CI 板卡/IDF 产物选择与烧写，随后调度 Core 真机检查；不另实现 EV-MUX。
   * - ``tools/tests/``
     - 固件主机原生状态机、JSON、队列、存储及复位契约。

故障定位顺序
------------

.. list-table:: 现象与下一步
   :header-rows: 1
   :widths: 30 70

   * - 现象
     - 先检查
   * - 串口有字但 hello 超时
     - 是否为原始启动/panic 文本；选错 USB 接口；startup 半帧；EV-MUX 是否被运行期关闭；帧 CRC/字节长度与 reader 是否持续运行。
   * - USB 已配置却不能接管
     - capabilities 与有效 owner；不要拿 DTR=false 当成 v4 不可用条件。
   * - ``SESSION_BUSY``
     - 哪个 IDE/Agent/终端仍保活；先显式释放或等待租约，不修改 route 来抢占。
   * - vmReady=true 但文件/抓帧超时
     - boot/main.py 是否仍运行；VM 队列是否忙；中断后是否打印了真实的新 REPL marker。
   * - 收到 seq 相同的数据却 RPC 未完成
     - type/channel/method/session/epoch 是否符合请求；是否拿 event 代替 rsp。
   * - preview 偶尔跳帧
     - JPEG 是否完整、是否属于当前范围；慢读者和 latest-frame 策略是否符合预期。
   * - TCP 断线后两槽耗尽
     - handshake/lease/30 秒 idle 各自状态，旧 generation 是否被回收，而不是仅看 owner 已释放。
   * - 下载请求后端口消失
     - ROM 是否重新枚举，USJ 是否有 ROM 日志，下载位是否按掩码设置/清除，原句柄是否已失效。
   * - 全局 drop 增长但 TCP 标记通过
     - 未读默认/回退口、队列类别和采样范围；不要把不同端点的计数等同于这个业务失败。

只保存必要的状态与标量计数。SSID/密码、完整文件内容和 Flash/NVS 备份可能含敏感信息，不应混入公开诊断报告；占位示例不代表设备实际配置。

文档与实现的同步边界
--------------------

若新增 channel、后端、控制 action 或修改 stderr/所有权/限额，应同时更新本章相关页、capabilities、参考客户端与契约测试。仅修改图中的名称不构成协议变更；新方法还须有实际固件分发实现。

固件单元测试与 Core 验收的边界
-------------------------------

``tools/tests/test_ev_*.py`` 在主机上借助 ESP-IDF/FreeRTOS 替身编译运行生产 C 逻辑，不连接设备，也不实现另一套主机协议客户端。保留部分写入、队列背压、分配失败、旧连接 generation、会话过期和存储交接等确定性覆盖。配套 C 测试和 stub 头文件属于测试基础设施，不是另一套设备操作工具。

用户可见功能和真机验收统一走 Device Core；新增连接、Python、文件、preview 和抓图场景使用公开 API。固件故障注入仍与固件同处维护，因为普通 SDK 请求无法稳定强制任务调度、内存分配或驱动失败。源码字符串断言仅作为补充，不能替代行为执行和真实硬件验收。
