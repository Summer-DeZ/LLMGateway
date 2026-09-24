# 用量账本与配额（PostgreSQL + Redis）

每个 API key 用了多少 token（输入、输出、缓存命中）要**可靠落盘、可查询、可对账**；每个 key 有日/月总量配额，
超额时准入拒绝、流式中途直接截断。采用常见的后端分工：

| 组件 | 职责 | 为什么是它 |
| --- | --- | --- |
| PostgreSQL 16 | 账本：请求明细 + 按日汇总，计费与对账的唯一依据 | 持久、事务、并发写、多个网关实例共用、SQL 报表 |
| Redis（Valkey 8） | 配额热计数：每个 key 当日/当月已用 token，流式过程中持续累加 | 原子自增 + Lua 脚本，多个网关实例共享额度，亚毫秒延迟 |
| 网关进程内存 | 本请求的计量（UsageMeter）、待上报增量、最近一次得知的剩余额度 | 每个 token 都要判断，不能每次都访问 Redis |

范围：

- 不做：支付、发票、key 的增删改接口、按金额的预算（配额只按 token 计）。
- API key 与配额仍在 `home/gateway.json` 里配置。以后把 key 迁到 PG（存 SHA-256 哈希）是自然的下一步，本期不做。
- 所有 token 数来自上游报告，规则见 [policies.md §4](policies.md#4-token-计量)。

## 1. 限流与配额的区别

| | 限流（[policies.md §1](policies.md#1-限流)） | 配额（本文） |
| --- | --- | --- |
| 控制什么 | 速度：每分钟请求数、token 数 | 总量：每天、每月 token 数 |
| 状态存放 | 单实例内存令牌桶，重启清零 | Redis 计数（多实例共享，AOF 持久化），PG 账本兜底重建 |
| 超出时 | 429，Retry-After 为秒级，客户端退避后能成功 | 准入：429 `insufficient_quota`，Retry-After 到下个周期；流式中：截断 |
| 依据 | 准入时的预留量，结束时按真实值结算 | 只用上游报告的真实值 |

## 2. 部署：独立容器

`deploy/docker-compose.yml`（不复用本机 RAGFlow 的 MySQL/Valkey 容器）：

```yaml
name: llm-gateway
services:
  postgres:
    image: postgres:16
    environment:
      POSTGRES_DB: gateway
      POSTGRES_USER: gateway
      POSTGRES_PASSWORD: ${GW_PG_PASSWORD}        # 来自 deploy/.env，.env 不入库
    ports: ["127.0.0.1:5433:5432"]
    volumes: ["../data/postgres:/var/lib/postgresql/data"]
    healthcheck: { test: ["CMD-SHELL", "pg_isready -U gateway"], interval: 5s }
  valkey:
    image: valkey/valkey:8
    command: ["valkey-server", "--appendonly", "yes", "--appendfsync", "everysec", "--maxmemory-policy", "noeviction"]
    ports: ["127.0.0.1:6380:6379"]
    volumes: ["../data/valkey:/data"]
```

- 端口 5433/6380 与本机已有的 5432（系统 PG 集群）、6379/3306（RAGFlow）错开；只绑定 127.0.0.1。
- `data/` 加入 `.gitignore`。`noeviction`：配额计数绝不能被内存淘汰策略删掉。
- 网关配置用 `env:` 引用连接信息：`"postgres": {"dsn": "env:GW_PG_DSN"}`，
  例如 `GW_PG_DSN="host=127.0.0.1 port=5433 dbname=gateway user=gateway password=…"`；`"redis": {"host": "127.0.0.1", "port": 6380}`。
- 构建依赖：`sudo apt install -y libpq-dev`；CMake 用 `pkg_check_modules(PQ REQUIRED IMPORTED_TARGET libpq)`。
  （`find_package(PostgreSQL)` 需要服务端开发头 `pg_type.h`，只装 libpq-dev 时会失败，所以用 pkg-config。）

## 3. 新模块 `store`（`gw_store`，依赖 `gw_net`）

两个数据库客户端都是**非阻塞、挂在 EventLoop 上**的，与上游 HTTP 客户端同一套 Reactor 用法。

### 3.1 RedisClient：自己实现的 RESP2 客户端

```cpp
class RedisClient : noncopyable, public std::enable_shared_from_this<RedisClient> {
public:
    RedisClient(EventLoop*, Resolver&, RedisOptions);
    void connect();                                                   // 断线自动重连（退避 100ms → 5s）
    // 回调在本 loop 上执行；命令按发送顺序对应回复（FIFO），支持流水线（不等回复就连续发送）
    void command(std::vector<std::string> args, std::function<void(std::expected<RespValue, RedisError>)> cb);
    bool connected() const;
};
using RespValue = std::variant<RespSimple, RespError, std::int64_t, RespBulk /* 可空 */, RespArray>;
```

- 编码：`*<n>\r\n$<len>\r\n<arg>\r\n…`；解码：增量 `RespParser`，逐字节喂入与整段喂入结果一致（单测）。
- 连接用本项目的 `TcpConnection`（明文），`TCP_NODELAY`；每个命令带超时（默认 50ms），超时后该连接上所有待回复命令失败并重连，
  因为 RESP 没有请求 ID，丢掉一个回复后无法再对齐后续回复。
- **每个 io loop 一个 RedisClient**（和 UpstreamPool 一样挂在 loop 的 context 上），会话只用本 loop 的连接，不跨线程。
- 连上后 `SCRIPT LOAD` 两个 Lua 脚本（§5.2）拿到 SHA；调用用 `EVALSHA`，收到 `NOSCRIPT` 时重新加载后重试一次。

### 3.2 PgClient：libpq 异步接口接入 Reactor

| 阶段 | libpq 调用 | Reactor 侧 |
| --- | --- | --- |
| 建连 | `PQconnectStart(dsn)` → 循环 `PQconnectPoll` | 按返回的 `PGRES_POLLING_READING/WRITING` 关注 `PQsocket()` 的读/写 |
| 就绪 | `PQsetnonblocking(conn, 1)`；`PQsendPrepare` 预编译语句 | — |
| 发送 | `PQsendQueryPrepared` / 流水线模式下连续发送 | `PQflush()` 返回 1 表示没写完 → 关注写事件，可写时继续 `PQflush` |
| 接收 | 可读时 `PQconsumeInput()`；`while (!PQisBusy()) PQgetResult()` 直到返回 null | 结果按发送顺序交给回调 |
| 批量 | `PQenterPipelineMode` + 多条语句 + `PQpipelineSync` | 以 `PGRES_PIPELINE_SYNC` 标记一批结束 |
| 断线 | `PQstatus == CONNECTION_BAD` | 移除 Channel（重连后 socket 可能变化），退避重连，未完成的请求全部失败回调 |

```cpp
class PgClient : noncopyable, public std::enable_shared_from_this<PgClient> {
public:
    PgClient(EventLoop*, std::string dsn, std::vector<PreparedStatement>);
    void connect();
    void execute(std::string_view stmt_name, std::vector<std::optional<std::string>> params,
                 std::function<void(std::expected<PgResult, PgError>)> cb);          // 单条
    void pipeline(std::vector<PgCall> calls, std::function<void(std::expected<void, PgError>)> done);  // 一个事务里的一批
};
```

- 参数全部用文本格式 + 占位符 `$1…$n`，**不拼接 SQL**。
- PgClient 只在**账本专用的 EventLoop 线程**（`ledger loop`）上使用：它持有两个连接，一个写（账本入账），一个读（查询接口）。
  io 线程通过 `ledgerLoop->queueInLoop` 投递任务，结果再 `ioLoop->queueInLoop` 送回。

## 4. PostgreSQL 账本

### 4.1 迁移

`sql/migrations/001_init.sql` 等文件在构建时嵌入（或随安装复制）。启动时：

```sql
SELECT pg_advisory_lock(727001);        -- 多个网关实例同时启动时只有一个执行迁移
CREATE TABLE IF NOT EXISTS schema_migrations (version int PRIMARY KEY, applied_at timestamptz NOT NULL DEFAULT now());
-- 对每个尚未应用的迁移：BEGIN; <迁移 SQL>; INSERT INTO schema_migrations(version) VALUES (n); COMMIT;
SELECT pg_advisory_unlock(727001);
```

### 4.2 表结构（001_init.sql）

```sql
CREATE TABLE usage_records (
    ledger_id          text        PRIMARY KEY,   -- 网关生成的唯一 ID（不用客户端的 x-request-id，见 §4.4）
    client_request_id  text,
    gateway_instance   text        NOT NULL,      -- 哪个网关实例写的，多实例排查用
    key_name           text        NOT NULL,      -- users.*.name；绝不保存 key 原文
    billing_day        date        NOT NULL,      -- 按 started_at（请求收齐时刻）与配置的 UTC 偏移计算，与 Redis 计数用同一个日期
    started_at         timestamptz NOT NULL,
    finished_at        timestamptz NOT NULL,
    ingress            text        NOT NULL,
    model              text        NOT NULL,
    upstream           text,
    stream             boolean     NOT NULL,
    outcome            text        NOT NULL,
    http_status        smallint    NOT NULL,
    input_tokens       bigint,                    -- prompt 总数（含缓存命中）；NULL = 上游未报告
    cached_tokens      bigint,
    computed_input_tokens bigint,                 -- 上游实际做了预填充的 token（prompt_computed）
    output_tokens      bigint,
    billable_tokens    bigint      NOT NULL,      -- 各尝试 consumed 之和（policies.md §4.6）；consumed 为 null 的尝试按 0 计，并使 usage_complete = false
    usage_complete     boolean     NOT NULL,
    cost_micro_usd     bigint,                    -- 百万分之一美元，整数避免浮点累加误差
    truncated          boolean     NOT NULL DEFAULT false,
    latency_ms         integer,
    ttft_ms            integer,
    attempts           jsonb       NOT NULL
);
CREATE INDEX usage_records_key_time ON usage_records (key_name, finished_at DESC);

CREATE TABLE usage_daily (
    key_name            text   NOT NULL,
    day                 date   NOT NULL,
    model               text   NOT NULL,
    requests            bigint NOT NULL,
    input_tokens        bigint NOT NULL,
    cached_tokens       bigint NOT NULL,
    output_tokens       bigint NOT NULL,
    billable_tokens     bigint NOT NULL,
    cost_micro_usd      bigint NOT NULL,
    unreported_requests bigint NOT NULL,          -- usage_complete = false 的请求数，对账时可见
    truncated_requests  bigint NOT NULL,
    PRIMARY KEY (key_name, day, model)
);
```

字段口径：`input_tokens` 与 OpenAI `prompt_tokens` 一致，是 prompt **总数**、**包含**缓存命中部分（Anthropic 上游已由 UsageMeter 换算）；
`billable_tokens` 是 [policies.md §4.6](policies.md#46-请求级汇总与成本) 定义的 consumed（= cached + computed_input + output），正常结束时等于 input + output，
预填充中途中止时只含已处理部分。缓存命中的 token 同样计入配额，只在成本上按 `cached` 单价打折。
汇总表里 NULL 按 0 累加并计入 `unreported_requests`。一个请求整体按 `billing_day`（开始日）入账，跨零点的请求不拆分。

明细表按月分区（`PARTITION BY RANGE (finished_at)`）是数据量大时的常规做法，本期不做，在设计文档里列为扩展项。

### 4.3 入账：一条语句同时写明细和汇总

预编译语句 `ledger_insert`：

```sql
WITH ins AS (
    INSERT INTO usage_records (ledger_id, client_request_id, gateway_instance, key_name, billing_day, started_at, finished_at,
                               ingress, model, upstream, stream, outcome, http_status, input_tokens, cached_tokens,
                               computed_input_tokens, output_tokens, billable_tokens, usage_complete, cost_micro_usd,
                               truncated, latency_ms, ttft_ms, attempts)
    VALUES ($1, …, $24)
    ON CONFLICT (ledger_id) DO NOTHING
    RETURNING key_name, billing_day, model, input_tokens, cached_tokens, output_tokens, billable_tokens,
              cost_micro_usd, usage_complete, truncated
)
INSERT INTO usage_daily AS d
SELECT key_name, billing_day, model, 1, coalesce(input_tokens, 0), coalesce(cached_tokens, 0), coalesce(output_tokens, 0),
       billable_tokens, coalesce(cost_micro_usd, 0), (NOT usage_complete)::int, truncated::int
FROM ins
ON CONFLICT (key_name, day, model) DO UPDATE SET
    requests = d.requests + 1,
    input_tokens = d.input_tokens + EXCLUDED.input_tokens,
    cached_tokens = d.cached_tokens + EXCLUDED.cached_tokens,
    output_tokens = d.output_tokens + EXCLUDED.output_tokens,
    billable_tokens = d.billable_tokens + EXCLUDED.billable_tokens,
    cost_micro_usd = d.cost_micro_usd + EXCLUDED.cost_micro_usd,
    unreported_requests = d.unreported_requests + EXCLUDED.unreported_requests,
    truncated_requests = d.truncated_requests + EXCLUDED.truncated_requests;
```

重复的 `ledger_id` 使 CTE 返回空，汇总不会被重复累加，**幂等**。

### 4.4 LedgerWriter

```cpp
class LedgerWriter : noncopyable {
public:
    bool submit(UsageRow row);        // io 线程调用：加锁入队后立刻返回；队列满返回 false
    bool healthy() const;             // 积压未超过上限、PG 已连接、最近一次提交成功（原子标志，任何线程可读）
    std::size_t backlog() const;
    void flushAndStop();              // 优雅退出：写完队列后返回（最长 shutdown.drain_seconds）
};
```

- 队列上限 `ledger.max_backlog`（默认 100000 行）。ledger loop 每 200ms 或攒够 500 行，用 pipeline 模式提交一个事务：
  `BEGIN` → N 条 `ledger_insert` → `COMMIT` → `PQpipelineSync`。
- 失败（断线、磁盘满）：这批放回队首，重连后重试，错误计数加一。
- 主键 `ledger_id` 由网关生成（16 字节随机数的十六进制）。客户端的 `x-request-id` 可能重复，拿它当主键会让后一个请求被
  `DO NOTHING` 吞掉、少记账，所以它只存进 `client_request_id`。
- **计费不可用就拒绝服务（fail closed）**：`healthy() == false` 时新请求在准入阶段返回 503，不继续提供服务再悄悄丢账。
- 已知丢失窗口：进程崩溃时队列里未提交的行（最多约 200ms 的请求）会丢失；SIGTERM 优雅退出会先 `flushAndStop()`，不丢。
  要求零丢失可以改成每请求同步提交，代价是吞吐降到 PG 提交速度，本期不采用，在设计文档里说明。

## 5. Redis 配额

### 5.1 数据模型

| 键 | 值 | 过期 |
| --- | --- | --- |
| `gw:quota:{key_name}:d:{YYYYMMDD}` | 当日已用 billable token（整数） | 3 天 |
| `gw:quota:{key_name}:m:{YYYYMM}` | 当月已用 | 40 天 |

日期按配置的 `quota.utc_offset_minutes`（默认 480，即 +08:00）计算，取**请求开始时刻**（received_at）所在的日、月：
一个请求的所有上报都加到同一对键上，与账本的 `billing_day` 一致，对账才能对上。`{key_name}` 用花括号是 Redis Cluster 的 hash tag，
保证同一个 key 的日、月计数落在同一个槽里，Lua 脚本可以同时操作两者（单机时不影响）。

计数的含义是"上游已报告的真实消耗"，**进行中的请求也实时累加**。因此不需要单独区分"进行中"和"已结算"：
每次 UsageMeter 更新，就把累计值的增量加上去。

### 5.2 Lua 脚本

`quota_add`（KEYS: day, month；ARGV: delta, day_ttl, month_ttl）：

```lua
local d = redis.call('INCRBY', KEYS[1], ARGV[1])
local m = redis.call('INCRBY', KEYS[2], ARGV[1])
if d == tonumber(ARGV[1]) then redis.call('EXPIRE', KEYS[1], ARGV[2]) end
if m == tonumber(ARGV[1]) then redis.call('EXPIRE', KEYS[2], ARGV[3]) end
return {d, m}
```

`quota_get`（KEYS: day, month）：`return {tonumber(redis.call('GET', KEYS[1]) or '0'), tonumber(redis.call('GET', KEYS[2]) or '0')}`。

### 5.3 三个检查点

| 检查点 | 动作 | 超额时 |
| --- | --- | --- |
| 准入（鉴权后、限流前） | `EVALSHA quota_get` → `used_day`、`used_month`；记住 `remaining = min(日剩余, 月剩余)` | 429；OpenAI `{"type":"insufficient_quota","code":"insufficient_quota"}`，Anthropic `rate_limit_error`，message "token quota exceeded"；`Retry-After` = 距下个周期开始的秒数 |
| 流式进行中 | 见 §5.4 | 未提交：429（同上），不输出任何内容；已提交：**截断**（§6） |
| 结束（finish） | 把尚未上报的增量用 `quota_add` 补上（取消、失败的尝试也一样） | — |

准入检查放在缓存之前：额度用完的 key 连缓存命中也拿不到。缓存命中没有上游消耗，billable 为 0，不访问 `quota_add`，账本照记一行。

### 5.4 流式中的上报与截断判断

每个 token 都访问一次 Redis 太浪费，也没必要。每个会话维护三个值：

```text
consumed   本请求各尝试已知 consumed 之和（policies.md §4.6），随 UsageMeter 更新
projected  本请求各尝试已知 projected 之和（prompt 取总数 + completion），≥ consumed
reported   已经用 quota_add 加到 Redis 的量
remaining  最近一次从 Redis 得知的剩余额度（准入时、每次 quota_add 回复后更新）
```

- 每次 UsageMeter 更新：
  - **本地判断（用 projected）**：`projected − reported > remaining` → 超额：未提交则 429，已提交则截断。不等 Redis。
  - **上报（用 consumed）**：`consumed − reported ≥ quota.report_every_tokens`（默认 32）或距上次上报超过 `report_interval_ms` → `quota_add(consumed − reported)`；
    回复里的 `used` 超过限额 → 同上处理，否则更新 `remaining`。
  - 判断用 projected、入账用 consumed 的原因：prompt 总数在预填充开始时就已知，用它判断可以在算力花出去之前拒绝超大 prompt；
    但实际只处理了一部分就被中止时，只按实际消耗扣额度。
- 同一个 key 有多个并发流（可能分布在不同网关实例）时，每个流都只知道稍旧的 `remaining`，
  超额上限约为"并发流数 × report_every_tokens + 一次 Redis 往返内生成的量"。这是用有限的超额换取不在每个 token 上访问 Redis，属于常规取舍，写进文档。
- **预填充也算**：llamacpp 上游在开始处理 prompt 时就报出 prompt 总数，一个超大 prompt 在输出任何内容前就会触发超额（尚未提交 → 429）。
- `final` 计量的上游（`qwen36-anth`、`qwen36-final`）流中没有计数：准入时把发给上游的 `max_tokens` 限制为
  `min(请求里的 max_tokens，缺省时取 models.*.default_max_tokens, remaining)`。
  这个值来自 Redis 的剩余额度，不是估算。结束时一次性 `quota_add`，超额上限是"一次请求的 prompt"。
- 未报告的用量不会进入 Redis，只在账本里计入 `unreported_requests`，对账时可见。

### 5.5 Redis 故障与重建

- Redis 不可用（未连接或命令超时）时的准入行为由 `quota.on_redis_failure` 决定：`reject`（默认，503，与账本 fail closed 一致）或 `allow`（放行，只记日志和指标）。
- 进行中的流在 Redis 出故障时继续，待上报增量留在会话里，finish 时再试一次；仍失败就放弃这次上报并计数
  （账本仍然正确，Redis 计数由下面的重建机制修正）。
- **启动重建**：每个网关实例启动时，对每个配置的 key 从 PG 读出当日、当月 `billable_tokens` 之和，执行 `SET … NX EX …`。
  NX 保证不会覆盖其他实例正在累加的计数。Valkey 开了 AOF，正常重启不会丢计数；只有数据被清空时这个重建才起作用。
- **对账接口** `GET /admin/quota/reconcile?key=<name>`：对比 Redis 当日计数与 PG 当日汇总，返回两者的差值。
  正常的偏差来自：进行中的请求、账本批量提交的延迟（这两项会收敛）、`allow` 模式下或 finish 时失败的上报、Redis 重建前的请求（这两项不会自动收敛，
  需要人工处理，本期只展示差值）。所有请求结束、队列写完、期间没有 Redis 故障时，差值应为 0。

## 6. 截断的行为

截断 = 按取消的路径停止上游（`abortAndDrain`，[proxy-session.md §5.2](proxy-session.md#52-各状态下的动作)）+ 按协议**正常收尾**下游：

| 入口 | 收尾 |
| --- | --- |
| OpenAI | 结束块 `delta:{}`、`finish_reason:"length"`；客户端请求了 usage 就发 usage 块（记账的真实值）；`data: [DONE]` |
| Anthropic | 关闭打开的内容块；`message_delta` 的 `stop_reason:"max_tokens"` 与 usage；`message_stop` |

- 用 `length` / `max_tokens` 的原因：语义上就是"输出上限到了"，所有客户端都能正确处理，响应保持协议合法。
  截断原因记在账本（`truncated = true`、outcome `quota_truncated`），客户端的下一个请求会收到 429 `insufficient_quota`。
- 截断点如果落在工具调用参数中间，参数 JSON 不完整，与撞上 `max_tokens` 的情况相同，由客户端处理（DAgent 会把解析错误回填给模型）。
- 截断的响应不写缓存。非流式请求无法中途截断：只做准入检查，并按 §5.4 对 `max_tokens` 做限制。

## 7. 查询接口

查询在 ledger loop 的读连接上执行，结果送回发起请求的 io loop；io 线程不做任何阻塞 I/O。

| 接口 | 鉴权 | 返回 |
| --- | --- | --- |
| `GET /v1/usage?start=YYYY-MM-DD&end=YYYY-MM-DD` | 用自己的 key | 配额状态（来自 Redis，实时）+ 按日 × 模型的汇总（来自 PG）+ 区间合计 |
| `GET /v1/usage/requests?limit=50&before=<finished_at>` | 用自己的 key | 请求明细，按 `finished_at` 倒序，用 `before` 翻页（limit 上限 500） |
| `GET /admin/usage?key=<name>&start&end` | admin（仅本机） | 任意 key 的汇总；不带 key 时按 key 分组 |
| `GET /admin/usage/requests?key=<name>&limit&before` | admin | 任意 key 的明细 |
| `GET /admin/quota/reconcile?key=<name>` | admin | Redis 与 PG 的当日/当月计数对比 |

`/v1/usage` 响应示例：

```json
{
  "key": "dagent",
  "quota": {
    "daily_tokens": 2000000, "daily_used": 183520, "daily_remaining": 1816480,
    "monthly_tokens": 30000000, "monthly_used": 912004, "monthly_remaining": 29087996,
    "resets_at": "2026-09-25T00:00:00+08:00"
  },
  "days": [
    { "day": "2026-09-24", "model": "qwen", "requests": 42,
      "input_tokens": 160211, "cached_tokens": 98304, "output_tokens": 23309, "billable_tokens": 183520,
      "cost_usd": 0.025338, "unreported_requests": 0, "truncated_requests": 1 }
  ],
  "total": { "requests": 42, "input_tokens": 160211, "cached_tokens": 98304, "output_tokens": 23309,
             "billable_tokens": 183520, "cost_usd": 0.025338 }
}
```

`days` 最多落后一个账本提交批次（约 200ms）。每个代理响应额外带 `x-gateway-quota-remaining-tokens`（日、月剩余中较小的那个）。

## 8. 配置

```json
"users": {
  "gk-dagent": { "name": "dagent", "rpm": 60, "tpm": 400000,
                 "quota": { "daily_tokens": 2000000, "monthly_tokens": 30000000 } }
},
"instance_id": "gw-1",
"postgres": { "dsn": "env:GW_PG_DSN", "connect_timeout_ms": 3000 },
"redis": { "host": "127.0.0.1", "port": 6380, "password": "", "command_timeout_ms": 50 },
"ledger": { "max_backlog": 100000, "batch_rows": 500, "batch_interval_ms": 200 },
"quota": { "utc_offset_minutes": 480, "report_every_tokens": 32, "report_interval_ms": 100, "on_redis_failure": "reject" }
```

配额 `0` 或缺省表示不限；不配置 `quota` 的 key 不访问 Redis。

## 9. 指标

| 指标 | 类型 | 标签 |
| --- | --- | --- |
| `gateway_quota_rejections_total` | counter | user, period（day/month） |
| `gateway_quota_truncations_total` | counter | user |
| `gateway_redis_commands_total`、`gateway_redis_errors_total` | counter | command / kind |
| `gateway_redis_latency_seconds` | histogram | |
| `gateway_ledger_backlog` | gauge | |
| `gateway_ledger_commit_seconds` | histogram | |
| `gateway_ledger_write_errors_total` | counter | |

## 10. 文件

| 文件 | 内容 |
| --- | --- |
| `deploy/docker-compose.yml`、`deploy/.env.example` | §2 |
| `sql/migrations/001_init.sql` | §4.2 |
| `store/resp.hpp`、`store/redis_client.hpp` | RESP 编解码、RedisClient |
| `store/pg_client.hpp` | PgClient |
| `gateway/ledger.hpp` | `UsageRow`、`LedgerWriter`、查询、迁移执行 |
| `gateway/quota.hpp` | 周期与键名计算、会话侧的上报与截断判断、启动重建 |
| `gateway/usage_api.hpp` | `/v1/usage*`、`/admin/usage*`、`/admin/quota/reconcile` |

（均为 `src/public/<模块>/` 下的 hpp 与 `src/private/<模块>/` 下同名的 cpp。）
