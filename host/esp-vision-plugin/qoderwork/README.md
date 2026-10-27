# ESP-Vision 嵌入式 AI 视觉开发（QoderWork）

基于 ESP32 的 ESP-VISION 视觉应用开发：查询官方固件/API 知识，使用随包本地 CLI 检查设备、部署 MicroPython 脚本并验证实际结果。作者为 Espressif Systems (Shanghai) CO LTD，联系邮箱 libo@espressif.com。

此包使用 QoderWork 的 `.qoder-plugin/plugin.json`、`skills/`、`qoder.md` 与 `.mcp.json` 布局；不是 WorkBuddy Expert ZIP、旧 Skill ZIP，也不是已发布的 Qoder CLI 市场条目。版本 {{VERSION}}；原 npm 包身份仍为 `@esp-vision/plugin@0.1.0`，本构建不会发布 npm 或提交市场。

需要 Node.js 22+、允许执行本地命令的 QoderWork 环境和可达的 Knowledge MCP。设备使用还需要用户指定的受信任设备及串口权限或 TCP 网络连通性。完整第三方许可证随 `cli/licenses/`、`THIRD_PARTY_NOTICES.md` 和 `DEVICE_CORE_NOTICE.md` 一并提供。

先按 [CONNECTORS.md](CONNECTORS.md) 配置官方知识连接，再确认宿主安装后保留本地 CLI 与许可证。定位安装后的插件根目录，确认 CLI 文件存在后，将 POSIX shell 变量 CLI 设为该文件的实际绝对路径，执行 `node "${CLI:?Set CLI to the verified installed bundle path}" --help`。Windows 上同样使用已验证的绝对路径并正确引用。不得使用用户项目路径代替安装路径。使用 Knowledge MCP 查询一个目标开发板/API，确认连接实际可用，再在用户明确指定设备后检查设备；未连接硬件时不宣称硬件验收通过。

公开上架路径为 QoderWork 内“我的发布 → 提交新拓展”，选择 Plugin，经客户端结构预检、机审及可能的人工抽检后上架。显示名按用户指定使用“ESP-Vision 嵌入式 AI 视觉开发”；指南中的 2–6 汉字岗位名称为建议，本名称的实际接受情况仍需客户端预检确认。发布后在“设置 → 我的发布”管理版本；后续发布须递增语义化版本。本地 ZIP 和校验和是待平台验收候选，尚未通过客户端预检、激活、连接依赖或公开审核。

建议验收提示：查询 ESP32-P4 官方摄像头 API；仅列出可用设备候选且不自动选择；用户指定设备后检查固件与开发板；部署最小 MicroPython 应用并检查输出和清理结果。分别记录知识查询、本地 CLI 路径/版本、设备身份与真实结果，不以包版本代替载荷身份。
