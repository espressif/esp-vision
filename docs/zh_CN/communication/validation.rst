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

使用 Node.js 22。串口模式使用 npm 原生绑定，无需主机 Python。所有输出放在项目目录中；以下从仓库根目录执行：

.. code-block:: console

   npm --prefix host/device-core test
   npm --prefix host/device-core run check:schema
   npm --prefix host/device-core run check:package
   npm --prefix host/device-core run test:hardware -- --serial <USJ-port> --role usj --checks all --timeout 600000
   npm --prefix host/device-core run test:hardware -- --serial <USB-OTG-port> --role usb --checks identity,camera,display
   npm --prefix host/device-core run test:hardware -- --tcp <device-ipv4>:3232 --checks identity,memory,camera

``--checks`` 接受 ``all`` 或逗号分隔的检查名。摄像头/显示检查包括 ``identity``、``memory``、``camera``、``framesizes``、``pixformats``、``orientation``、``ipa_stats``、``snapshot_rate``、``preview``、``image_ops``、``display``、``display_stream``、``camera_display_lifecycle``、``image_copy_to_fb``、``image_transform_argument``、``sensor_snapshot_buffer``、``memory_policy``、``espdl_inference``、``isp_ccm_frames`` 和 ``isp_lsc_display``。``all`` 选择完整检查集，不表示所有板卡都具备对应硬件；报告应明确区分通过、失败与缺少能力的跳过，不能将跳过算作已通过。初次实验只测试当前硬件，最终提交才由 CI 运行完整固件矩阵。

ISP 图像与内存检查
~~~~~~~~~~~~~~~~~~

调查 ISP 时使用静止且有足够照明的目标，并指定准确板型和实际安装的固件版本：

.. code-block:: console

   npm --prefix host/device-core run test:hardware -- --serial <USB-OTG-port> --role usb --checks isp_ccm_frames,isp_lsc_display --expect-board ESP32_P4X_EYE --expect-firmware-version <installed-version> --frames 400 --require-all

``isp_ccm_frames`` 对受控的红色/灰度 CCM 矩阵各单写一次，记录连续八张实际图像的帧序号、耗时、RGB 总和及平均饱和度。其中一次灰度采集故意在写入后休眠两秒：第一张仍可能来自旧缓冲，但持续消费后应在有限序列内看到新效果。检查还验证 IPA 占用时拒绝手动写入，以及重写单位 CCM 后显式 ISP 白平衡增益的读回值和实际 RGB 比例保持一致。比例对照要求目标静止，不测量或冻结传感器内部自动白平衡。亮度足够的采样点少于十六个时，检查会以效果不可观测失败，不将暗图当作 CCM 生效证据。

``isp_lsc_display`` 使用 QVGA RGB565、驱动报告的 LSC 表长度和原报告从 1.0 到 0.5 的二次增益曲线，将摄像头图像居中绘制到全屏 canvas 后调用 ``lcd.write()``。预热两个 LSC 表存储区和显示暂存区后至少执行八十帧（``--frames`` 可增加帧数），每帧双写 CCM，每二十帧切换 LSC。报告包含 DMA/内部/PSRAM 的空闲量与最大连续块、非法表被拒绝后恢复显示、资源释放及新一轮摄像头/LCD 初始化。内存历史最多保留十个采样点，避免观测记录持续增长。小幅内存容差允许无关分配；长时间测试应结合完整序列检查趋势。这属于软件图像与资源证据；判断物理面板是否仍花屏，需要在 LSC 切换时观察真实 LCD，检查通过或软件截图均不能单独证明。当前固件通过也不等于已经查明旧固件报告的根因。

CI target test 契约
~~~~~~~~~~~~~~~~~~~

CI 硬件门禁使用 ESP-IDF v5.5.6，在 P4 Function EV Board 上执行显式选定的 18 项运行时检查，包括 ``espdl_inference``。两项 ISP 检查保留为照亮的静止目标上的单独人工验收；新增注册检查不会静默扩大无人值守 CI 套件。CI 同时保存摄像头 JPEG 与 LCD 测试图案 JPEG，并将截图尺寸与活动显示对象核对。所有选中项必须通过（``--require-all``），不支持能力的 SKIP 也会使该板卡门禁失败。摄像头模式循环仅容忍明确的不支持错误；内存分配失败、超时及非预期的模式切换错误都会使检查失败。这只验证所选开发板与 IDF 组合，不代表编译矩阵中的其它板卡。驱动会将 ``--expect-board`` 和下载构建的 ``project_description.json`` 中的精确 ``project_version`` 同时传给启动探测与完整套件，在执行 Python、摄像头和显示检查前拒绝错误板型或旧固件。

通过 CI 验证单块开发板时，将 ``ESP_VISION_TEST_BOARD`` 和 ``ESP_VISION_TEST_IDF_IMAGE`` 设为固件矩阵中的准确组合，并显式设置 ``ESP_VISION_TEST_HARDWARE=1``。P4 Function EV Board 使用 ``ESP32_P4X_FUNCTION_EV_BOARD`` 和 ``v5.5.6``。此时只选择匹配的固件和硬件任务；未启用硬件开关的定向任务仅构建固件。``ESP_VISION_DEVICE_CORE_ONLY=1`` 仍禁用所有固件和硬件任务。

运行时辅助脚本通过有界的 ``python.execute()`` 调用装载到独立 RAM 命名空间。每条编码后命令都满足固件声明的 ``replInputMax`` （最多 511 字节）；更长的请求会触发 Device Core 的临时文件执行路径。成功、失败或取消后都会释放辅助命名空间和 LCD 资源，因此内存、摄像头和显示检查不依赖可写文件系统。

辅助脚本源码按 UTF-8 字节偏移传输，回执包含会话 nonce、偏移和实际提交的内容。相同分块可安全重复提交；内容冲突或无效回执立即失败。加载器在安装前验证拼接后的字节数和 SHA-256，安装后要求明确确认安装完成、入口函数可调用且源码缓冲已释放。只有 ``TIMEOUT`` 允许恢复，每个初始化操作最多通过公共 Python API 尝试三次，单次调用和整体超时保持不变。安装完成后的响应丢失可以恢复，且不重新执行辅助脚本；执行中断造成的部分安装会失败，不会重放。取消和语义错误均不重试。

初始化重试和失败会记录阶段（``init``、``chunk``、``assemble``、``verify`` 或 ``install``）、从零开始的命令序号、尝试次数、编码输入字节数、毫秒耗时及最后一个已确认操作；分块诊断还包含从零开始的分块序号、UTF-8 偏移和字节数。能力查询和主机源码读取失败单独标记阶段。公共执行接口超时可能发生在 SDK 同步或命令执行期间；这些诊断能定位初始化操作，但不能据此认定传输或固件层根因。报告不包含原始 Python 输出或源码。

辅助脚本不可用时，报告只记录一次 ``fixture-bootstrap`` 失败。所有依赖项（包括首个受影响的检查）均报告 ``BLOCKED``，携带 ``blockedBy: fixture-bootstrap`` 和 ``FIXTURE_UNAVAILABLE``；独立身份检查仍继续执行。汇总分别统计 ``passed``、``failed``、``skipped`` 和 ``blocked``。受阻检查既不算已执行失败，也不算能力不支持的跳过，即使未指定 ``--require-all``，硬件任务仍失败。清理使用独立取消信号；清理失败单独报告，不替换原始初始化失败或取消原因。

``espdl_inference`` 通过有界的公共 ``python.execute()`` 调用，将仓库自带的 495,840 字节 ESPDet Pico Face 模型装入只读 RAM VFS，在主机和设备两端验证 SHA-256，再通过真实 ``espdl.ESPDet`` 对 RGB565 摄像头图像推理。检查同一复制帧的结果可重复性、检测元组结构、检测框与图像重叠、置信度/类别范围及正的推理耗时；报告包含模型 SHA-256、传输分块数/耗时、加载耗时、帧数/检测数及平均/最慢推理延迟。镜头内没有人脸时允许空检测结果；这项合理性检查不代表模型准确率或性能指标已达标。无论成功、失败或取消，都会释放模型、RAM 挂载及摄像头，不读写 Flash/SD 文件，也不取得 MSC writer 租约。

模型上传通过命名空间 nonce 和准确的已提交字节偏移确认完成。只有超时分块允许重试，每块最多调用三次公共执行接口；重放只接受最近一个字节完全相同的分块。数据冲突或乱序、确认缺失或错误以及取消均立即失败。报告包含 ``modelRetries`` 和 ``modelRetryOffsets``。挂载、模型清理及最终命名空间不存在均要求显式确认或读回，不能将打印 Python 异常后执行包装器返回成功当作操作成功。各命令与整体的现有超时保持不变。

默认模型 SHA-256 为 ``04eec2220adbf642a843c953817fb219a06c094aedb76c181ba2a958c65201dc``。``--model MODEL.espdl`` 可以选择兼容的 ESPDet 模型（至多 2 MiB），并通过 ``--model-sha256 HEX`` 固定预期字节。已安装的 npm 包不携带模型资源，需要显式提供 ``--model``。模型缺失只会跳过可选验收，在 ``--require-all`` 下仍失败。严格推理验收要求摄像头和 ESP-DL 能力，与持久化存储是否可用无关。

存储单独取证。身份检查会报告各卷的存在、挂载及写入状态；运行时通过不代表文件上传可用。在已准备好文件系统的板卡上使用 ``--write``，显式要求 Flash 已挂载且可写，执行唯一临时文件的二进制写入与读回，并仅清理测试自己的文件。未挂载或不可用的存储会使该项以 ``STORAGE_UNAVAILABLE`` 失败。CI 和硬件 CLI 都不会格式化存储、通过擦除数据掩盖挂载失败，或用 SD 卡代替 Flash。

摄像头闭环要求当前会话中新的完整 stdout marker 或经校验的 JPEG。Core 管理上传、执行和关联，测试继续检查像素格式、尺寸、方向、IPA 状态、快照速率、preview、图像处理及显示。握手、上传 ACK、执行排队或 REPL 提示符本身都不能替代业务完成证据。长期运行时持续接收数据并处理 ``VM_BUSY`` / ``VM_TIMEOUT``。

LCD 抓屏与图案核验
------------------

``--screen-output`` 显示已知测试图案，通过 ``device.capture({ source: 'screen', format: 'jpeg' })`` 读取 LCD 最后一帧 RGB565 的 JPEG，再保存到指定路径。先将 ``ESP_VISION_WORK_DIR`` 设置为检出目录外可写的工作目录，再创建输出目录：

.. code-block:: console

   mkdir -p "$ESP_VISION_WORK_DIR/hardware-validation"
   npm --prefix host/device-core run test:hardware -- --serial <USJ-port> --role usj --checks display --screen-output "$ESP_VISION_WORK_DIR/hardware-validation/lcd.jpg"

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


11.14 发布 Target 验收
----------------------

Function EV、P4 Vision 和 S31 Korvo 发布 Target 任务在原有板级相机、图像、显示和推理检查之外，强制执行以下检查。``--require-all`` 将不支持的已选项目判为失败；缺板或缺外设不能记为通过。

* ``--verify-flash`` 使用 esptool 将 ``flasher_args.json`` 中每个镜像与 Flash 独立比较，并应用与写入相同的镜像头设置。比较失败后不执行运行时测试。存在多个串口候选时，写入前必须明确指定 ``ESPPORT``/``--port``；校验与运行时连接继续使用选中的端口。
* ``discovery`` 在设备保存带随机标记的 Python 变量时，通过 SDK 被动枚举选中的串口，再检查变量和运行时身份仍保持不变。显式选择 TCP 端点时，要求 mDNS 广播的端点和设备 ID 同时匹配。枚举不会探测其他设备。默认串口 CI 任务不代表已验证局域网发现。
* ``communication`` 检查五轮不同的 Unicode 输出，每轮 UTF-8 数据超过 4 KiB，并在执行之间插入身份和运行状态 RPC。截断、旧输出或数据改变均失败。
* ``reconnect`` 对选中的传输连接执行三次释放与重新获取，每轮检查设备 ID、板型、固件版本以及新的 Python 标记。这验证正常断开重连，不代表已验证拔线、断电或租约争用。
* ``modules`` 在设备上执行 SHA-256 已知答案、十六进制/base64 二进制往返、struct 字节序、Unicode JSON、BytesIO 定位读取及垃圾回收。``ulab`` 实算并核对均值、逐元素乘法与求和、平方根及排序。两项均使用 RAM，不依赖 Flash/SD 写入。

Python 驱动的 ``--junit PATH`` 为各项 SDK 检查、烧录与校验阶段及运行时就绪生成 GitLab 测试报告。夹具加载进度事件仅保留为诊断日志；被阻塞的检查显示为失败，并保留其前置依赖。运行时验收前的失败也会生成失败的驱动测试项。日志及相机、屏幕制品继续保留；仅烧录成功不构成发布验收通过。

2026.11.14 候选版应共同保留最终提交、流水线、板型、接口、固件版本及镜像制品。功能完成后再运行完整 CI 固件矩阵。现有发布门禁仍要求三个 Target 任务全部通过。旧固件上的测试仅验证测试程序，不能作为候选版验收。创建标签与发布是独立的发布操作。

协调发布 npm 包时，受保护 master 上的 CLI 打包必须等待 Core 发布成功或确认该版本已存在；取得已发布制品地址与 SHA-512 后，再执行 ``npm ci``、构建及测试，并使用解析后的锁文件发布同一份已验证 CLI 制品。等待发布期间，MR 检查可以使用版本匹配的 Core 源码包。Plugin 的发布打包要求已发布 CLI 实际内置的 Core 版本与 Plugin 清单记录一致，不会回退到旧版 CLI 或 Core。因此，Core 0.8.1 必须先于 CLI 0.2.1 可用，两者均可用后才能打包 Plugin 0.2.0。固件资格检查、npm 发布与在线 Knowledge 可用性是独立的验收节点；仅创建固件标签不能证明对应 Knowledge 已上线。

Core 0.8.1、CLI 0.2.1、Plugin 0.2.0 和 Knowledge MCP 0.2.0 均已发布到 npm。Knowledge MCP 0.2.0 不绑定 Core/CLI。初始保留目录覆盖 2026.10.21、2026.10.23 和 2026.11.06，尚不覆盖 2026.11.14 固件候选版。应在标签通过资格检查且固件发布后归档新快照，再通过独立评审的 Knowledge 发布导入。Plugin 0.2.0 的发布门禁会等待同流水线的 Knowledge 部署成功，并在写入 npm 前校验线上版本化接口。
