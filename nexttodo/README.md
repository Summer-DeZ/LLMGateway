# LLMGateway 实施规格：入口

状态：**全部任务未开始**。编写日期：2026-09-24。源码基线：空骨架（`README.md`、`src/CMakeLists.txt` 为空）。

本目录是交给实现者的执行规格：基于 muduo 思路自研 Reactor，实现一个 OpenAI / Anthropic 兼容的 LLM 网关，
并让 `~/DAgent` 只改配置即可通过网关访问模型。

**范围：只做本地。** 所有上游都是本机的明文 HTTP 服务（Qwen3.6、Qwen3.8、fault_upstream），
不接入任何远端/云端模型，不实现 TLS（原 G08 已移出范围，编号不复用）。配置里出现 `https://` 的 `base_url` 在加载时报错。

## 1. 要展示的能力

| 功能 | 背后的后端能力 | 规格位置 |
| --- | --- | --- |
| SSE 流式转发 | 上游边生成边转发；下游读得慢时的背压和缓冲区控制 | [proxy-session.md §4](proxy-session.md#4-流式转发与背压) |
| 取消传播 | 客户端断开时立刻中断上游请求，不再消耗 token | [proxy-session.md §5](proxy-session.md#5-取消传播) |
| 限流 | 按用户、按模型的令牌桶，请求数和 token 数两个维度 | [policies.md §1](policies.md#1-限流) |
| 重试与降级 | 超时、429、5xx 按策略重试或切到备用模型；熔断 | [proxy-session.md §6](proxy-session.md#6-重试降级与熔断) |
| 缓存 | 完全相同的请求直接命中；LRU 淘汰 + TTL 过期 | [policies.md §2](policies.md#2-响应缓存) |
| 可观测性 | 每个请求的延迟、TTFT、token 用量和成本 | [policies.md §3](policies.md#3-可观测性) |
| 用量账本 | 每个 API key 的每个请求入账 PostgreSQL（输入/输出/缓存命中/成本），日汇总、查询、对账；计费不可用时拒绝服务 | [usage-ledger.md §4](usage-ledger.md#4-postgresql-账本) |
| 配额与截断 | Redis 共享日/月额度（多实例），准入拒绝，流式中超额直接截断 | [usage-ledger.md §5–6](usage-ledger.md#5-redis-配额) |
| 数据库客户端 | libpq 异步接口与自研 RESP 客户端都挂在 Reactor 上 | [usage-ledger.md §3](usage-ledger.md#3-新模块-storegw_store依赖-gw_net) |
| 精确计量 | token 用量全部取自上游报告（含取消、失败尝试的服务端消耗），拿不到就如实标记，绝不估算 | [policies.md §4](policies.md#4-token-计量) |
| 双协议 | OpenAI Chat 与 Anthropic Messages 入口/上游任意组合，流式互转 | [protocol-translation.md](protocol-translation.md) |

## 2. 阅读顺序

| 顺序 | 文档 | 解决的问题 |
| --- | --- | --- |
| 1 | [architecture.md](architecture.md) | 模块、依赖方向、线程模型、对象所有权、目录与构建目标 |
| 2 | [net-reactor.md](net-reactor.md) | EventLoop / TcpConnection / Transport / Resolver / Connector / 连接池的接口与不变量 |
| 3 | [http.md](http.md) | 服务端与客户端 HTTP/1.1 解析、chunked、SSE、路由、错误体 |
| 4 | [proxy-session.md](proxy-session.md) | 一次代理请求的状态机：背压、取消、超时、重试、降级、熔断 |
| 5 | [protocol-translation.md](protocol-translation.md) | OpenAI ↔ Anthropic 请求、响应、流事件的逐字段映射 |
| 6 | [policies.md](policies.md) | 限流、缓存、指标/日志/成本、token 计量、完整配置格式 |
| 6b | [usage-ledger.md](usage-ledger.md) | PostgreSQL 账本、Redis 配额、流式截断、用量查询、部署容器 |
| 7 | [implementation-tasks.md](implementation-tasks.md) | G00–G15 任务：改哪些文件、单测、真实验收、完成标准、执行记录 |

按任务推进时，先读任务对应的规格章节，再动手。

## 3. 执行规则

1. `.hpp` 只放 `src/public/<模块>/`，`.cpp` 只放 `src/private/<模块>/`；第三方 vendor 头放 `src/public/lib/`。
2. 单元测试放 `test/unit/`（gtest），只测纯逻辑组件（解析器、令牌桶、LRU、协议转换等），不在单测里起模型。
3. 唯一允许的测试辅助服务是 `test/fault_upstream/`（故障注入上游，规格见 [implementation-tasks.md §4](implementation-tasks.md#4-故障注入上游-fault_upstream)），
   它只用于制造真实模型难以稳定复现的故障（429、流中断开、挂起）。功能正确性以真实模型为准。
4. 真实验收的脚本、日志、截图放 `temp/<任务编号>/`，不放进源码树。
5. 不修改、停止或重启正在被别人使用的模型服务；需要停 Qwen3.6 做降级演示时，先确认没有其他使用者。
6. 真实密钥与数据库密码只从环境变量或 `deploy/.env` 读取（配置里写 `env:NAME`），不得写进配置文件、文档、日志和验收材料。
7. 状态取值：未开始、实施中、构建通过待真实验收、真实验收通过、受环境阻塞。构建通过不等于完成。
8. 本规格锁定的是**对外行为、线程归属、对象所有权和状态机语义**；类名、局部算法、容器选型可自行决定，
   偏离锁定部分时在任务记录里写明原因。

## 4. 环境准备

| 项 | 现状 | 动作 |
| --- | --- | --- |
| GCC 13.3 / CMake 3.28 / Ninja 1.11 | 已有 | — |
| OpenSSL 头文件 | **缺** `libssl-dev`（只有运行库 3.0.13） | `sudo apt install -y libssl-dev`，G00（只用 libcrypto 的 SHA-256） |
| spdlog / CLI11 | DAgent 已从 GitHub 拉过，网络可用 | FetchContent |
| googletest | 未拉取 | FetchContent（v1.15.2） |
| nlohmann/json | `~/DAgent/src/public/lib/nlohmann/` | 复制 vendor |
| libpq 头文件 | 未装（PostgreSQL 16 服务端已装但集群未启动，不使用它） | `sudo apt install -y libpq-dev`，G00 |
| PostgreSQL / Valkey | 本机 6379/3306 上的容器属于 RAGFlow，不共用 | `docker compose -f deploy/docker-compose.yml up -d`（:5433、:6380），G11 之前 |
| wrk | 可能未装 | `sudo apt install -y wrk`（G15 压测用） |

测试上游：

| 名称 | 启动/停止 | 地址 | 说明 |
| --- | --- | --- | --- |
| Qwen3.6（主） | `~/LLM/run-qwen36.sh` / `~/LLM/stop-qwen36.sh` | `127.0.0.1:10010` | 别名 `Qwen3.6-35B-A3B`；4 槽 × 131072；同时提供 `/v1/chat/completions` 与 `/v1/messages`；服务端默认 temp 0.6，思考模式开启（流里有 `reasoning_content`） |
| Qwen3.8（备） | `~/LLM/run-qwen.sh` / `~/LLM/stop-qwen.sh`（会停掉**所有** llama-server） | `127.0.0.1:10009` | fallback 目标 |
| fault_upstream | 本项目构建产物 | `127.0.0.1:19000` | 故障注入 |

已核实的 llama.cpp（5b59b83）行为，可直接用作验收场景：

- 模型加载期间所有接口返回 `503 {"error":{"message":"Loading model","type":"unavailable_error","code":503}}`——真实的可重试 5xx；
- 流式生成时检测客户端断开并中止任务（`server-http.cpp` 的 `is_connection_closed`）——取消传播的真实观察点；
- `GET /slots` 默认开启，可查看槽位是否在处理任务——确认取消后槽位立即释放；
- 4 个槽位满时新请求排队——可观察排队对 TTFT 的影响。

注意 `run-qwen36.sh` 的硬限制：不超过 4 槽 × 131072 上下文，DAgent 里指向它的 `context_window` 不得大于 131072。

## 5. 恢复一次中断的实施

读 [任务状态表](implementation-tasks.md#1-任务图与状态)，找到第一个前置已满足但未真实验收的任务，
核对已有文件和 `temp/<任务编号>/` 里的证据，补完剩余部分。每完成一个任务，在 [执行记录](implementation-tasks.md#6-执行记录) 追加：
日期、完成内容、构建结果、单测结果、真实运行结果、证据位置、遗留项。

## 6. 最终交付

- 可执行文件 `llm-gateway`（配置 `home/gateway.json`）与 `fault-upstream`；`deploy/` 下的 PostgreSQL + Valkey 容器与迁移脚本；`gateway_unit_tests` 单测全部通过；
- DAgent 通过 `gw-qwen`（OpenAI 入口）和 `gw-claude`（Anthropic 入口）两个配置项完成真实对话、工具调用、取消、限流重试、降级；
- `docs/design/` 下各模块设计文档（写实测数据），根目录 README 概述架构与能力。
