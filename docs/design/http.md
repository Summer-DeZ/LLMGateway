# HTTP 与 SSE

HTTP/1.1 增量解析覆盖 Content-Length、chunked、连接关闭定界、响应头大小限制和 keep-alive。SSE 解析支持 LF/CRLF/单 CR、BOM、多行 data 和注释；终止标记由协议层判断，TCP EOF 不是正常模型完成标记。

公开接口：`GET /healthz`、`GET /v1/models`、`POST /v1/chat/completions`、`POST /v1/messages`、`POST /v1/messages/count_tokens`。使用 Bearer 或 x-api-key 鉴权；管理接口默认仅本机可访问。count_tokens 转发到目标链中声明支持的上游，没有能力时返回 501，不估算。

响应头携带 request id、实际上游、尝试次数、缓存状态和限流快照。错误按入口协议编码；跨协议转换保持上游 HTTP 状态和 message。非法 JSON、非法 tool_choice、Anthropic 缺少正数 max_tokens 都返回 400，不能让异常逃出 IO loop。

真实 SDK 验收两入口×两种上游均通过；count_tokens 与直连一致（14 token）。健康接口 4 线程、256 连接、30 秒达到 550341.09 请求/秒，p99 2.07ms。


设计约束：[参考文档](../README.md#详细约束)。历史证据：[验收记录](../acceptance/history.md)。所有数值为 2026-09-25 本机实测，不代表远端部署结果。
