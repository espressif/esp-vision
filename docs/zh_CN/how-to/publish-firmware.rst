发布合格固件
============

:link_to_translation:`en:[English]`

固件与 Device Core 独立发布。发布 npm 包（包括 Device Core 0.3）不会运行固件上传任务，只有符合发布 CI 规则的固件标签才会运行 ``pack_launchpad``、``qualify_firmware_release`` 和 ``upload-dl-trigger``。``2026.08.27`` 等已有版本早于元数据生成功能，因此二进制下载可能正常，但 ``firmware/<board>/<version>.json`` 返回 HTTP 404。

发布包与 URL 契约
-----------------

打包器复用发布矩阵的 IDF 产物，不重新编译固件。``binaries/`` 产物包含：

.. code-block:: text

   config.toml
   releases/<tag>/manifest.json
   esp-vision-<board>-<tag>.bin
   esp-vision-<board>-<tag>-app.bin
   firmware/<board>/<tag>.json

上传器必须将 ``config.toml``、``releases/`` 和 ``firmware/`` 保留在 ``https://dl.espressif.com/AE/esp-vision/`` 根目录，两种二进制文件则放入 ``bin/``。Core 根据根配置 URL 查找元数据；即使生成成功，误放到 ``bin/firmware/`` 也会导致 HTTP 404。上传器仅接受清单声明的发布对象和可选 MCP 文件，拒绝无关产物。

按八个发布板型计算，默认布局有 16 个 BIN、8 个逐板 JSON、1 个不可变清单和 1 个根配置，共 26 个对象；加上可选的版本化 MCP 包及 ``bin/mcp/latest.json`` 兼容 alias 为 28 个。单 BIN 模式去掉 8 个应用 BIN 和 8 个逐板 JSON，剩下 8 个 BIN 加清单/config，共 10 个对象（含 MCP 为 12 个）。源码配置仍只生成一份 TOML；服务端以根 config 内一个权威可变固件入口引用每版不可变清单。MCP latest alias 是独立的兼容便利入口。

生成后，``tools/ci/launchpad/validate_release.py <tag>`` 检查每个发布板型，包括未在官网展示的板型。缺少应用升级或完整安装元数据、板型/版本/构建身份不匹配、URL 错误、二进制损坏、引导程序/应用/分区表散列错误、芯片修订范围交集错误或分区越界都会被拒绝。仅要求本次发布具有元数据，发布列表中的历史版本无需补齐。检查失败时发布任务停止，不会将不完整的包交给上传器。

不可变清单与当前入口
--------------------

每版只有一个不可变 ``releases/<tag>/manifest.json``，包含 ``schemaVersion = 1``、版本、完整 40 位 ``sourceCommit``、正整数 CI ``pipelineId``、``layout``、全部板型发布描述、该版完整 ``website`` 快照及明确的 ``mcp`` 描述或 JSON ``null``。模型下载链接固定到源提交，不使用漂移分支。根 ``config.toml`` 保留 launchpad 字段和兼容网站快照，并增加 ``[publication]``：同一身份、``manifestUrl``、清单原始字节的 SHA-256、``previousConfigSha256`` 和 ``releases`` 记录。每条记录只引用该版清单及哈希，并列出实际发布的非空、唯一 ``boards`` 集合。

打包器读取上一份公开 config，验证其全部历史清单的哈希、身份与板型集合后原样继承记录。下载或校验失败直接阻断，不丢弃历史继续发布。Git 标签不证明发布成功，不会自动成为记录。旧网站版本标签仅保留下载入口，不能凭空获得清单或板型与版本的组合。已经发布的版本不能同名重新打包。

``manifest.website`` 是该版板型和模型的权威快照。根 ``config.website`` 为兼容旧消费者镜像最新快照。历史版本必须跟随自己的 publication 记录，不能把今天的板型列表与所有版本标签做笛卡尔积。

构建导出的 Wi-Fi 能力
---------------------

新发布项的 ``capabilities.wifi`` 来自 ``release-build-identity.json``。CI 在成功构建后复用网络服务源文件的真实编译命令，仅运行预处理，沿用实际 include 路径和宏定义。配网能力要求 ``MICROPY_PY_NETWORK_WLAN && ESP_VISION_ENABLE_BACKGROUND_NETWORK && (CONFIG_ESP_WIFI_ENABLED || CONFIG_ESP_WIFI_REMOTE_ENABLED)``。身份文件还绑定板型、固件版本、完整源提交及应用 SHA-256。缺失或错配身份不能通过资格门禁；不能用展示数据或板型白名单宣称支持 Wi-Fi。

可选单 BIN 布局
---------------

默认仍为 ``ESP_VISION_PUBLICATION_LAYOUT=legacy``。只有支持 manifest 和切片的 Core 版本及官网消费者已部署后，才显式启用 ``single-bin``。它只输出八个完整 BIN 和中央清单，不生成应用 BIN 或逐板 JSON。应用的 ``image.source`` 包含完整容器的 ``url``、``bytes``、``sha256`` 及应用在容器内的字节 ``offset``。``image.url`` 等于容器 URL，而 ``image.bytes`` 和 ``image.sha256`` 仍描述应用。source 必须与 ``install`` 的完整容器一致，切片偏移等于应用烧写地址。Core 先验证完整容器和应用切片，再仅烧写应用；完整安装继续执行独立的布局与安全校验。

MCP 与发布资格
--------------

先生成并按 schema 验证 MCP，再组装清单。版本化描述绑定本版包的字节、哈希、版本和完整源提交；失败默认阻断。只有显式 ``ESP_VISION_ALLOW_MISSING_MCP=1`` 才允许 ``mcp: null``，清单不能引用旧 ``latest.json``。兼容 alias 由上传器在当前入口发布成功后维护，不作为发布资格输入。

打包复用构建产物，可以与硬件检查并行。``qualify_firmware_release`` 明确等待全部必需固件矩阵、Function 板的三组 IDF HIL、SDK 和 host/config/MCP 检查，并以实际提交及流水线 ID 全量校验包。上传 trigger 同时依赖打包和资格门禁成功。上传器再次核对这两个 job，先上传并验证不可变资产与清单，最后基于 ``previousConfigSha256`` 比较并交换可变 config 入口。过期或并发候选不能覆盖更新的历史。

发布流程
--------

#. 先合入新的上传器与 Core 契约，并保留根目录布局修复依赖。旧消费者不能启用单 BIN 模式。
#. 打标签前分别更新 Plugin 和 Knowledge MCP 的 npm 版本，并在两个包的清单中声明相同的 Core 和 CLI minor 范围及本次解析版本。指定的 CLI 必须已经发布且实际打包指定 Core；Plugin 打包该已发布 CLI。patch 版本只能兼容地修复问题，只有 minor 版本可以增加兼容功能，只有 major 版本可以引入不兼容变更；patch 和 major 版本不得额外添加新功能。Core 和 CLI 的版本由受保护默认分支流水线独立发布。
#. 先确认该完整源提交已成功镜像到 GitHub，固定到提交的模型链接已经可用，再通过正常受保护流程创建新固件标签；上传器在推进 config 前会拒绝尚不可用的源提交。等待构建、HIL、SDK 及包资格通过；npm 发布不等于固件发布。
#. 用根 config 的 publication 哈希验证版本清单、精确板型集合、website/MCP 快照和镜像哈希。配置 dry-run 或分支 CI 成功不代表生产上传成功。

历史 BIN 和不可变清单继续保留。不要为旧镜像编造元数据、覆盖同名版本或从 git 标签猜兼容性。离线验证和合入这些修改不会发布标签、npm 包或 CDN 数据。

Iris 固件文件
-------------

``ESP_VISION_ENABLE_IRISFW`` 是默认关闭的 CMake 选项和 C 编译宏，仅支持 ``ESP32_S31_MOSAICO``；其他板型开启时配置失败。普通 Launchpad BIN 构建继续采用原 factory 布局。独立的 ``firmware_irisfw_mosaico`` 作业使用 IDF ``release-v6.1`` 开启该宏，产物存放在 ``build/ESP32_S31_MOSAICO/idf6.1-irisfw``。固件标签的 ``pack_irisfw`` 只消费这一作业，其他板型不生成 Iris 包。

切换选项时使用新的构建目录。构建会核对实际分区选择、QIO Flash 模式和回滚配置，复用旧配置也不能跳过检查：

.. code-block:: sh

   idf.py --board ESP32_S31_MOSAICO -B "$ESP_VISION_WORK_DIR/build/irisfw" \
     -D ESP_VISION_ENABLE_IRISFW=ON build

保留区布局
~~~~~~~~~~

16 MiB 布局遵循 Mosaico Recovery ABI 2 的固定系统前缀，产品契约为 ``esp-mosaico/v2``，布局 ID 为 ``mosaico-retained-test-2m-v2``，分区表仍位于 ``0x8000``。应用 NVS 地址保持不变；7 MiB 应用分区之后的全部剩余空间划给 FAT，直至 16 MiB Flash 末尾，不再单独预留 ``ui_apps``：

.. list-table:: Iris 构建分区
   :header-rows: 1

   * - 分区
     - 类型/子类型
     - 起始地址
     - 大小
   * - otadata
     - data/ota
     - 0x9000
     - 0x2000 (8 KiB)
   * - phy_init
     - data/phy
     - 0xB000
     - 0x1000 (4 KiB)
   * - sysmeta
     - data/nvs
     - 0xC000
     - 0x14000 (80 KiB)
   * - vibe_mode
     - app/test
     - 0x20000
     - 0x1C0000 (1.75 MiB)
   * - coredump
     - data/coredump
     - 0x1E0000
     - 0x20000 (128 KiB)
   * - nvs
     - data/nvs
     - 0x200000
     - 0x10000 (64 KiB)
   * - main_app
     - app/ota_0
     - 0x210000
     - 0x700000 (7 MiB)
   * - ffat
     - data/fat
     - 0x910000
     - 0x6F0000 (6.9375 MiB)

固定的 ``0x210000`` 应用起点使应用与 FAT 合计只有 13.9375 MiB；两者各占 7 MiB 会超出 16 MiB Flash 末尾 64 KiB，因此默认分配应用 7 MiB、FAT 6.9375 MiB。

打包器拒绝超过 7 MiB 应用分区的镜像。此布局将原 UI 资源区并入 FAT；修改已有 FAT 分区的起始地址或容量不会自动迁移或扩展文件系统，切换布局前应保存需要保留的文件。

Iris 构建直接从 NVS 分区 ``sysmeta``、命名空间 ``shared`` 的字符串键 ``wifi_ssid`` 和 ``wifi_password`` 读取配网信息，有效配置默认启用。只有两个共享键都不存在时才只读回退到旧版 ``sysmeta/wifi`` 的 ``ssid``/``password``；启动时不复制配网信息、不写 NVS。此构建不使用普通 ESP-VISION 的 ``nvs/ev_network`` 记录或出厂配网信息。没有配网记录时保持未配置；记录不完整、超长或读取失败时报告配置错误，保留原数据。

ESP-VISION 配网操作写回相同的共享键，另用 ``ev_enabled``（u8）和 ``ev_revision``（u32）保存服务的启用状态及修订号；Vibe Mode 继续使用共享 SSID/密码。忘记网络写入一对空字符串，避免任一固件重新启用旧版凭据。操作不擦除 ``sysmeta``，也不改动其他共享设置。

此包要求设备已安装兼容的 Vibe Mode，版本不低于 0.2.0，通过其原生 System Update 流程安装 ``.irisfw``。这个构建的原始 IDF 烧录清单不是 Vibe Mode 初始化包，其中的 bootloader 和空白 OTA 元数据不能替换保留系统。从普通 factory 布局切换不会迁移原有文件。新的 ``ffat`` 遵循板级文件系统初始化策略，更新包不携带空白文件系统镜像。

MicroPython 和 EV-MUX 就绪后，启用 Iris 宏的应用确认待验证 OTA 镜像，并在现有 ABI 2 恢复元数据中记录应用地址。同一宏在现有 USB CDC 接口启用最小 Iris 0.2 安装验收子集：硬件与固件身份、认证绑定、BOOT/LINK_READY/HEALTHY 事件、状态、ping/时间及只读系统清单。原生更新器无需修改，即可核对新启动、准确 ELF 身份、分区表哈希和保留的安装操作回执。HEALTHY 要求启动已确认、VM 就绪且复位原因不是崩溃；外设仍须分别验收。

兼容代码复用 EV-MUX 的先到先得会话租约和发送队列。仅在 EV-MUX 帧之外解析 Iris 记录，发送时不插入已经开始的 EV-MUX 帧。Iris owner 不会启用 EV-MUX 的 REPL、日志或预览输出；断开或租约到期后释放所有权。Iris 构建为 USB 产品字符串增加 ``ESP-Iris`` 标记以供原生发现，沿用原 CDC 接口。普通构建不编译该适配，不引入 Iris 运行库、第二套 USB、应用更新器或远程进入 Vibe Mode 的命令。

后续安装新包前，请按住 AI 键并按 Reset，手动进入 Vibe Mode。应用拒绝 Iris 复位和更新写入命令。原生更新器释放设备后，ESP-VISION 客户端继续使用 EV-MUX；其 ``firmware.status.v1`` 能力提供应用 BIN 准确哈希等额外只读证据，详见 :doc:`../communication/wire-protocol`。


容器与发布
~~~~~~~~~~

无签名 ZIP 使用 ``esp-iris-system-update/0.2``，恰好包含 ``manifest.json``、补齐擦除字节至 4096 字节的 ``partition-table.bin`` 和 ``app.bin``，清单声明 ``minimum_recovery_version=0.2.0``。包不包含 bootloader、OTA 元数据、Vibe Mode、NVS、coredump、FFAT 或 UI 资源镜像。打包器核对保留分区策略；除 IDF 生成的标准空白 OTA 元数据外，额外烧录输入均被拒绝。Secure Boot 签名和加密镜像需要独立策略，目前拒绝。

Iris 构建保留标准 ESP-IDF 应用描述符的工程名和版本。标签必须适配 31 字节版本字段，并与实际镜像完全一致。构建来源记录绑定编译配置类型、板型、源提交、流水线/任务 ID、实际 IDF 提交、配置/布局哈希和所有输入哈希，不包含个人路径或配置内容。打包仅消费当前流水线的准确产物，不重新编译或选择其他 IDF 目录。

格式基线为 `esp-mosaico-utils 的 be151ae047014e79a9c7ae0092dd203844499003 提交 <https://github.com/esp-mosaico/esp-mosaico-utils/blob/be151ae047014e79a9c7ae0092dd203844499003/ESP-Iris/components/esp_iris/protocol/system_update_manifest.schema.json>`_。打包器和独立校验器使用标准库及哈希锁定的 YAML 解析依赖。``dist/irisfw/`` 包含此板型的固件包、``.provenance.json`` 和 ``.validation.json`` 伴随文件，以及 ``SHA256SUMS`` 和 ``release.json``。

``publish_irisfw`` 等待 ``qualify_firmware_release`` 成功，通过 ``CI_JOB_TOKEN`` 将冻结产物发布到本项目 Generic Package Registry，包名 ``esp-vision-irisfw``、版本 ``<tag>``。下载端点为 ``${CI_API_V4_URL}/projects/${CI_PROJECT_ID}/packages/generic/esp-vision-irisfw/<tag>/<filename>``，沿用 registry 现有访问权限。原 Launchpad BIN 地址及外部 DL 上传器不变。

发布前检查全部远端文件，相同字节复用，不同字节拒绝覆盖。逐文件下载比对成功后，最后写入 ``release.json``。重试使用相同冻结产物，换流水线或重新生成来源记录须使用新版本；CI resource group 将发布串行化。校验和证明完整性，不代表安装授权。平台上传、审批及公开上架与 registry 发布和实机测试分别验收。

从同一流水线下载产物后打包：

.. code-block:: sh

   python -m pip install --require-hashes -r tools/ci/irisfw/requirements.lock
   python tools/ci/irisfw/package_release.py --tag "$CI_COMMIT_TAG" \
     --source-sha "$CI_COMMIT_SHA" --pipeline-id "$CI_PIPELINE_ID" \
     --build-root build --output dist/irisfw
   python tools/ci/irisfw/validate_release.py --tag "$CI_COMMIT_TAG" \
     --source-sha "$CI_COMMIT_SHA" --pipeline-id "$CI_PIPELINE_ID" --root dist/irisfw

每个候选使用新的输出目录。API/web CI 同时设置 ``ESP_VISION_IRISFW_ONLY=1``、``ESP_VISION_TEST_BOARD=ESP32_S31_MOSAICO`` 和 ``ESP_VISION_TEST_IDF_IMAGE=release-v6.1`` 时，仅编译 Iris 固件，不操作硬件。离线回归命令为 ``python -m unittest tools.tests.test_irisfw_release``。分支和 MR 不发布 registry 文件。
