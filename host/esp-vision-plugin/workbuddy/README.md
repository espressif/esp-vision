# ESP-VISION for WorkBuddy

The npm installer prepares a local WorkBuddy Skill package and the official ESP-VISION Knowledge MCP configuration. WorkBuddy completes the installation through its documented user interface; this installer does not edit WorkBuddy configuration files or claim that the Skill or connector is active.

## Import the Skill

In WorkBuddy, open **专家 · 技能 · 连接器** (Expert · Skill · Connector), choose the **技能** (Skills) tab, then select **添加技能 > 上传技能** (Add Skill > Upload Skill) and choose `esp-vision-development.zip`. WorkBuddy's Skill guide says it imports a local Skill package and configures it automatically. Start a new conversation after import.

When replacing an earlier imported version, use WorkBuddy's **已安装** (Installed) Skills list to uninstall the previous ESP-VISION Skill before uploading this version. The npm installer keeps earlier versioned local files in place.

## Configure Knowledge MCP

In WorkBuddy, open **插件 > MCP 服务器 > 配置 MCP** (Plugins > MCP Server > Configure MCP). Merge only the `esp-vision-knowledge` entry from `mcp.json` into the editor's existing `mcpServers` object; preserve every other server and setting. If no MCP configuration exists yet, use the supplied file as the starting configuration. Save and confirm that `esp-vision-knowledge` shows a connected state before asking for official ESP-VISION documentation. This is the online Knowledge endpoint; no device MCP server is included.

The official WorkBuddy documentation describes both user-level and project-level MCP settings. Choose the scope in WorkBuddy that matches where you want the endpoint available. This installer does not write `~/.workbuddy/mcp.json` or a project `.workbuddy/mcp.json`.

## Device access

The imported Skill invokes the verified Device CLI copy at `{{CLI_PATH}}`. It requires Node.js 22 or newer and local shell execution on the computer that can access the board. Serial operations additionally require Python 3 with `pyserial`; firmware flashing requires `esptool>=5.3,<6`. TCP operations do not require serial Python support. The ZIP also carries the dependency-closed CLI under `esp-vision-development/scripts/cli/` for inspection or separate local use.

The Skill asks for explicit device targets and preserves the existing consent, application workflow, safe connection, and result verification behavior. It never starts a device MCP process.

## Official WorkBuddy instructions

- [Upload a local Skill package](https://www.codebuddy.cn/docs/workbuddy/From-Beginner-to-Expert-Guide/Function-Description/Skills-Market)
- [Configure WorkBuddy MCP](https://www.codebuddy.cn/docs/workbuddy/From-Beginner-to-Expert-Guide/Function-Description/MCP-Guide)
- [WorkBuddy connector integration guide](https://open.workbuddy.cn/docs/connector)
