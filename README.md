# LLMGateway

基于自研 Reactor（muduo 思路）的 LLM 网关：OpenAI / Anthropic 兼容入口，向上游模型服务转发并做计量、限流、缓存、账本与可观测。

只接本机的明文 HTTP 上游（Qwen3.6、Qwen3.8、fault_upstream），不接远端/云端模型，不实现 TLS。

当前里程碑：**G00–G07 真实验收通过**（Reactor、TCP、HTTP、上游客户端、OpenAI 透传代理、背压/取消/超时，G07 含 2026-09-25 审查返工）。
G09–G15（可观测性、限流、账本与配额、重试熔断、缓存、协议转换、DAgent 对接与压测）待推进；原 G08（TLS）已移出范围。

## 架构

```text
app ──► gateway ──► http ──► net ──► base
            │                  ▲
            ├──► store ────────┤ (Redis/PG 客户端，G11)
            └──► proto ────────┘ (协议转换，纯逻辑)
```

| 模块 | 内容 |
| --- | --- |
| base | noncopyable、Timestamp、日志（spdlog 异步）、字符串工具 |
| net | EventLoop/Channel/EpollPoller/TimerQueue、TcpConnection、Resolver、Connector、PlainTransport |
| http | 增量 HTTP/1.1 解析、ResponseWriter、SseParser、chunked、UpstreamExchange、UpstreamPool |
| proto | 中立消息模型、OpenAI 编解码与透传改写、错误体 |
| gateway | Config、Auth、Router、ProxySession（状态机）、UsageMeter |
| app | CLI11、signalfd 优雅退出、装配 GatewayContext |

线程模型：one loop per thread；一个下游请求的全部 I/O 对象（含其上游连接）在同一个 io loop 上，热路径无锁。

## 构建

```bash
sudo apt install -y libssl-dev libpq-dev        # 首次（libssl-dev 只为 libcrypto 的 SHA-256）
cmake --preset dev && cmake --build --preset dev -j
ctest --preset dev --output-on-failure          # 单测（gtest，73 例）
```

预设：`dev` / `release` / `asan` / `tsan`（aarch64 下 tsan 需要 `setarch -R`，已在 test/CMakeLists.txt 处理）。

## 运行

```bash
./build/dev/llm-gateway -c home/gateway.json
```

`home/gateway.json` 默认监听 `0.0.0.0:8080`，上游指向本机 Qwen3.6（`127.0.0.1:10010`）与 Qwen3.8（`10009`）。

```bash
export GW=http://127.0.0.1:8080
export AUTH='Authorization: Bearer gk-dagent'
export JSON='Content-Type: application/json'

# 流式
curl -sN $GW/v1/chat/completions -H "$AUTH" -H "$JSON" \
  -d '{"model":"qwen","stream":true,"messages":[{"role":"user","content":"用三句话介绍 epoll"}]}'
# 非流式
curl -s $GW/v1/chat/completions -H "$AUTH" -H "$JSON" \
  -d '{"model":"qwen","messages":[{"role":"user","content":"1+1=?"}],"max_tokens":256}'
# 模型列表
curl -s $GW/v1/models -H "$AUTH"
# 健康检查
curl -s $GW/healthz
```

行为要点：

- 透传上游输出结构（reasoning_content、tool_calls 分片、finish_reason、`[DONE]`），网关注入的计量字段（`timings`/`prompt_progress`）默认对客户端不可见；
- token 计量只取上游报告值，`prompt/cached/prompt_computed/completion` 记录在运行日志（G09 起进访问日志与指标）；
- 背压：下游慢读触发高水位 → 暂停读上游 + stall 定时器；恢复后继续；
- 取消传播：下游断开 → 向上游发 FIN + 排空已到达数据取最后一个累计计数 → 立即释放模型槽位；
- `/v1/messages`（Anthropic 入口）在 G14 之前返回 404；
- 超时：connect / ttft / idle / total / precommit / stall 全部按上游配置生效；llamacpp 上游的进度事件会把 ttft 切换为 idle。

## 测试与验收

- 单测：`test/unit/`（Buffer、定时器/Reactor、URL、请求/响应/chunked/SSE 解析、配置、计量等）。
- 真实验收材料：`temp/G00`–`temp/G07`（脚本、日志、请求/响应摘要）。
- 故障注入上游 `fault-upstream` 属于 G12。

## 已知限制（当前里程碑）

- 尚无访问日志/指标（G09）、限流（G10）、账本与配额（G11）、重试/熔断（G12）、缓存（G13）、Anthropic 入口与协议转换（G14）；
- 下游连接 SO_SNDBUF 默认 64KB（`stream.downstream_sndbuf_bytes`），用于让用户态高水位（背压）及时生效并约束内核内存；
- 上游 `base_url` 只支持 `http://`。
