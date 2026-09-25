# 里程碑与验收记录

归档日期：2026-09-25。G00–G15 已完成真实验收（G08/TLS 移出范围），本页保留当时的构建、运行证据及问题修复过程。
历史条目中的“未开始”“受环境阻塞”“下一任务”、旧模型名和旧启动命令描述当时状态，不代表当前操作指南。
当前用法见 [运行与配置](../operations.md)，验证方法见 [验证指南](../validation.md)，设计约束见 [文档索引](../README.md)。
原始脚本、日志、截图保留在仓库根目录的 `temp/`，未纳入 Git；历史数值不等同于持续有效的运行状态。

## 里程碑状态

```mermaid
flowchart LR
    G00 --> G01 --> G02 --> G03 --> G04 --> G05 --> G06 --> G07
    G07 --> G09 --> G10 --> G11 --> G12 --> G13 --> G14 --> G15
```

| 编号 | 交付 | 状态 | 前置 |
| --- | --- | --- | --- |
| G00 | 环境、工程骨架、依赖、预设 | 真实验收通过 | 无 |
| G01 | base 与 Buffer | 真实验收通过 | G00 |
| G02 | EventLoop / Channel / EpollPoller / TimerQueue / 线程池 | 真实验收通过 | G01 |
| G03 | Acceptor / TcpServer / TcpConnection / PlainTransport / 水位回调 | 真实验收通过 | G02 |
| G04 | HTTP 服务端、配置加载、main、`/healthz` | 真实验收通过 | G03 |
| G05 | 上游客户端：Resolver / Connector / 响应解析 / chunked / 连接池 | 真实验收通过 | G04 |
| G06 | OpenAI 透传代理（非流式 + 流式）、鉴权、路由、SSE | 真实验收通过 | G05 |
| G07 | 背压、取消传播、超时（含 2026-09-25 审查返工） | 真实验收通过 | G06 |
| G09 | 可观测性：记录、访问日志、指标、成本、admin 接口 | 真实验收通过 | G07 |
| G10 | 限流 | 真实验收通过 | G09 |
| G11 | 用量账本（PostgreSQL）与配额（Redis）、流式超额截断 | 真实验收通过 | G10 + 容器已启动 |
| G12 | 故障注入上游、重试、降级、熔断 | 真实验收通过（原 fault/503 验收 + 新 Nemotron/Gemma4 真实降级验收） | G11 |
| G13 | 响应缓存 | 真实验收通过 | G12 |
| G14 | Anthropic 入口与上游、协议转换 | 真实验收通过 | G13 |
| G15 | DAgent 对接、压测、设计文档 | 真实验收通过 | G14 |

项目只做本地：所有上游都是本机明文 HTTP 服务，不接远端/云端模型。原 G08（TLS）已移出范围，编号不复用。

## 历史执行记录


### G15：DAgent 对接、压测与设计文档（2026-09-25）

- 状态：真实验收通过。用户本轮要求继续完成 G14/G15，按此授权备份并修改 `~/DAgent/home/models.json`，新增 gw-qwen/gw-claude，保留默认配置；备份为同目录 models.json.G15.bak（0600）。DAgent 代码未改，实际验收使用隔离 home 与临时端口。
- 七类 DAgent 场景全部完成：双协议文件工具、多轮持久会话、Esc 取消/槽位释放、TPM=20000 的429后成功重试、真实Nemotron降级/熔断跳过、usage与PG核对、额度4700中途截断和下一轮429。
- 用量：13条请求PG与访问日志输入/输出逐条相等；DAgent三轮合计与API/访问日志的input=55635、output=1470、cached=50387一致。另有独立UI用量截图和API核对。
- 配额：真实DAgent输入3840+输出861达到4701时按4700额度截断；下一轮Retry-After=7887，DAgent直接报告等待过长。场景发现并修复：Redis预输出阶段超额未发429；当前llama.cpp progress.processed包含cache引起重复计量；在途增量与Redis旧回复口径不一致。
- 偏离旧规格：`prompt_computed=processed` 按现场上游源码与真实缓存进度修正为 `max(0,processed-cache)`；录制显示total1020/cache1016/processed1016→1020，对应computed0→4。该调整修正旧规格对上游字段的解释，不引入token估算。
- 压测：release healthz 550341.09 req/s、p99 2.07ms；缓存347312.35 req/s、p99 3.19ms；直连/网关20次TTFB中位数4.1615/45.7400ms；1000流0错误、RSS峰值28.88MiB；100真实慢读者触发100次背压，RSS峰值22.25MiB。
- 测量说明：慢读者先禁读10秒，再约200B/s读取5秒后取消，接收缓冲收紧；首轮客户端大缓冲结果不作为背压证据。RSS含基线、读批次与allocator，不能等同100×high_water；TTFB差值包含响应提交时机。详见benchmark.md。
- 文档：8份 `docs/design/{net,http,proxy,protocol,ratelimit,cache,observability,benchmark}.md`，README架构/能力/启动/DAgent接入说明。
- 构建与单测：dev/release通过；144/144单测通过。配额修复双轴复核通过。
- 证据：`temp/G15/RESULT.md`，`dagent-20260925-213853/`，`tui-20260925-214435/`（除旧quota失败），`tui-20260925-214816/`（最终quota），`tui-20260925-215228/`（UI usage），`benchmark-20260925-214107/`、`benchmark-20260925-214516/`。
- 运行状态：临时验收进程均退出；三模型保持运行；用户可按README启动8080网关。未部署常驻网关；默认配置PG/Redis仍为显式可选，本次账本验收使用临时启用配置。

### G14：Anthropic 入口、上游与协议转换（2026-09-25）

- 状态：真实验收通过。新增 `proto/{anthropic,translator}`；纯协议请求/响应映射、中立事件解码/编码、Anthropic 计量观察与透传块状态；接入 `/v1/messages`、`/v1/messages/count_tokens`。
- 每次尝试重新编码请求并独占解码器/编码器，混合目标链沿用原重试状态机；同协议保留原始响应，跨协议输出入口格式，错误保持上游状态和原始 message。Anthropic 取消排空分别保留最后 message_start / message_delta 用量。
- 跨协议并行工具流：当前工具参数立即输出，其他工具暂存至能顺序打开对应块，避免向已关闭块继续发送参数；单工具暂存上限 4MiB。这细化了规格“切换关闭”规则，以保留交错工具参数并维持合法 Anthropic 块状态。
- 配额截断分别使用转换编码器或透传实际块状态收尾；修复审查发现的透传块编号错配、畸形 tool_choice 异常边界、Anthropic 必填 max_tokens 校验。
- 构建与单测：dev/release 构建通过；143/143 单测通过，包含真实录制双并行工具样本的协议往返、请求映射、非流式互转、错误、交错块及截断状态。
- 真实 SDK 验收：OpenAI/Anthropic 两入口 × 两上游协议的普通对话、SSE、流式工具调用及结果回传，共 15/15 通过；count_tokens 与直连一致（14）；跨协议图片 400；无计数能力 501。工具调用使用显式 tool_choice，避免模型选择直接作答导致误判。
- 边界验收：畸形 tool_choice、缺少 max_tokens、错误 max_tokens 类型均为 400且进程继续服务；原生 Anthropic 配额截断流只有一个 message_start/message_stop，块均关闭，后续请求 429。
- 证据：`temp/G14/tests-final.log`、`live-20260925-213540/`、`edges-20260925-213944/`；SDK 版本和接口以安装于 `temp/G14/venv` 的实际客户端为准，新版 Anthropic SDK 的 temperature 经 extra_body 传入。
- 双轴审查原阻塞项修复后复核通过；下一任务 G15。

### G09–G13 验收提交前修复（2026-09-25）

- 状态：真实验收通过。提交前双轴审查发现并修复：final 上游取消伪造零计量、缓存末条突破分片预算、半开探测超过冷却期被重复放行、PG 健康状态跨线程竞争、异步提交未排空即退出，以及账本对象销毁缺少线程屏障。
- 实现：final 的未知计量保留 null；允许淘汰超预算的最后条目；探测必须显式结束或释放；PG connected 原子发布；受锁保护的 in-flight 行数计入 backlog；main 等待 ledger loop 清理完成后 join。
- 验证：dev 构建成功，134/134 单测通过；真实 qwen-final 取消为 unreported/null；小缓存重复请求仍 MISS；持有 PostgreSQL 表锁延迟写入时发送 SIGTERM，退出等待约 3.475 秒，退出后待提交账单存在。
- 证据：`temp/G13/upstream-refresh/review-tests.log`、`live-20260925-212229/review-summary.json` 及对应访问日志。两轴复核原阻塞项均通过。

### G13 补充：本机模型服务入口调整（2026-09-25）

- 状态：**真实验收通过**。原 G13 缓存验收结论保留；本次新入口的 20 项真实检查全部通过。
- 依据：现场读取 `~/LLM/README.md`、`~/LLM/configs/{qwen36,nemotron,gemma4}.env` 和 `llm.sh status`；不沿用旧服务快照。
- 配置：移除 qwen38 / 10009，新增 nemotron / 10011（`Nemotron-3.5-Lightning-30B-A3B`）与 gemma4 / 10012（`gemma-4-26B-A4B-it`）；qwen36 / 10010（`Qwen3.6-35B-A3B`）保留。
- 路由：兼容别名 `qwen` 按 qwen36 → nemotron → gemma4 降级；新增三个同名独立路由。新模型使用 llamacpp 计量和服务端采样默认值，不复制旧 Qwen3.8 的模板覆盖。未知模型单价不编造，按现有未配置单价规则成本记 0。
- 文档：同步当前入口表、统一 `llm.sh` 启停命令、配置示例与 G12/G15 后续降级场景；历史执行记录和验收材料原样保留。未修改 C++ 实现，未推进 G14。
- 构建与单测：`cmake --preset dev && cmake --build --preset dev -j4` 成功；`ctest --preset dev --output-on-failure`，133/133 通过。
- 实际网关检查：使用仅覆盖监听地址和日志路径的临时配置启动真实网关；`/healthz` 200，`/v1/models` 与配置中的 7 个路由完全一致；检查后该临时网关正常退出（exit 0）。
- 真实验收：用户要求继续验收后启动三模型，现场 `/health` 和模型标识匹配；三模型非流式、SSE、流式工具调用与工具结果回传（17+25=42）、取消传播均通过。取消后约 0.103s 观测到槽位释放，部分用量正常记录。
- 缓存：三模型非流式/SSE 各自 MISS → HIT，响应体一致，HIT attempts=0、约 1ms；无 temperature 和 bypass 请求不命中；指标 6 MISS / 6 HIT。
- 降级：通过临时配置的不可达本机端口制造建连失败，qwen36 不可达时 attempts=3 到 nemotron；前两目标不可达时 attempts=5 到 gemma4；未停用在用模型。补齐 G12 原真实备用模型降级验收。
- 计量：22 条完整响应的 prompt/completion 与访问日志逐条一致；建连失败尝试计量为 0；3 条取消记录为 upstream_partial 且字段完整。本次未配置 PG/Redis，未重验 G11 数据库账本。
- 环境处理：普通命令启动的模型进程在命令返回后消失，首次连接拒绝记录保留于 `live-20260925-211057/`；随后用用户级临时服务 `llmgateway-acceptance-models.service` 托管现有启动脚本，三模型保持运行，未修改模型配置。临时验收网关均正常退出。
- 证据：`temp/G13/upstream-refresh/RESULT.md`、`accept.py`、`ctest-current.log`；最终真实记录位于 `live-20260925-211235/`，包含 results/summary、请求响应、访问日志、指标与取消计量记录。
- 偏离锁定部分：用户要求更新服务入口，将旧 Qwen3.8 备用目标替换为 Nemotron/Gemma4；线程、所有权与代理状态机语义未变。

### G13：响应缓存（2026-09-25）
- 完成内容：`gateway/response_cache`（16 分片 LRU + 每片 mutex/字节预算）；SHA-256 键与请求体规范化；ProxySession 准入第 5 步（命中回放、MISS 标记、流式暂存与写回、命中记录/saved cost）；main 每 30 秒清扫 + gauge。
- 构建：dev/asan/tsan 成功。
- 单测：LruCacheTest 8 例（命中/未命中、LRU 顺序与字节预算、惰性过期、sweep、单条超限、键规范化、user 忽略、stream/protocol/usage 参与、stream_options 规范化）；全量 133。
- 真实验收（temp/G13/gateway*.json、日志与 headers）：
  1) 非流式 temperature=0 两次：MISS(957ms, `x-gateway-cache: MISS`) → HIT(0.95ms, attempts:0, body 完全相同)，记录 outcome=cache_hit、cost=0、saved=2.7e-05、usage_source=cache；
  2) 流式两次：MISS(781ms) → HIT(0.74ms)，流内容逐字节相同、均含 [DONE]；
  3) 不带 temperature 不缓存、`x-gateway-cache: bypass` 绕过、`force` 在 temperature≠0 时可缓存；
  4) ttl_seconds=5：MISS→HIT→6 秒后 MISS；
  5) max_bytes=64KB：250 个不同请求后 entries=176、bytes=61776（分片预算内），最近的命中、最早的已被淘汰；
  6) /metrics：`gateway_cache_requests_total{result=hit/miss}` 与 `gateway_cache_saved_cost_usd_total{model=qwen}` 随命中增长。
- 偏离规格之处及原因：无（未配置单价的上游 saved cost 记 0，符合 §3.3 口径）。
- 遗留项与下一任务：G14（Anthropic 入口与协议转换）。

### G12：故障注入上游、重试、降级、熔断（2026-09-25，进行中）
- 完成内容：`test/fault_upstream/`（路径指令、times 状态、确定性分词、timings/progress、drop/hang/error、限速、连接日志）；`gateway/{retry_policy,circuit_breaker}`；ProxySession 尝试计划（skip 熔断目标）、失败分类表、Backoff 状态、降级、耗尽错误映射与 `x-gateway-upstream/attempts`；`/metrics` 熔断 gauge 与熔断状态日志。
- 构建：dev/asan/tsan 成功。
- 单测：RetryPolicyTest 5（失败分类表逐项、退避上下界、Retry-After 秒/HTTP-date）、CircuitBreakerTest 6（阈值打开、成功重置、半开单探测、冷却翻倍上限、取消释放探测、按上游隔离）；全量 133。
- 真实验收（temp/G12/，11 项 fault 场景全部通过）：
  1) status=503,times=1 → 200，attempts=2，upstream=fault-503；
  2) status=429,retry_after=1,times=1 → 退避 1.01s 成功后 200，attempts=2；
  3) status=429,retry_after=60 → 超 max_retry_after_ms 立即降级 qwen36，attempts=2；
  4) status=500 → fault 2 次后降级，attempts=3，upstream=qwen36；
  5) status=400 → 原样透传（fault 错误体），attempts=1；
  6) drop_after=0（流式）→ 首事件前断开，透明重试+降级，attempts=3（2 fault + qwen36），客户端 200 且收到 [DONE]；
  7) drop_after=10 → 客户端收到 10 个事件+流内错误、无 [DONE]，outcome=stream_interrupted，completion=10 与 fault 日志 "drop_after sent=10" 相等；
  8) hang_after=5、idle_ms=3000 → 3.0s 后流内错误，completion=5 = fault "hang_after sent=5"；
  9) ttfb_ms=5000、ttft_ms=2000 → 两次超时重试后降级 qwen36（4.7s）；
  10) 熔断：连续请求 status=500，第 3 个请求期间打开（日志 `circuit breaker fault-breaker -> open`），之后 attempts=1 直接 qwen36；冷却 2s 后半开探测（日志 half_open → open），`gateway_upstream_circuit_state{fault-breaker}=1`；
  11) 取消：rate=5,tokens=1000，timeout 3 → fault "conn close while streaming sent=15" 与 curl 被杀相差 1ms；记录 completion=15 == fault sent，cancel_latency_us=28。
  计量核对：场景 1/2/4/5 被拒尝试全部为 0/`rejected`；7 的 completion=10；9 的超时尝试 0；请求总量=各尝试之和；`gateway_wasted_tokens_total` 与失败尝试消耗一致（drop10: completion=10/prompt=1/computed=1；rate: 30/4/4）；精确计量场景 fault-meter completion=10=上游确定性计数。
- 返工记录（本轮修复）：`UpstreamExchange::handleClose` 中 `feedEof()` 已回调一次 `on_error`，随后又手动回调导致同一尝试被处理两次、破坏重试计划；修复为 feedEof 后若已 finished 直接返回；同时给 exchange 回调加 attempt epoch 守卫、失败尝试的 exchange 统一 forceClose 后再重试。
- 真实模型场景：
  - ✅ 加载中 503 重试：`~/LLM/stop-qwen36.sh` 后立即 `run-qwen36.sh` 并发请求（模型 `qwen-retry` targets=[qwen36-load]，长 precommit、高熔断阈值）：记录 attempts=[连接失败(0) → 503 Loading ×2 → 200]，退避 1157→2330→3224ms（debug 日志），7.2s 加载完成后成功。
  - ⛔ 停机降级 qwen38：本环境 **不允许启动 Qwen3.8**（用户约束），无法执行；fault 场景 3/4/6/9 已覆盖同链条的降级逻辑（两个目标间切换）。
- 偏离规格之处及原因：无。
- 遗留项与下一任务：真实模型降级（qwen36 停、qwen38 起）待环境允许时人工执行；G13。

### G11：用量账本与配额（2026-09-25）
- 完成内容：`deploy/docker-compose.yml`、`deploy/.env.example`、`sql/migrations/001_init.sql`（CMake 配置阶段嵌入）；`store/{resp,redis_client,pg_client}`（RESP2 自研客户端、libpq 异步接入 Reactor）；`gateway/{ledger,quota,usage_api}`（迁移、批量入账、周期键、会话上报/截断、查询接口、对账）；ProxySession 准入 3a/3b、流式上报与截断、finish 入账与配额补报；main 装配 ledger loop、每 io loop RedisClient、迁移→预编译→启动重建；规格 §9 的 Redis/账本指标。
- 构建：dev/asan/tsan 成功。
- 单测：RespTest 11、QuotaPeriodTest 7、UsageRowTest 6；全量 133。
- 真实验收（temp/G11/RESULT.md、gw*.log、drive.py、各场景输出；容器 llm-gateway-postgres-1:5433 / valkey-1:6380）：
  1) 两实例同时启动迁移：schema_migrations 恰好 1 行，两实例均正常服务；
  2) 20 个请求（流式/非流式/取消）：usage_records 20 行，token 与访问日志逐行相等，4 个取消行 usage_complete=true（llamacpp 已报告全部字段）；
  3) 汇总一致性 SQL 返回 0 行；
  4) 同一 x-request-id 两次：2 行、ledger_id 不同；
  5) /v1/usage、/v1/usage/requests 翻页正常；Redis 日计数与 PG 汇总一致（1667）；
  6) 准入超额：429 insufficient_quota，Retry-After=23251≈到本地零点；
  7) 流式截断：finish_reason=length + usage 真实值 + [DONE]；账本 truncated=t、billable=501（限额剩余 500，超额 1 token）；下一个请求 429；
  8) 58.7k prompt：首个 prompt_progress 即 429（无流内容）；input_tokens=58715、billable=0；槽位立即空闲；
  9) 双实例并发 8 个流：4 截断 + 4 个 429；总超额 128（≈4×report_every_tokens）；结束后 reconcile diff=0；
  10) 停 PG：新请求 503（fail closed）；期间队列中的行恢复后补记，恢复后 200；
  11) 停 Redis：reject → 503；allow → 200 且入账；恢复后 200；
  12) FLUSHALL 后重启：SET NX 从 PG 重建 day/month=3285，reconcile diff=0；
  13) 压力下 SIGTERM：5 个流全部入账（access 5 行 = PG 5 行），队列写完、干净退出。
- 返工/修复记录：迁移先查 schema_migrations 再应用；pipeline 模式禁用 PQsendQuery（改 PQsendQueryParams）；预编译必须在建表后；PgClient 析构漏摘 Channel 导致退出断言；RedisClient 需成员持有 Connector；配额重建必须在读连接就绪后；慢请求日志记录被 move 的字段。
- 偏离规格之处及原因：月配额用尽的 Retry-After 按 30 天近似（规格未定义月边界秒数口径）；迁移 SQL 在 CMake 配置阶段从 `sql/migrations/` 嵌入。
- 遗留项与下一任务：G12。

### G10：限流（2026-09-25）
- 完成内容：`gateway/{token_bucket,rate_limiter}`（4 桶、注入时钟、固定层级锁序、结算多退少补、waitFor）；准入第 4 步接线；响应 429 的 Retry-After/scope/dimension 与通过时的 x-ratelimit-* 头；配置支持 rpm_burst/tpm_burst；`proto/token_estimate` 增加 JSON 估算（唯一估算点）。
- 构建：dev/asan/tsan 成功。
- 单测：TokenBucketTest 8、RateLimiterTest 8（含 16 线程 × 1000 次并发精确扣减）；全量 133。
- 真实验收（temp/G10/gateway.json、s1/*、s2_*、s3_*）：
  1) gk-dagent rpm=5，10 个并发 → 5×200 + 5×429；429 带 Retry-After:12、scope=user、dimension=requests；200 带 x-ratelimit-*;
  2) qwen tpm=3000、max_tokens=2500：第一个 200（约 3s 完成、实际用量 159）、紧接第二个 429（scope=model、dimension=tokens、Retry-After 40s）；结算后退还预留，5 秒内第三个 200（未结算需等待 40s+）；
  3) 取消的请求按 consumed=157 结算，其余预留退还（取消后立即再发 200）。
- 偏离规格之处及原因：无。
- 遗留项与下一任务：G11。

### G09：可观测性（2026-09-25）
- 完成内容：`gateway/{request_record,metrics,pricing,access_log,admin}`；ProxySession 结束统一汇总（RequestRecord/访问日志/指标/最近 256 条/慢请求日志/账本预留位）；Router 增加 /metrics、/admin/stats、/admin/requests（admin.listen_local_only）与 401 留痕；main 装配与 pool idle gauge。
- 构建：dev/asan/tsan 成功。
- 单测：无新增（可观测性以真实验收为准），全量 133 通过。
- 真实验收（temp/G09/access.log、metrics_final.txt、admin_stats.json、final_cancel_record.json、ttft_curl.txt 等）：
  1) 一次流式请求后 access.log 末行 JSON 字段齐全；ttft_ms=89 与 curl starttransfer 90.5ms 相差 <2ms；
  2) /metrics 指标齐全、随请求增长（含 cost/tokens/histogram/pool）；
  3) 成本手算一致：4×0.1+11×0.1+1243×0.4=498.7µ→0.000499；取消 25×0.1+357×0.4=145.3µ→0.000145；
  4) /admin/requests、/admin/stats 返回合理数据；取消请求 usage_source=upstream_partial、attempts[] 完整；
  5) `final` 计量的取消（qwen-final）：completion_tokens=null、usage_complete=false、usage_source=unreported、gateway_usage_unreported_total 的 qwen36-final prompt/completion 各 +1；槽位立即空闲；
  6) 记录与指标不含 token_estimate 数：预留量（G10 起）只用于准入，记录 token 全部来自上游。
- 返工/修复记录：访问日志 logger 逐条 flush；admin 渲染 try/catch 防进程被可观测性缺陷拖垮；修复 /admin/stats 的 null json 崩溃；修复 cost 单位换算与慢请求日志取 moved 字段。
- 偏离规格之处及原因：无。
- 遗留项与下一任务：G10。

### G07 返工（2026-09-25）
- 完成内容：返工清单第 1–15 项。
  - net：`handleClose` 把连接析构推迟到本轮事件之后；写失败只标记并延后关闭；`PlainTransport` 持有并关闭 fd；删除 TLS（`net/tls`、握手状态、Transport 的 TLS 钩子）。
  - http：连接池去掉 https 分支，新连接直接交付 Lease（不再自持回调），`acquire(..., allow_idle)`；`HttpServer::stop()`/`forEachConnection()`；下游 `SO_SNDBUF` 改为 `stream.downstream_sndbuf_bytes`；`https://` 的 base_url 配置报错。
  - gateway：stall 先 finish 再关下游；背压暂停时停 idle；超时/中断统一 `abortUpstream()` 排空计量，排空只解析最后一个事件；复用连接首字节前失败换新连接重发一次；`/v1/messages` 在 G14 前 404；`Router::cancelAll`。
  - app：退出轮询改为 `runEvery`；drain 超时先 `cancelAll` 再退出；先 `server.stop()` 再析构 Router；可执行文件输出到 `build/<preset>/`。
- 构建：dev / asan / tsan 成功，无警告。
- 单测：73/73（新增 `ConfigTest.HttpsUpstreamRejected`、`ConfigTest.DownstreamSendBuffer`；url_test 改为只接受 http）；dev、asan 通过，tsan 通过。
- 真实验收（Qwen3.6 :10010，最终代码；脚本 `temp/G07/rework.py`、`temp/G07/rework/stuck_probe.py`、`temp/G07/run.py`，输出在 `temp/G07/rework/final_*.out`）：
  1) 原 7 项全部重跑通过（`final_original.out`）：背压 pause=23/resume=12、暂停期间脚本自动采到上游 Recv-Q=122841、RSS 6.9MB 平稳；stall 25s 结束；流式取消 completion=362 = 客户端最后 predicted_n，cancel_latency_us=48；预填充取消 prompt=82683/computed=4096/completion=0；建连取消 rejected；ttft 504；进度保活 200（38.0s）。
  2) stall 记录 `outcome=downstream_stalled`（修复前为 cancelled）；默认值 `stall_ms = idle_ms = 60000` 的慢读者 64s 后 `downstream_stalled`（修复前为 stream_interrupted），另按 5000/5000 等比缩小再验一次，结果相同。
  3) 暂停不触发 idle：`idle_ms=3000`、`stall_ms=60000`，客户端停读 20 秒后继续 → `outcome=ok`、收到 `[DONE]`（比规格的 idle 5000 / 暂停 10s 更严）。
  4) 预填充取消：debug 日志逐次打出 progress computed=0/2048/4096，最后一次 4096 与记录的 `prompt_computed_tokens` 相等。
  5) 优雅退出：流式请求进行中 SIGTERM → 流正常结束（有 `[DONE]`），进程在流结束后 50ms 内退出，退出码 0。
  6) `POST /v1/messages` → 404；`https://` 上游 → 启动失败并指出上游名。
  7) asan 取消压力：32 并发长输出流 × 10 轮、每个在 1–3 秒内随机被杀 → sanitizer 报告 0；每轮后 8 个新连接 `/healthz` 全部 200（覆盖全部 io loop）；打开的 fd 始终 24（修复前每轮 +72）；退出 58ms。
  8) asan 下重跑场景 1、3、5 与 stall：均通过，sanitizer 报告 0。asan 下取消的记录 completion=637 而客户端只收到 predicted_n=99，是 asan 版本转发慢、差额停在上游 socket 里被排空计入，符合"≥ 客户端值、多出的是排空读到的量"。
- 返工中新发现并修复的问题（第 12–15 项）：连接 fd 从不关闭；排空逐事件解析 JSON 在 asan 压测下把 loop 卡住数分钟（每轮一个 loop 停止处理新连接）；退出时已析构的 handler 被 io 线程回调；写下游失败同步关闭导致重入 SSE 解析器。
- 偏离规格之处及原因：
  - 下游 `SO_SNDBUF` 默认 64KB（可配置）：不限制时内核发送缓冲自动增长，用户态高水位很难触发，背压不可观测。
  - 验收地址：connect 超时用 10.77.77.1（本机透明代理会立即"连上"10.255.255.1）。
  - 第 6 项（复用连接首字节前失败重发）的真实复现放到 G12，用 fault_upstream 的 `close_before_head` 验证。
- 遗留项与下一任务：G09。

### 范围调整与 G07 审查（2026-09-25）
- 范围：项目只做本地，删除 DeepSeek 上游与 `smart` 模型，原 G08（TLS）移出范围、编号不复用；`final` 计量的本地验证改用 `qwen36-final`（同一个 llama-server，按 final 计量），相关验收挪到 G09。
- 规格改动：README、architecture、net-reactor（§6 改为传输层、§7 增加连接关闭后的寿命规则、§11 去掉 TLS 字段并要求 acquire 恰好回调一次）、http §9、proxy-session（§3 idle 暂停、§4.2 stall 收尾顺序）、policies（配置示例、metering、指标）、usage-ledger、protocol-translation。
- G07 状态退回"实施中"，返工清单与返工验收见 G07 小节。
- 已有执行记录保留原文作为历史；其中关于 TLS/G08 的表述以本条为准。

### G06：OpenAI 透传代理（2026-09-24）
- 完成内容：http/sse（SseParser）、proto/{neutral,errors,token_estimate,openai}（请求元信息、透传最小改写、流观察器）、gateway/{usage_meter,auth,router,proxy_session}；`/v1/models`；main 装配 Router 与每 io loop 的 UpstreamPool；删除 G05 的 `--probe`。
- 构建：dev/asan 成功。
- 单测：新增 sse_test（10 例，含逐字节等价）、usage_meter_test（4 例，基于录制的真实 Qwen3.6 流逐步比对、任意截断值、rejected/unreported）、OpenAiStreamObserverTest（3 例，进度块丢弃、计量字段增删、错误块）；共 71 例全通过。
- 真实验收（temp/G06/run.py、result.txt、stream_*.txt、upstream_400.txt、gateway.log）：
  1) 流式透传：1133 事件，末行 `data: [DONE]`，含 reasoning_content 与 finish_reason=stop，无 timings/prompt_progress 泄漏；
  2) 客户端没要 usage 时收不到 usage 块；要了则收到，且网关日志 prompt/cached/completion 与客户端 usage 逐一相等；
  3) 客户端自带 timings_per_token 时字段保留；
  4) 非流式响应 usage 与日志相等；
  5) 工具调用流式：tool_calls 分片 + finish_reason=tool_calls；
  6) 错误 key 401、未知 model 404（model_not_found）；
  7) 上游 400（prompt 280010 tokens 超上下文）原样透传状态码与 message，attempts=1 不重试；
  8) 同一 curl 两次请求复用下游连接，上游连接复用（idle 日志）。
- 偏离规格之处及原因：无（`SPDLOG_DEBUG` 需 SPDLOG_ACTIVE_LEVEL，已在构建里全局打开；日志 flush_every(1s) 便于验收）。
- 遗留项与下一任务：G07。

### G07：背压、取消、超时（2026-09-24）
- 完成内容：ProxySession 的 ttft/idle/total/precommit/stall 定时器、进度事件切换 idle、背压暂停/恢复（下游高水位 → 上游停读 + stall）、取消传播（下游断开 → SHUT_WR + 排空计量 + 关闭，cancel_latency_us）、失败与流内错误收尾；下游连接 SO_SNDBUF 限制为 64KB（让用户态高水位及时生效并约束内核内存）；TcpConnection 支持 setSendBufferSize。
- 构建：dev/asan 成功。
- 单测：现有 54 例全通过。
- 真实验收（temp/G07/run.py、result.txt、backpressure*.txt、stall.txt、cancel_stream*.txt、prefill_cancel.txt、connect_cancel.txt、ttft_timeout.txt、progress_keepalive.txt）：
  1) 背压（high_water=4096, stall_ms=10000）：慢读者 5s 不读 → 日志 pause=21、暂停期间 `ss` 显示上游 socket Recv-Q=121277（backpressure_ss.txt）、网关 RSS 12.4MB 平稳，读回后 resume=11；
  2) stall：完全不读 → 10s 后 `downstream_stalled`（paused_for=10000ms），上游连接关闭，/slots 全部 false；
  3) 流式中取消：`timeout 5 curl` → cancelled stage=streaming cancel_latency_us=23（只统计到发出 FIN），槽位立即空闲，completion=361 ≥ 客户端最后 predicted_n=361，usage_source=upstream_partial；
  4) 预填充中取消：prompt=82658、computed=4096、completion=0、usage_source=upstream_partial；
  5) 建连中取消：stage=acquiring，usage 全 0、usage_source=rejected；
  6) ttft 超时：metering=final 的上游 ttft_ms=50 → 504 upstream_timeout（llamacpp 上游有进度事件，不会误超时）；
  7) 进度保活：ttft_ms=2000、长 prompt 预填充 38.3s 的请求 200 完成，记录 ttft_ms>2000。
- asan 复验：场景 1、3、5 用 asan 构建重跑通过，LeakSanitizer/AddressSanitizer 报告 0（此前发现并修复 Connector 自捕获与退出 drain checker 的引用环泄漏）；asan 71 例单测、tsan 全通过。
- 偏离规格之处及原因：
  - 验收地址/参数按本机环境调整：10.255.255.1 被本机透明代理立即“连上”，connect 超时改用 10.77.77.1；预填充速度比规格假设快，进度保活用 ttft_ms=1000、预填充中取消用随机数字 prompt 避免 KV 缓存命中。
  - 下游 SO_SNDBUF 限制 64KB：否则内核发送缓冲会吸收全部数据，用户态高水位（4KB）永远不触发，背压不可观测。
- 遗留项与下一任务：G08（TLS 真实验收；TLS 传输已在 G05 实现并参与编译）。

### G00：环境与工程骨架（2026-09-24）
- 完成内容：libssl-dev/libpq-dev 已装；根 CMakeLists/CMakePresets（dev/release/asan/tsan + build/test presets）/cmake/deps.cmake（spdlog v1.17.0、CLI11 v2.7.2、googletest v1.15.2、nlohmann vendor）/src、test 的 CMakeLists；AGENTS.md、.gitignore、home/gateway.json。
- 构建：dev 成功；asan/tsan/release 均成功。
- 单测：0/0（G00 时点）。
- 真实验收：`cmake --preset dev` 成功、空库目标可构建、`ctest --preset dev` 可运行。
- 偏离规格之处及原因：无。
- 遗留项与下一任务：G01。

### G01：base 与 Buffer（2026-09-24）
- 完成内容：base/{noncopyable,timestamp,log,current_thread,strings,random}、net/buffer（readv+64KB 栈缓冲、findCRLF/findDoubleCRLF、前移与扩容）。
- 构建：dev/asan 成功。
- 单测：buffer_test 8 例全通过（含 readFd 100KB、逐字节/扩容查找）。
- 真实验收：单测通过，asan 无报告。
- 偏离规格之处及原因：无。
- 遗留项与下一任务：G02。

### G02：Reactor 核心（2026-09-24）
- 完成内容：EventLoop（eventfd 唤醒、跨线程任务、上下文槽）、Channel（tie、EPOLLRDHUP）、EpollPoller（LT）、TimerQueue（timerfd + canceling_timers_ 语义）、EventLoopThread/Pool。
- 构建：dev/asan/tsan 成功。aarch64 上 TSan 需要 `setarch -R`（内核 mmap_rnd_bits 高），tsan preset 用单个 add_test + launcher 解决，已在 test/CMakeLists.txt 注明。
- 单测：timer_queue_test 8 例（含跨线程 runInLoop 1 万次、cancel self、round-robin、长定时器精度）全通过。
- 真实验收：单测在 dev/tsan 通过。
- 偏离规格之处及原因：loop() 在每轮 poll 前先执行 pending functors（避免 loop() 前入队的任务等满一次 poll 超时）。
- 遗留项与下一任务：G03。

### G03：TCP 服务端连接（2026-09-24）
- 完成内容：inet_address/socket/acceptor（EMFILE 备用 fd）/tcp_server/tcp_connection（水位、abortAndDrain、TLS 读写重试钩子）/transport（Plain）。main 临时挂 echo。
- 构建：dev/asan 成功。
- 单测：无新增（复用 G01/G02）。
- 真实验收（temp/G03/run.py + result.txt + echo.log/water.log/emfile.log）：
  1) 基本回显 + 200 并发连接 200/200 成功，连接均匀分布到 8 个 io loop；
  2) high water 64KB 触发（output=79872），慢读者读完后 write complete；
  3) `ulimit -n 64` 下 100 连接，EMFILE 日志 70 条，2 秒 CPU ticks=0。
- 偏离规格之处及原因：无。
- 遗留项与下一任务：G04。

### G04：HTTP 服务端 + 配置 + main（2026-09-24）
- 完成内容：http/{http_request,request_parser,response_writer,http_server,url}、gateway/{config,context}、main（CLI11、signalfd 优雅退出、/healthz、404/405）。
- 构建：dev/release/asan 成功。
- 单测：request_parser_test 11 例、url_test 5 例、config_test 8 例全通过（含逐字节等价、pipelining、431/413/411/400、100-continue、env: 替换、引用校验）。
- 真实验收（temp/G04/run.py、result.txt、各 *.curl、server.log、bench.txt）：
  1) `curl -v /healthz` 200；`/nope` 404；POST /healthz 405 + Allow；两次请求 curl 打印 “Re-using existing connection”；
  2) release 基线压测（wrk 不可用，用 asyncio 客户端替代）：256 并发 15s，1047708 请求，QPS≈69827，p50=3.6ms，p99=4.1ms；
  3) `kill -TERM` 后 drain 退出码 0，日志含 shutdown signal received / gateway stopped。
- 偏离规格之处及原因：wrk 未安装且无 sudo，压测用 Python asyncio 客户端替代，数值仅作基线。
- 遗留项与下一任务：G05。

### G05：上游客户端（2026-09-24）
- 完成内容：net/{resolver,connector,tls}、http/{chunked,response_parser,upstream_client,upstream_pool}；main 临时 `--probe`；TLS 传输提前实现（UpstreamPool 的 https 分支需要链接符号，G08 再验收）。
- 构建：dev/asan 成功。
- 单测：chunked_test 6 例、response_parser_test 8 例（三种定界、1xx 跳过、逐字节、错误体截断）全通过。
- 真实验收（temp/G05/run.py、result.txt、probe_*.txt、gateway.log）：
  1) /health 与 /v1/models 200；
  2) localhost 走 Resolver（日志 resolved in 0 ms + cache hit）；
  3) 同一进程连续两次 probe：false→true；间隔 5s（>pool_idle_seconds=4）后再次 probe reused=false（日志 closing expired idle connection）；
  4) 127.0.0.1:1 refused（kind=1）；10.77.77.1 connect timeout（kind=2，10s）。
- 偏离规格之处及原因：验收地址从 10.255.255.1 改为 10.77.77.1——本机透明代理会立即“连上”10.255.255.1，无法产生 connect 超时。
- 遗留项与下一任务：G06（删除 --probe）。


每个任务完成后追加一节：

```markdown
### Gxx：<名称>（YYYY-MM-DD）
- 完成内容：
- 构建：dev / asan / tsan 结果
- 单测：通过数 / 总数
- 真实验收：逐项结果与证据路径（temp/Gxx/…）
- 偏离规格之处及原因：
- 遗留项与下一任务：
```

## G16–G20：API Key 管理页面

2026-09-25，状态：真实验收通过。无登录的本机管理页、动态密钥与按密钥用量已交付。详细结论见 [本期验收记录](api-key-management.md)。
