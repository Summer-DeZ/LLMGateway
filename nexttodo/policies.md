# 策略：限流、缓存、可观测性与配置

## 1. 限流

### 1.1 维度

每个请求检查 4 个令牌桶，任何一个不足就拒绝：

| 桶 | 容量（突发） | 补充速率 | 请求消耗 |
| --- | --- | --- | --- |
| `user.rpm` | `rpm`（或 `rpm_burst`） | `rpm / 60` 每秒 | 1 |
| `user.tpm` | `tpm`（或 `tpm_burst`） | `tpm / 60` 每秒 | 预估 token |
| `model.rpm` | 模型别名的 `rpm` | 同上 | 1 |
| `model.tpm` | 模型别名的 `tpm` | 同上 | 预估 token |

配置为 0 或缺省表示该维度不限。模型维度按**别名**计，降级到备用上游不重复扣。

### 1.2 TokenBucket

```cpp
class TokenBucket {                                    // 每桶一把 mutex
public:
    TokenBucket(double capacity, double refill_per_sec, const Clock&);
    // 以下均需在持锁状态下调用，由 RateLimiter 统一加锁
    void refill(Timestamp now);                        // tokens = min(capacity, tokens + elapsed × rate)
    bool canTake(double n) const;                      // n > capacity 时要求桶满（否则大请求永远无法通过）
    void take(double n);
    void adjust(double delta);                         // 结算：可以变负，下限 -capacity
    Duration waitFor(double n) const;                  // 距离 canTake(n) 为真还需多久
};
```

### 1.3 RateLimiter

- 用户和模型别名都来自配置，**启动时预先创建全部桶**，运行期只读查找，映射表无需加锁；只锁具体的桶。
- `tryAcquire(user, alias, est)`：用 `std::scoped_lock` 同时锁住 4 个桶（它自带防死锁算法）→ 全部 refill → 全部 canTake → 全部 take；
  否则不扣任何桶，返回 `Rejection{retry_after = 各不足桶 waitFor 的最大值, scope, dimension}`。
- `settle(ticket, actual)`：两个 tpm 桶 `adjust(reserved - actual)`（多退少补）。`actual` 是上游报告的真实总量（[§4.6](#46-请求级汇总与成本)）；
  有字段未报告时该部分不退还预留。
- 预留量 = `token_estimate(请求)` + `max_tokens`（缺省时取模型别名配置的 `default_max_tokens`，默认 4096）。
  这是准入时唯一用到估算的地方：请求发出前没有别的办法知道大小。预留不是消耗，不进记录、指标和成本。预留偏大是有意的，结算时会退回。
- 为什么允许负债：真实用量超出预估时（例如 max_tokens 未设置），把超出部分记到未来，下一个请求自然等待，保证长期速率正确。

### 1.4 响应头

- 通过时：`x-ratelimit-limit-requests`、`x-ratelimit-remaining-requests`、`x-ratelimit-limit-tokens`、`x-ratelimit-remaining-tokens`（取用户和模型两个维度中较紧的那个）。
- 拒绝时：`429`、`Retry-After: <秒，向上取整，至少 1>`、`x-gateway-ratelimit-scope: user|model`、`x-gateway-ratelimit-dimension: requests|tokens`。
- DAgent 收到 429 会读取 Retry-After 退避（≤300 秒）后重试，适合端到端演示。

## 2. 响应缓存

### 2.1 可缓存条件

全部满足才查找/写入缓存：

1. 请求头没有 `x-gateway-cache: bypass`；
2. 请求显式 `temperature == 0`，或请求头 `x-gateway-cache: force`（Qwen3.6 服务端默认 temp 0.6，不显式传 0 就不缓存）；
3. 没有 `n > 1`；
4. 写入时：outcome 为 `ok`，finish 为 stop / length / tool_calls，响应大小 ≤ `max_entry_bytes`。

### 2.2 键

```text
SHA-256( "v1|" + 入口协议 + "|" + (stream ? "s" : "n") + "|" + (client_wants_usage ? "u" : "-") + "|" + 模型别名 + "|" + 规范化请求体 )
```

规范化请求体：nlohmann::json 解析后删除 `user`、`metadata`、`stream`、`stream_options`，再 `dump()`。
nlohmann 默认用 `std::map` 存对象，键天然有序，`dump()` 结果就是规范形式。注意 `0` 与 `0.0` 输出不同，会是两个键，接受这个行为。
摘要用 OpenSSL `EVP_Digest(..., EVP_sha256())`，存成 32 字节二进制。

### 2.3 结构

- 16 个分片，每片：`std::list<Entry>`（LRU 顺序，最近使用在前）+ `unordered_map<Key, list::iterator>` + 一把 mutex + 字节预算 `max_bytes / 16`。
- `Entry { key; std::string body; bool stream; std::string content_type; Usage usage; std::string upstream; Timestamp expires_at; }`。
- `get`：不存在 → miss；已过期 → 删除并 miss（惰性过期）；否则移到表头并返回**副本**（`shared_ptr<const Entry>` 避免拷贝大字符串）。
- `put`：已存在则替换；插入表头；从表尾淘汰直到分片字节数不超预算。
- 定时清扫：main loop 每 30 秒对每个分片扫描一遍删除过期项（分片小，直接全扫）。
- 不做 single-flight：并发的相同请求同时未命中时都会访问上游。文档里说明这个取舍。

### 2.4 命中回放

- 响应头 `x-gateway-cache: HIT`、`x-gateway-upstream: <原上游>`、`x-gateway-attempts: 0`；未命中但可缓存时写 `MISS`，不可缓存时不写该头。
- 流式：`writeStreamHead` → 整段 `writeChunk(body)` → `endStream`；非流式：`writeFull`。
- 缓存内容里的 `id`（`chatcmpl-…`/`msg_…`）是首次请求的，接受这个行为。
- 记录：outcome `cache_hit`，usage 取条目保存的原始上游 usage（`usage_source = cache`），`cost = 0`，`saved_cost` = 按条目 usage 计算的成本。
  只有 `usage_source = upstream_final` 的响应才会被写入缓存。

## 3. 可观测性

### 3.1 RequestRecord

| 字段 | 说明 |
| --- | --- |
| `ts` | 请求收齐时刻（UTC，毫秒） |
| `request_id`、`user`、`ingress`（openai/anthropic）、`model`（别名）、`stream` | |
| `upstream`、`upstream_kind`、`attempts`、`fallback`（是否换过目标） | 最终服务的上游 |
| `outcome`、`http_status` | outcome 取值见 [proxy-session.md §8](proxy-session.md#8-finishoutcome) |
| `latency_ms` | received_at → 最后一个字节交给下游连接 |
| `ttft_ms` | received_at → 首 token 事件写给下游（流式；非流式为 null） |
| `upstream_ttfb_ms` | 最后一次尝试：请求写出 → 上游首个响应字节 |
| `connect_ms`、`tls_ms`、`tls_resumed`、`conn_reused` | 最后一次尝试的连接信息 |
| `prompt_tokens`、`cached_tokens`、`prompt_computed_tokens`、`completion_tokens` | 各尝试之和，全部来自上游报告；有未报告部分时为 null（[§4.6](#46-请求级汇总与成本)） |
| `usage_source`、`usage_complete` | `upstream_final` / `upstream_partial` / `unreported` / `cache` / `rejected` |
| `attempts[]` | 每次尝试：`upstream`、`result`、`http_status`、`prompt`、`cached`、`prompt_computed`、`completion`、`source` |
| `cost_usd`、`saved_cost_usd` | 有未报告字段时 `cost_usd` 为 null |
| `cache`（hit/miss/none） | |
| `backpressure_pauses`、`backpressure_ms` | |
| `cancel_latency_us` | 仅取消时 |
| `error` | 失败时的简短原因 |

### 3.2 TTFT 的定义

从网关收齐请求体开始，到第一个"首 token 事件"（[protocol-translation.md §2](protocol-translation.md#2-中立模型protoneutralhpp)）写给下游为止。
它包含网关排队、限流检查、建连/TLS、上游排队与预填充时间，是用户实际感受到的首字延迟。
`upstream_ttfb_ms` 用来区分"网关/网络慢"还是"模型慢"。

### 3.3 成本

公式与口径见 [§4.6](#46-请求级汇总与成本)（按尝试计算后求和）。单价按**实际服务该尝试的上游名**查 `pricing`；
没有配置单价的上游成本记 0；`cached` 单价缺省时等于 `input`。

### 3.4 输出

| 输出 | 实现 | 内容 |
| --- | --- | --- |
| 访问日志 | spdlog 异步 logger + `rotating_file_sink`（50MB × 5），路径 `log.access_file` | 每请求一行 JSON（RequestRecord） |
| 运行日志 | spdlog，控制台 + `log.file` | 启动配置摘要、熔断状态变化、TLS 错误、慢请求（latency > `log.slow_ms`） |
| `/metrics` | Prometheus 文本格式 0.0.4 | 见 §3.5 |
| `/admin/requests?limit=N` | 最近 256 条 RequestRecord 的环形缓冲（mutex） | JSON 数组，新的在前 |
| `/admin/stats` | 从 Metrics 汇总 | 按用户、按模型：请求数、各 outcome 计数、token、成本、TTFT/延迟的 p50/p95（由直方图估算）；缓存、熔断、连接池、活跃流 |

### 3.5 指标

用户、模型别名、上游名都来自配置，**启动时注册所有标签组合**，运行期只做原子加，不需要注册锁。

| 指标 | 类型 | 标签 |
| --- | --- | --- |
| `gateway_requests_total` | counter | user, model, outcome |
| `gateway_tokens_total` | counter | user, model, upstream, type（prompt/cached/prompt_computed/completion）；只累加上游报告的值 |
| `gateway_usage_unreported_total` | counter | upstream, field（prompt/completion）：上游没有报告、因而没有计入的次数 |
| `gateway_wasted_tokens_total` | counter | upstream, type：失败尝试和被取消请求消耗的 token（上游报告值） |
| `gateway_cost_usd_total` | counter | user, model |
| `gateway_request_duration_seconds` | histogram | model（桶：0.05 0.1 0.25 0.5 1 2.5 5 10 30 60 120 300） |
| `gateway_ttft_seconds` | histogram | model（桶：0.05 0.1 0.2 0.5 1 2 5 10 30 60） |
| `gateway_upstream_attempts_total` | counter | upstream, result（ok/retryable/fatal） |
| `gateway_upstream_ttfb_seconds` | histogram | upstream |
| `gateway_upstream_circuit_state` | gauge | upstream（0 closed / 1 open / 2 half_open） |
| `gateway_tls_handshakes_total` | counter | upstream, resumed（true/false） |
| `gateway_cache_requests_total` | counter | result（hit/miss） |
| `gateway_cache_bytes`、`gateway_cache_entries` | gauge | |
| `gateway_cache_saved_cost_usd_total` | counter | model |
| `gateway_ratelimit_rejections_total` | counter | scope, dimension |
| `gateway_cancellations_total` | counter | model, stage（acquiring/awaiting/streaming/backoff） |
| `gateway_active_streams` | gauge | |
| `gateway_backpressure_active`、`gateway_backpressure_pauses_total` | gauge / counter | |
| `gateway_upstream_pool_idle` | gauge | upstream（main loop 每秒汇总各 loop 的计数） |

直方图实现：每个桶一个 `std::atomic<uint64_t>`，sum 用微秒整数 `atomic<uint64_t>`，输出时换算成秒。

## 4. Token 计量

### 4.1 原则

1. **记账只用上游报告的数字**。RequestRecord、指标、成本、限流结算里的 token 数全部来自上游协议中的计数字段，网关自己不数 token、不估算。
2. 上游确实没有报告时，该字段记为 `null`，`usage_source` 标为 `unreported`，并计入 `gateway_usage_unreported_total`，**不用估算值填补**。
3. 唯一使用估算的地方是准入时的**限流预留**（§1.3）：请求发出之前不可能知道真实用量。预留只是占用额度，不算消耗，
   不进记录和成本，最终一定按 §4.5 的真实值结算。
4. 服务端消耗按**每次尝试**分别计量，请求的总消耗是所有尝试之和。重试、降级中失败的尝试如果已经让上游做了预填充或生成，也照样计入。

### 4.2 上游计量能力 `metering`

每个上游在配置里声明计量能力，网关据此向上游请求注入计数开关：

| `metering` | 适用上游 | 网关注入到上游请求 | 能拿到的精确计数 |
| --- | --- | --- | --- |
| `llamacpp` | `openai-chat` kind 指向 llama-server（Qwen3.6、Qwen3.8） | `timings_per_token: true`；流式再加 `return_progress: true` | 预填充期间：`prompt_progress{total, cache, processed}`；之后每个 chunk：累计的 `timings{cache_n, prompt_n, predicted_n}`；结束时 `usage` |
| `vllm` | `openai-chat` kind 指向 vLLM | `stream_options.continuous_usage_stats: true` | 每个 chunk 的累计 `usage` |
| `final`（默认） | 云端 OpenAI 兼容服务（DeepSeek 等）、所有 `anthropic` kind | OpenAI 加 `stream_options.include_usage: true` | 只在流结束时有 `usage`；Anthropic 另外在 `message_start` 里给出精确的输入与缓存 token |

已核实的 llama.cpp（5b59b83）行为：`timings_per_token` 为真时每个 OpenAI chunk 都带 `timings`（`server-context.cpp` 2082 行）；
`return_progress` 为真时开始处理 prompt 就发 0% 进度，处理中持续发进度（3418、3808 行）；
Anthropic `/v1/messages` 流的 `message_start` 带 `input_tokens` 与 `cache_read_input_tokens`，但中间事件不带 timings，所以 `qwen36-anth` 按 `final` 计量。

**对下游透明**：客户端没有自己请求这些字段时，网关从转发的 chunk 里删掉 `timings` 和 `prompt_progress`，
只含进度、没有 delta 内容的 chunk 整个丢弃。这需要对每个 chunk 解析 JSON、删字段、再序列化；观察 usage 时本来就要解析，额外开销是一次 `dump()`。

### 4.3 UsageMeter

`gateway/usage_meter.hpp`，每次尝试一个实例，由流处理器喂入上游事件：

```cpp
struct AttemptUsage {
    std::optional<std::int64_t> prompt;          // prompt 总 token（含缓存命中部分）
    std::optional<std::int64_t> cached;          // 命中 KV/提示缓存、未重新计算的部分
    std::optional<std::int64_t> prompt_computed; // 上游实际做了预填充的 token 数（中途中止时小于 prompt - cached）
    std::optional<std::int64_t> completion;      // 上游已生成的 token 数（累计值）
    enum class Source { none, rejected, upstream_final, upstream_partial, unreported } source = Source::none;
};
```

| 上游事件 | 更新 |
| --- | --- |
| llama.cpp `prompt_progress` | `prompt = total`，`cached = cache`，`prompt_computed = processed` |
| llama.cpp chunk 的 `timings` | `cached = cache_n`，`prompt_computed = prompt_n`，`prompt = cache_n + prompt_n`，`completion = predicted_n` |
| vLLM chunk 的 `usage` | `prompt`、`completion`、`cached`（`prompt_tokens_details.cached_tokens`）覆盖 |
| Anthropic `message_start` | `cached = cache_read_input_tokens`，`prompt = input_tokens + cache_read + cache_creation` |
| 结束时的 `usage`（OpenAI 末块 / Anthropic `message_delta`） | 全部字段覆盖为最终值，`source = upstream_final` |

所有字段都是**累计值，只覆盖不相加**。

`prompt_computed` 的补全规则（只在上游没有直接报告它时使用）：一旦观察到**任何输出事件**（文本、思考、工具调用增量）
或 Anthropic 的 `message_start`，说明预填充已经完成，`prompt_computed = prompt - cached`。
在此之前被中止、且上游也没有报告进度的尝试，`prompt_computed` 为 null（`unreported`）。

### 4.4 各种结局下的计量

| 尝试的结局 | 计量 |
| --- | --- |
| 正常结束 | 结束时的 `usage`，`upstream_final` |
| 建连失败、上游返回 4xx/5xx 状态（包括 429、503 Loading model） | 上游没有开始处理，全部为 0，`rejected` |
| llamacpp 上游：响应头前或首个进度前超时/断开 | 没有任何字节说明上游还没开始处理（进度在开始处理 prompt 时就会发出），全部为 0，`rejected` |
| 被取消、提交后中断、idle 超时、提交前超时（已有字节） | 按 §4.5 收尾后取 meter 的最后值，`upstream_partial`；某个字段仍为空则 `unreported` |

### 4.5 取消时拿到最后的精确计数

中止一个正在进行的尝试时，按下面的顺序处理，不能直接 `close()`：

1. `shutdown(SHUT_WR)`（TLS 先发 `close_notify`）：上游立刻看到 FIN，llama.cpp 的 `is_connection_closed` 随即成立并中止任务。**取消延迟以这一步为准**。
2. 非阻塞地把 socket 里已经到达的字节全部读出（直到 `EAGAIN`，上限 1MB），只喂给 SSE 解析器和 UsageMeter，不转发。
   背压暂停期间内核缓冲里可能积了不少数据，这一步把它们里面最新的累计计数取出来。
3. `close()`。

这样记下的 `completion` 是**上游最后发出的那个 chunk 里的累计值**，是上游自己报告的精确数。
协议层面唯一拿不到的，是上游在最后一次发送之后、发现连接断开之前又生成的 token：llama.cpp 每生成一个 token 就发送一次，这部分通常是 0–1 个。
文档和 `/admin` 输出里要如实说明这一点，不能把它说成"完全精确"。

`final` 计量的上游（DeepSeek 等）被取消时协议里没有 output 计数：`completion = null`、`source = unreported`；
Anthropic 上游的 `prompt`/`cached` 在 `message_start` 之后已知，照常记录。

### 4.6 请求级汇总与成本

- RequestRecord 的 `attempts[]` 保存每次尝试的 `{upstream, result, AttemptUsage}`；请求级的 `prompt_tokens`、`completion_tokens`、`cached_tokens`、`prompt_computed_tokens` 是各尝试之和。
  任何一次尝试的某个字段是 `unreported`，请求级的这个字段就记为 `null`，同时 `usage_complete = false`；已知部分另外放在 `attempts[]` 里。
- **消耗量（consumed，全项目唯一口径）**：每次尝试 `consumed = cached + prompt_computed + completion`，即上游实际查过缓存、实际算过、实际生成的 token。
  正常结束时它等于 `prompt + completion`；预填充中途被中止时只算已处理的部分。
  账本的 `billable_tokens`、Redis 配额计数、限流结算、`gateway_tokens_total` 都用它；三个字段有任一为 null，这次尝试的 consumed 为 null。
- **预判量（projected）**：`prompt + completion`（prompt 取总数）。只用于配额**检查**，让超大 prompt 在开始计算前就被拒绝（[usage-ledger.md §5.4](usage-ledger.md#54-流式中的上报与截断判断)）；不入账。
- 成本：每次尝试按实际服务它的上游单价计算后求和：

  ```text
  cost = (prompt_computed × input + cached × cached_price + completion × output) / 1e6
  ```

  字段为 `null` 时这次尝试的成本也记 `null`，不按部分计算。
- 限流结算（§1.3）：两个 tpm 桶按各尝试 consumed 之和结算；有尝试的 consumed 为 null 时，这部分**不退还**预留额度（保守处理），并且不计入 token 指标。

### 4.7 其他用到 token 数的地方

| 位置 | 做法 |
| --- | --- |
| Anthropic 编码器 `message_start.usage.input_tokens`（[protocol-translation.md §4.4](protocol-translation.md#44-anthropic-编码器入口为-anthropic)） | commit 时 meter 已经有精确 prompt 就用它（llamacpp 上游在首个 token 前一定已经收到进度或 timings）；`final` 计量的 OpenAI 上游此时还没有，写 0，精确值放进 `message_delta.usage.input_tokens` |
| `POST /v1/messages/count_tokens` | 转发给路由里第一个声明了 `count_tokens: true` 的上游（llama-server 在 `{base_url}/messages/count_tokens` 提供精确计数）；没有这样的上游就返回 501，不做本地估算 |
| 缓存命中 | 用条目里保存的原始上游 usage（它本身是上游报告的精确值），`source = cache` |
| fault_upstream | 自带确定性的"分词"：按空白切分的词数就是 token 数，每个文本增量就是一个 token；同样支持 `timings_per_token` 与 `return_progress`，用来精确验证 §4.5 |

## 5. 配置 `home/gateway.json`

启动时一次性加载并校验，之后只读。字符串值 `env:NAME` 在加载时替换为环境变量值，变量不存在则启动失败并报出变量名（不打印值）。

```json
{
  "server": {
    "listen": "0.0.0.0:8080",
    "io_threads": 8,
    "max_body_bytes": 8388608,
    "header_timeout_ms": 30000,
    "body_timeout_ms": 60000,
    "keepalive_idle_ms": 75000
  },
  "admin": { "listen_local_only": true },
  "shutdown": { "drain_seconds": 30 },
  "log": {
    "level": "info",
    "file": "logs/gateway.log",
    "access_file": "logs/access.log",
    "slow_ms": 30000
  },
  "stream": { "high_water_bytes": 262144, "stall_ms": 60000 },
  "tls": { "verify_peer": true, "ca_file": "" },
  "dns": { "cache_ttl_seconds": 60 },
  "cache": { "enabled": true, "max_bytes": 268435456, "max_entry_bytes": 4194304, "ttl_seconds": 600 },
  "users": {
    "gk-dagent": { "name": "dagent", "rpm": 60, "tpm": 400000 },
    "gk-bench":  { "name": "bench",  "rpm": 0,  "tpm": 0 }
  },
  "upstreams": {
    "qwen36": {
      "kind": "openai-chat",
      "base_url": "http://127.0.0.1:10010/v1",
      "model": "Qwen3.6-35B-A3B",
      "metering": "llamacpp",
      "count_tokens": true,
      "timeouts": { "connect_ms": 3000, "ttft_ms": 90000, "idle_ms": 60000, "total_ms": 600000, "precommit_ms": 100000 },
      "pool_idle_seconds": 4,
      "breaker": { "failure_threshold": 5, "cooldown_ms": 30000 }
    },
    "qwen36-anth": { "kind": "anthropic", "base_url": "http://127.0.0.1:10010/v1", "model": "Qwen3.6-35B-A3B",
                     "default_max_tokens": 8192, "count_tokens": true },
    "qwen38": { "kind": "openai-chat", "base_url": "http://127.0.0.1:10009/v1", "model": "Qwen3.8-Flash-Next", "metering": "llamacpp",
                "extra_body": { "chat_template_kwargs": { "enable_thinking": true } } },
    "deepseek": { "kind": "openai-chat", "base_url": "https://api.deepseek.com", "model": "deepseek-flash", "api_key": "env:DEEPSEEK_API_KEY" },
    "fault": { "kind": "openai-chat", "base_url": "http://127.0.0.1:19000/v1", "model": "fault", "metering": "llamacpp" }
  },
  "models": {
    "qwen":       { "targets": ["qwen36", "qwen38"], "rpm": 120, "tpm": 1000000, "default_max_tokens": 4096,
                    "retry": { "max_attempts": 2, "base_ms": 500, "max_ms": 8000, "max_retry_after_ms": 10000 } },
    "qwen-anth":  { "targets": ["qwen36-anth"] },
    "smart":      { "targets": ["deepseek", "qwen36"] },
    "fault-demo": { "targets": ["fault", "qwen36"], "retry": { "max_attempts": 3, "base_ms": 200, "max_ms": 2000 } }
  },
  "pricing": {
    "qwen36":   { "input": 0.10, "output": 0.40 },
    "qwen38":   { "input": 0.05, "output": 0.20 },
    "deepseek": { "input": 0.27, "output": 1.10, "cached": 0.07 }
  }
}
```

| 字段 | 默认 | 说明 |
| --- | --- | --- |
| `upstreams.*.kind` | 必填 | `openai-chat` 或 `anthropic` |
| `upstreams.*.base_url` | 必填 | 解析规则见 [http.md §9](http.md#9-url) |
| `upstreams.*.api_key` | 空 | 空则不发鉴权头；建议 `env:NAME` |
| `upstreams.*.timeouts.*` | 见 [proxy-session.md §3](proxy-session.md#3-超时) | |
| `upstreams.*.pool_idle_seconds` | 4 | 必须小于对端 keep-alive 超时 |
| `upstreams.*.extra_body` | 无 | 只补缺失的顶层字段 |
| `upstreams.*.default_max_tokens` | 8192 | anthropic 上游缺 max_tokens 时使用 |
| `upstreams.*.metering` | `final` | `llamacpp` / `vllm` / `final`，见 [§4.2](#42-上游计量能力-metering)；`llamacpp`、`vllm` 只能用于 `openai-chat` kind |
| `upstreams.*.count_tokens` | false | 上游是否提供 `/messages/count_tokens` |
| `models.*.targets` | 必填 | 引用不存在的上游 → 启动失败 |
| `models.*.retry` | `{2, 500, 8000, 10000}` | |
| `users` 的键 | — | 就是客户端使用的 API key；`name` 用于日志和指标标签 |

本地模型的单价是为演示成本统计设的名义价格。

账本、配额相关的 `users.*.quota`、`instance_id`、`postgres`、`redis`、`ledger`、`quota` 段见 [usage-ledger.md §8](usage-ledger.md#8-配置)。
这些段是**可选**的：没有 `postgres` 段时不启动 ledger loop、不做 3a 检查、不入账；没有 `redis` 段时不创建 RedisClient。
G00–G10 阶段用不带这些段的配置运行；G11 起 `home/gateway.json` 加上它们。
任何 key 配置了 `quota` 但没有 `redis` 段，或者配置了 `redis` 但没有 `postgres` 段（无法做启动重建），都是启动错误。

启动校验失败（未知字段类型错误、引用不存在、端口占用、PG/Redis 首次连接失败等）时打印具体原因并以非零码退出。不做热加载。
