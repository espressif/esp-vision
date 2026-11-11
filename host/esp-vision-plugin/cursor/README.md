# ESP-Vision 嵌入式 AI 视觉开发 · Cursor

基于 ESP32 的 ESP-VISION 视觉应用开发助手。通过官方 Knowledge MCP 查询固件/API/示例，通过本插件随包本地 CLI 检查用户指定的设备、部署 MicroPython 程序并验证结果。版本 {{VERSION}}，作者 {{AUTHOR_NAME}}，联系 {{AUTHOR_EMAIL}}。

## 本地安装

1. 解压 ZIP，将其中完整的 `esp-vision` 文件夹复制到 `~/.cursor/plugins/local/`。文件夹内应直接包含 `.cursor-plugin/plugin.json`、`skills/`、`mcp.json` 和 `cli/`；不要额外嵌套一层。已有同名目录时先保留其内容，不直接覆盖或删除未知/修改过的副本。
2. 重启 Cursor 或运行 `Developer: Reload Window`，在 Customize 中确认插件、Skill 与 `esp-vision-knowledge` MCP。企业管理员必须允许 Local Plugin Imports；如策略禁止，请使用组织批准的安装渠道。同名市场插件优先于本地副本。
3. 先做官方知识查询，再按用户指定的端点进行设备操作。未连接硬件时不宣称硬件验收完成。设备文件操作和 Python 执行可能中断正在运行的应用，遵循 Skill 的会话所有权与授权要求。

需要 Node.js 22+ 和能访问官方知识服务的网络。串口使用随包 npm 原生绑定，需要端口权限，无需主机 Python。优先 PowerShell；Git Bash/MSYS 中使用单条命令的 `MSYS2_ARG_CONV_EXCL='*'` 保持设备路径，并为 CLI 和本地文件使用经确认的 Windows 路径。

从 Cursor 中当前安装的 Skill 文件定位插件根目录，确认 `cli/device-cli.mjs` 存在，再将当前 shell 的 CLI 变量设置为该文件的绝对路径。POSIX shell 可运行 `node "${CLI:?Set CLI to the verified installed bundle path}" --help`；PowerShell 可在确认 CLI 已设置后运行 `node "$CLI" --help`。不要根据项目工作目录猜测路径，也不要用全局 CLI、npx 或 Device MCP 替代本插件的 CLI。

## Local installation

Extract the ZIP and copy its complete `esp-vision` directory into `~/.cursor/plugins/local/`. Reload Cursor and verify the plugin, Skill and official Knowledge MCP in Customize. Preserve existing unknown or modified copies. Enterprise policy must allow local imports; an installed marketplace plugin with the same name takes precedence. See the [official local testing instructions](https://cursor.com/docs/plugins#test-plugins-locally).

Use Node.js 22+ and the bundled CLI located from the installed Skill, using bundled npm native bindings for serial devices without host Python. Select the endpoint explicitly. Keep Knowledge queries separate from device operations and obey session ownership, interruption and authorization rules. The package carries its complete CLI/license closure; no Device MCP is configured.

This ZIP is a locally testable Cursor Plugin, not proof of Cursor activation or marketplace approval. Public marketplace distribution requires a complete payload hosted in a public Git repository and submitted at [Cursor Marketplace](https://cursor.com/marketplace/publish), following the [official reference](https://cursor.com/docs/reference/plugins). Installing or building this package does not submit it to a marketplace or publish npm.
