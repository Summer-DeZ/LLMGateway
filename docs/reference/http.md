# http：HTTP/1.1 服务端、上游客户端、chunked 与 SSE

本文保留对外行为、线程归属、所有权和状态机约束；接口片段用于说明设计，具体签名以源代码为准。
实现摘要、运行说明和历史验收见 [文档索引](../README.md)。


模块 `gw_http`，依赖 `gw_net`。只处理 HTTP 语义，不理解模型协议。所有解析器都是**增量**的：
数据可以在任意字节处切开（包括 `\r` 与 `\n` 之间、多字节 UTF-8 中间），结果必须与整段喂入一致——单测用"逐字节喂入 vs 整段喂入"对比覆盖。

## 1. 服务端请求解析

```cpp
struct HttpRequest {
    std::string method, target, path, query;     // target 拆成 path + query
    int version_minor = 1;                        // 只接受 HTTP/1.0 与 1.1
    Headers headers;                              // 名字转小写，保留顺序与重复项
    std::string body;
    std::optional<std::string_view> header(std::string_view lower_name) const;
    bool keepAlive() const;                       // 1.1 默认 true，除非 Connection: close；1.0 需显式 keep-alive
};

class RequestParser {
public:
    enum class Result { need_more, complete, error };
    explicit RequestParser(ParserLimits);
    Result feed(Buffer& in);                      // 消费已解析的字节；complete 时剩余字节留在 Buffer
    HttpRequest take();                           // complete 后取出并 reset
    int errorStatus() const;                      // error 时应回复的状态码
};
```

| 限制 / 情况 | 默认 | 回复 |
| --- | --- | --- |
| 请求行 + 头总长 | 64 KB | 431 |
| 请求体 | `server.max_body_bytes`，默认 8 MB（DAgent 长上下文请求可能有几 MB） | 413 |
| `Transfer-Encoding: chunked` 请求体 | 不支持 | 411 |
| POST 没有 Content-Length | — | 411 |
| `Expect: 100-continue` | 先回 `HTTP/1.1 100 Continue\r\n\r\n` 再读体 | — |
| 请求行/头格式错误、多个 Content-Length 且不同 | — | 400 |

解析错误后回复并 `shutdown()` 连接，不再读后续字节。

## 2. 服务端响应写出

```cpp
class ResponseWriter {   // 包装下游 TcpConnection 的弱引用，只在 loop 线程使用
public:
    void writeFull(int status, const Headers& h, std::string_view contentType, std::string_view body, bool keepAlive);
    void writeStreamHead(int status, const Headers& h);   // 固定加 Content-Type: text/event-stream、Cache-Control: no-cache、Transfer-Encoding: chunked
    void writeChunk(std::string_view data);               // "<hex>\r\n<data>\r\n"；空数据不写（空块等于结束）
    void endStream();                                     // "0\r\n\r\n"
    bool committed() const;                               // 是否已经写出过状态行——重试/降级的分界线
    std::size_t pendingBytes() const;                     // 下游输出缓冲大小（背压用）
};
```

- 所有响应都带 `x-request-id`、`server: llm-gateway/0.1`；代理响应额外带 `x-gateway-upstream`、`x-gateway-attempts`、`x-gateway-cache`（见 [policies.md](policies.md)）。
- 流式响应也使用 chunked 而不是 `Connection: close` 定界，这样流结束后连接还能复用（DAgent 的 libcurl 会复用）。
- `x-accel-buffering: no` 一并写上，便于将来前面再挂 nginx。

## 3. 连接状态与超时（HttpServer）

`HttpConnState`（挂在下游 TcpConnection 的 context 上）：`RequestParser`、当前 `shared_ptr<ProxySession>`、`draining` 标记、定时器。

| 定时器 | 默认 | 超时动作 |
| --- | --- | --- |
| 请求头读取 `header_timeout` | 30s | 408 并关闭 |
| 请求体读取 `body_timeout` | 60s | 408 并关闭 |
| keep-alive 空闲 `keepalive_idle` | 75s（比 DAgent 客户端的复用窗口长） | 直接关闭 |

请求处理中不计空闲超时；流式期间的慢客户端由 ProxySession 的 stall 定时器管理。

## 4. 路由

| 方法 路径 | 处理 | 鉴权 |
| --- | --- | --- |
| `POST /v1/chat/completions` | ProxySession，入口协议 openai | 需要 |
| `POST /v1/messages` | ProxySession，入口协议 anthropic | 需要 |
| `POST /v1/messages/count_tokens` | 转发给路由中第一个 `count_tokens: true` 的上游，返回其精确计数；没有则 501（[policies.md §4.7](policies.md#47-其他用到-token-数的地方)） | 需要 |
| `GET /v1/models` | 列出配置里的模型别名（OpenAI 列表格式） | 需要 |
| `GET /healthz` | `200 ok` | 否 |
| `GET /metrics` | Prometheus 文本 | 否（仅在 `admin.listen_local_only` 时限制来源为 127.0.0.1） |
| `GET /admin/stats`、`GET /admin/requests?limit=N` | JSON | 同上 |
| `GET /v1/usage`、`GET /v1/usage/requests` | 本 key 的配额与用量（[usage-ledger.md §7](usage-ledger.md#7-查询接口)） | 需要 |
| `GET /admin/usage`、`GET /admin/usage/requests`、`GET /admin/quota/reconcile` | 任意 key 的用量与对账 | 同 `/metrics` |
| 其他路径 | 404（按请求路径猜协议返回对应错误体） | — |
| 已知路径但方法不对 | 405，带 `Allow` 头 | — |

路径前缀兼容：DAgent 的 `base_url` 写 `http://127.0.0.1:8080/v1`，OpenAI 客户端拼出 `/v1/chat/completions`；
Anthropic 客户端（DAgent anthropic kind）以同样 base_url 拼出 `/v1/messages`。两者都能直接命中。

## 5. 错误体格式（按入口协议）

OpenAI 入口：

```json
{"error": {"message": "…", "type": "rate_limit_error", "code": "rate_limited", "param": null}}
```

Anthropic 入口：

```json
{"type": "error", "error": {"type": "rate_limit_error", "message": "…"}}
```

| 场景 | 状态 | OpenAI type | Anthropic type |
| --- | --- | --- | --- |
| 缺少/错误凭据 | 401 | `authentication_error` | `authentication_error` |
| 请求 JSON 无法解析、缺 model/messages | 400 | `invalid_request_error` | `invalid_request_error` |
| 模型别名不存在 | 404 | `invalid_request_error`（code `model_not_found`） | `not_found_error` |
| 网关限流 | 429 | `rate_limit_error` | `rate_limit_error` |
| 配额用尽 | 429 | `insufficient_quota`（code 同名） | `rate_limit_error`（message 写明 token quota exceeded） |
| 账本或 Redis 不可用（fail closed） | 503 | `service_unavailable` | `overloaded_error` |
| 所有尝试失败，最后一次是超时 | 504 | `upstream_timeout` | `api_error` |
| 所有尝试失败，最后一次是 DNS/连接/断开 | 502 | `upstream_error` | `api_error` |
| 所有尝试失败，最后一次是上游 429/5xx | 上游原状态码 | 同协议原样透传；跨协议按 [protocol-translation.md §7](protocol-translation.md#7-错误体转换) 转换 | 同左 |
| 所有目标都被熔断 | 503 | `upstream_unavailable` | `overloaded_error` |
| 网关正在退出 | 503 | `server_shutting_down` | `overloaded_error` |

上游返回的不可重试 4xx（如 400 上下文超长）**原样透传状态码与响应体**（同协议时）或转换为入口协议的错误体（跨协议时，保留原 message 文本）。
DAgent 依赖 400 的上下文超长措辞触发压缩，所以错误 message 文本不能改写。

## 6. 上游客户端（UpstreamClient）

对一个已获得的 `Lease` 发送一次请求并增量接收响应，事件回调给 ProxySession：

```cpp
struct UpstreamRequest { std::string method = "POST"; std::string path; Headers headers; std::string body; };

class UpstreamExchange : noncopyable, public std::enable_shared_from_this<UpstreamExchange> {
public:
    struct Handler {
        std::function<void(int status, const Headers&)> onHead;         // 1xx 已跳过
        std::function<void(std::string_view)> onBody;                    // 已去掉 chunked 包装
        std::function<void(bool reusable)> onComplete;                   // 响应完整结束
        std::function<void(UpstreamIoError)> onError;                    // 连接断开/解析失败
    };
    UpstreamExchange(Lease, const UrlParts&, UpstreamRequest, Handler);
    void start();
    void pauseReading(); void resumeReading();                           // 背压：转发到 TcpConnection::stopRead/startRead
    // 中止：SHUT_WR → 非阻塞读完已到达的字节并交给 onDrain（只用于计量）→ close；之后不再回调
    void abortAndDrain(const std::function<void(std::string_view)>& onDrain);
    bool receivedAnyByte() const;                                        // 复用竞态判断用
};
```

请求报文：

```text
POST {path_prefix}/chat/completions HTTP/1.1
Host: {host}[:{port，非默认端口时}]
Content-Type: application/json
Content-Length: {n}
Accept: text/event-stream            ← 仅流式请求
Accept-Encoding: identity            ← 不接受压缩，避免自己实现 gzip 流解压
Connection: keep-alive
User-Agent: llm-gateway/0.1
Authorization: Bearer {key}          ← openai-chat 且配置了 key
x-api-key: {key} / anthropic-version: 2023-06-01   ← anthropic 上游
x-request-id: {id}
```

## 7. 上游响应解析（ResponseParser + ChunkedDecoder）

- 状态行 + 头：限 64KB；`1xx` 响应（`100 Continue`）跳过后继续解析。
- 响应体定界优先级：`Transfer-Encoding: chunked` > `Content-Length` > 连接关闭定界（此时连接不可复用）。
- `ChunkedDecoder` 状态：`size_line → (ext 忽略) → data → data_crlf → … → last_chunk → trailers → done`。
  十六进制长度溢出（超过 16 位十六进制数）或非法字符报错。trailer 忽略。
- 非 2xx 响应的体最多保留 64KB（错误体只用于透传/分类），超出截断并标记连接不可复用。
- 连接复用条件见 [architecture.md §3](architecture.md#3-对象所有权与生命周期)。

## 8. SSE

### 8.1 解析（`SseParser`，与 DAgent `net::SseParser` 语义一致）

- 行尾接受 LF / CRLF / 单独 CR；去 BOM；`:` 开头是注释，忽略（但注释也算"收到字节"，用于 idle 计时）；
- 空行派发事件；多行 `data:` 用 `\n` 连接；`event:` 可选；`id` 保留；`retry:` 忽略；
- 流结束时未以空行结尾的事件丢弃。
- 额外暴露 `bytesSinceLastEvent()`，用于在事件被切成多段时仍能计算 TTFB。

### 8.2 写出

```text
[event: {event}\n]          ← event 非空时
data: {第一行}\n
data: {第二行}\n …          ← data 里的每个 \n 拆成独立的 data 行
\n
```

透传模式下按"解析 → 重新格式化"逐事件转发，而不是转发原始字节块，原因：
(1) 下游永远不会收到半个事件；(2) 需要时能丢弃或改写单个事件（例如客户端没要 usage 却被网关强制打开的 usage 块）；
(3) 缓存保存的是规范化后的字节。代价只是一次字符串拷贝。

## 9. URL

`parseBaseUrl("http://127.0.0.1:10010/v1")` → `{host="127.0.0.1", port=10010, path_prefix="/v1"}`；
`http://localhost` → `{"localhost", 80, ""}`。支持 IPv6 字面量 `[::1]:8080`。
`https://` 及其他 scheme 返回错误（本项目不实现 TLS），配置加载因此失败并指出是哪个上游。
末尾 `/` 去掉；不支持 userinfo 和 query。拼接：`path_prefix + "/chat/completions"` 或 `path_prefix + "/messages"`。
