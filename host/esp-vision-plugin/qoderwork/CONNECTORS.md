# 官方知识连接

本插件只声明 `esp-vision-knowledge`，使用官方公开文档 MCP：`https://mcp.vision.espressif.com/mcp`。安装时在 QoderWork 的连接配置中填写该完整地址；不输入设备地址或凭证。`_setup.guide` 中提供相同地址，主页链接仅用于了解服务。连接工具只查询官方文档，本地串口/TCP 设备由随包 CLI 访问。

此声明遵循 QoderWork 发布指南 3.2 节的 `streamable-http`、`{{USER_CONFIG}}` 和 `_setup` 结构。该占位符由宿主安装流程替换，不能作为 HTTP 地址直接请求。这里没有虚构市场 Connector ID、OAuth 能力或已上架状态。

指南 2.2 节要求“引用的 Connector 必须为市场中已上架的有效条目”，3.2 节同时允许声明式 MCP 地址。公开指南未说明固定公开文档 MCP 是否必须先登记市场 Connector，也未提供 Connector ID 字段。必须在实际客户端预检或官方反馈中确认这两条规则如何适用于本插件；尚无平台证据，不能宣称依赖已解析或审核就绪。

若官方要求另行发布 Connector，须另行满足其 OAuth 2.0、公开 HTTPS、公司身份电话核验、隐私政策/服务条款、至少三个工具调用示例、权限与异常说明和近七日可用性记录等条件。不要把这组 Connector 独立上架要求未经确认扩展为所有插件内嵌 MCP 的要求，也不要通过添加 Device MCP 绕过依赖审核。
