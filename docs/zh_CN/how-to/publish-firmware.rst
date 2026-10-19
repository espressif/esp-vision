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
#. 先确认该完整源提交已成功镜像到 GitHub，固定到提交的模型链接已经可用，再通过正常受保护流程创建新固件标签；上传器在推进 config 前会拒绝尚不可用的源提交。等待构建、HIL、SDK 及包资格通过；npm 发布不等于固件发布。
#. 用根 config 的 publication 哈希验证版本清单、精确板型集合、website/MCP 快照和镜像哈希。配置 dry-run 或分支 CI 成功不代表生产上传成功。

历史 BIN 和不可变清单继续保留。不要为旧镜像编造元数据、覆盖同名版本或从 git 标签猜兼容性。离线验证和合入这些修改不会发布标签、npm 包或 CDN 数据。
