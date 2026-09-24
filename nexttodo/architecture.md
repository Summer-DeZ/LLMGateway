# 架构：模块、线程、所有权与构建

## 1. 模块与依赖方向

```text
app ──► gateway ──► http ──► net ──► base
            │                  ▲
            ├──► store ────────┤ (Redis/PG 客户端，只依赖 net)
            └──► proto ────────┘ (proto 只依赖 base 与 nlohmann/json，不依赖 net/http)
```

| 模块 | 库目标 | 命名空间 | 职责 | 外部依赖 |
| --- | --- | --- | --- | --- |
| base | `gw_base` | `gw` | noncopyable、Timestamp/Clock、日志封装、CurrentThread、字符串小工具 | spdlog |
| net | `gw_net` | `gw::net` | Reactor、TCP、Transport（明文/TLS）、Resolver、Connector | OpenSSL（G08 起）、Threads |
| http | `gw_http` | `gw::http` | HTTP/1.1 请求/响应解析、chunked、SSE、HttpServer、UpstreamClient、UpstreamPool | — |
| store | `gw_store` | `gw::store` | 自研 RESP2 Redis 客户端、libpq 异步 PgClient，均挂在 EventLoop 上 | libpq（pkg-config） |
| proto | `gw_proto` | `gw::proto` | 中立消息/流事件、OpenAI 与 Anthropic 编解码、Translator、token 估算（仅用于限流预留） | nlohmann/json |
| gateway | `gw_gateway` | `gw::gateway` | Config、Auth、Router、ProxySession、UsageMeter、RateLimiter、RetryPolicy、CircuitBreaker、ResponseCache、Metrics、Pricing、AccessLog | OpenSSL::Crypto（SHA-256） |
| app | 可执行 `llm-gateway` | — | 命令行、装配 GatewayContext、信号与优雅退出 | CLI11 |

硬约束：

- `net` 不知道 HTTP；`http` 不知道模型协议；`proto` 不知道网络。只有 `gateway` 把它们装配成代理逻辑。
- `proto` 是纯函数 + 小状态机（流转换器），可以完全在单测里验证。
- 跨模块的公共头只放在 `src/public/<模块>/`，模块私有辅助放同名 `*_detail.hpp`，同样在 `src/public`（遵守 hpp 位置规则），但只被本模块 cpp 引用。

## 2. 线程模型（one loop per thread）

| 线程 | 数量 | 拥有并运行的对象 |
| --- | --- | --- |
| main（base loop） | 1 | `Acceptor`、`TcpServer`、signalfd 通道、每秒一次的全局定时任务（缓存过期清扫、熔断状态、指标 gauge 刷新） |
| io loop | `io_threads`（默认 8） | 下游 `TcpConnection`、`HttpConnState`、`ProxySession`、该会话的上游 `TcpConnection`、本 loop 的 `UpstreamPool`、TLS 会话与 `RedisClient` |
| resolver | 1 | 阻塞 `getaddrinfo` 工作队列、DNS 缓存 |
| ledger loop | 1 | 一个专用 EventLoop：PgClient 写连接（账本批量入账）与读连接（用量查询）、LedgerWriter 队列消费 |
| spdlog async | 1 | 访问日志与运行日志落盘 |

核心不变量：**一个下游请求的所有 I/O 对象都在同一个 io loop 上**。上游连接用下游连接所在的 loop 创建，
所以 `ProxySession` 的全部状态只在一个线程上读写，热路径无锁。跨线程的只有下面这些共享服务：

| 共享服务 | 并发方式 | 调用频率 |
| --- | --- | --- |
| `RateLimiter` | 所有桶在启动时按配置预建，映射表只读；每个桶一把 mutex，准入时 `std::scoped_lock` 同时锁 4 个桶 | 每请求 2 次（准入、结算） |
| `ResponseCache` | 16 路分片 LRU，每片一把 mutex | 每请求 1–2 次 |
| `Metrics` | 所有标签组合在启动时注册；运行期只做 `std::atomic` relaxed 加法 | 每请求若干次 |
| `CircuitBreakerSet` | 每上游一个对象，内部 mutex | 每次上游尝试 |
| `LedgerWriter` | 入队 mutex + condvar，消费在 ledger loop；`healthy()` 为原子标志 | 每请求 1 次 |
| RedisClient | **每个 io loop 一个连接**，不共享（与 UpstreamPool 相同） | 准入 1 次 + 流式每约 32 token 1 次 |
| `Resolver` | 任务队列 mutex + condvar；结果经 `loop->queueInLoop` 回到发起 loop | 缓存未命中时 |
| `AccessLog` | spdlog 异步 logger（自带队列） | 每请求 1 次 |
| `Config` | `std::shared_ptr<const Config>`，启动后只读（不做热加载） | — |

UpstreamPool 是 **每 loop 一个**，不共享：在 `EventLoopThreadPool` 的线程初始化回调里创建，挂在 `EventLoop` 的上下文槽上
（例如 `EventLoop::setContext(std::any)` 或 `thread_local`）。连接不会跨 loop 复用，这是 one-loop-per-thread 的代价，接受它。

## 3. 对象所有权与生命周期

```text
GatewayContext（main 栈上，生命周期覆盖所有 loop）
 ├─ shared_ptr<const Config>
 ├─ RateLimiter / ResponseCache / Metrics / CircuitBreakerSet / Resolver / TlsContext / AccessLog / RecentRequests
 ├─ ledger loop 线程（EventLoopThread）+ LedgerWriter + 两个 PgClient（写 / 读）   ← 仅配置了 postgres 时
 ├─ 每个 io loop 的 context：UpstreamPool、RedisClient（仅配置了 redis 时）
 └─ HttpServer
      └─ TcpServer ──owns──► map<name, shared_ptr<TcpConnection>>（下游）
                                  └─ context: HttpConnState
                                        ├─ HttpRequestParser
                                        └─ shared_ptr<ProxySession>（当前请求，至多一个）
                                              ├─ weak_ptr<TcpConnection>  downstream
                                              ├─ shared_ptr<TcpConnection> upstream（占用中，不在池里）
                                              ├─ TimerId × N（ttft / idle / stall / backoff）
                                              └─ RateLimiter::Ticket、RequestRecord、流转换器
```

规则：

1. 所有注册到 Channel/Timer/Resolver 的回调都捕获 `weak_ptr<ProxySession>`，回调里先 `lock()`，失败即丢弃。
2. `ProxySession` 结束（完成、失败、取消）时必须经过唯一的 `finish(Outcome)`，保证只执行一次；步骤与顺序以
   [proxy-session.md §8](proxy-session.md#8-finishoutcome) 为准。
3. 上游连接归还连接池的条件：响应完整读完（Content-Length 读满或 chunked 读到末块）、状态码非 1xx、响应头没有 `Connection: close`、
   会话没有被取消。其余情况一律 `forceClose()`。
4. `TcpConnection::connectDestroyed` 之后不能再有 Channel 回调；按 muduo 做法用 `Channel::tie(weak_ptr)` 保护 handleEvent 期间的生命周期。
5. HTTP keep-alive：一个下游连接同一时刻只处理一个请求。收到下一个请求时上一个 ProxySession 必须已经 finish；
   pipelining 的后续请求留在输入缓冲里，等上一个 finish 后再解析。

## 4. 优雅退出

1. main loop 通过 signalfd 收到 SIGINT/SIGTERM：停止 Acceptor（关闭监听 fd）。
2. 向每个 io loop 投递"draining"：新请求一律 `503` + `Connection: close`；空闲 keep-alive 连接直接关闭。
3. 等待活跃会话数归零，最长 `shutdown.drain_seconds`（默认 30s）；超时后对剩余会话执行取消（与客户端断开同一路径）。
4. 退出所有 io loop → `LedgerWriter::flushAndStop()`（队列写完 PG）→ 退出 ledger loop → 关闭 resolver 线程 → `spdlog::shutdown()` 刷盘。

## 5. 目录结构

```text
LLMGateway/
├─ CMakeLists.txt            CMakePresets.json        cmake/deps.cmake
├─ AGENTS.md                 README.md                .gitignore
├─ home/gateway.json         （开发配置；密钥只写 env:NAME）
├─ deploy/docker-compose.yml  deploy/.env.example        （PostgreSQL 16 :5433、Valkey 8 :6380）
├─ sql/migrations/001_init.sql
├─ data/                     （容器数据卷，已在 .gitignore）
├─ src/
│  ├─ CMakeLists.txt
│  ├─ public/
│  │  ├─ lib/nlohmann/json.hpp
│  │  ├─ base/     noncopyable.hpp timestamp.hpp log.hpp current_thread.hpp strings.hpp
│  │  ├─ net/      event_loop.hpp channel.hpp poller.hpp timer_queue.hpp timer_id.hpp
│  │  │            event_loop_thread.hpp event_loop_thread_pool.hpp buffer.hpp
│  │  │            inet_address.hpp socket.hpp acceptor.hpp tcp_server.hpp tcp_connection.hpp
│  │  │            transport.hpp tls.hpp resolver.hpp connector.hpp callbacks.hpp
│  │  ├─ http/     http_request.hpp request_parser.hpp response_parser.hpp chunked.hpp
│  │  │            sse.hpp response_writer.hpp http_server.hpp upstream_client.hpp upstream_pool.hpp url.hpp
│  │  ├─ store/    resp.hpp redis_client.hpp pg_client.hpp
│  │  ├─ proto/    neutral.hpp openai.hpp anthropic.hpp translator.hpp token_estimate.hpp errors.hpp
│  │  └─ gateway/  config.hpp context.hpp auth.hpp router.hpp proxy_session.hpp usage_meter.hpp ledger.hpp quota.hpp usage_api.hpp rate_limiter.hpp
│  │               token_bucket.hpp retry_policy.hpp circuit_breaker.hpp response_cache.hpp
│  │               metrics.hpp pricing.hpp access_log.hpp request_record.hpp admin.hpp
│  └─ private/     （与 public 同名的 .cpp；app/main.cpp）
├─ test/
│  ├─ CMakeLists.txt
│  ├─ unit/        buffer_test.cpp timer_queue_test.cpp request_parser_test.cpp response_parser_test.cpp
│  │               chunked_test.cpp sse_test.cpp token_bucket_test.cpp rate_limiter_test.cpp
│  │               lru_cache_test.cpp translator_test.cpp retry_policy_test.cpp circuit_breaker_test.cpp url_test.cpp
│  │               usage_meter_test.cpp resp_test.cpp quota_period_test.cpp usage_row_test.cpp config_test.cpp
│  └─ fault_upstream/ main.cpp
├─ docs/design/    （实现完成后写）
├─ nexttodo/       （本规格）
└─ temp/           （验收材料，已在 .gitignore）
```

`test/fault_upstream/main.cpp` 与单测是 hpp/cpp 位置规则的唯一例外（测试代码自成目录）。

## 6. 构建

### 6.1 根 CMakeLists.txt 要点

```cmake
cmake_minimum_required(VERSION 3.25)   # 本机 3.28
project(LLMGateway VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)  set(CMAKE_CXX_STANDARD_REQUIRED ON)  set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
option(GW_BUILD_TESTS "Build unit tests and fault upstream" ON)
find_package(Threads REQUIRED)
find_package(OpenSSL 3.0 REQUIRED)     # libssl-dev，G00 安装
find_package(PkgConfig REQUIRED)
pkg_check_modules(PQ REQUIRED IMPORTED_TARGET libpq)   # libpq-dev，G00 安装；不用 find_package(PostgreSQL)
include(cmake/deps.cmake)
add_subdirectory(src)
if(GW_BUILD_TESTS) enable_testing() add_subdirectory(test) endif()
```

编译选项统一加 `-Wall -Wextra -Wpedantic -Wshadow -Wconversion`（对第三方目标不加）。

### 6.2 cmake/deps.cmake

照搬 DAgent 的写法（`~/DAgent/cmake/deps.cmake`）：`BUILD_SHARED_LIBS OFF`；spdlog v1.17.0 且 `SPDLOG_USE_STD_FORMAT ON`；
CLI11 v2.7.2；nlohmann 作为 INTERFACE 目标指向 `src/public/lib`；googletest v1.15.2 仅在 `GW_BUILD_TESTS` 时声明，并设置 `INSTALL_GTEST OFF`。

### 6.3 CMakePresets.json

| preset | 生成器 | 目录 | 选项 |
| --- | --- | --- | --- |
| dev | Ninja | build/dev | Debug |
| release | Ninja | build/release | RelWithDebInfo，`-O2` |
| asan | Ninja | build/asan | Debug + `-fsanitize=address,undefined -fno-omit-frame-pointer` |
| tsan | Ninja | build/tsan | Debug + `-fsanitize=thread` |

同时提供同名 buildPresets 和 testPresets（`ctest --preset dev`）。

### 6.4 目标

| 目标 | 类型 | 链接 |
| --- | --- | --- |
| `gw_base` | STATIC | spdlog::spdlog |
| `gw_net` | STATIC | gw_base, Threads::Threads, OpenSSL::SSL |
| `gw_http` | STATIC | gw_net |
| `gw_store` | STATIC | gw_net, PkgConfig::PQ |
| `gw_proto` | STATIC | gw_base, nlohmann_json |
| `gw_gateway` | STATIC | gw_http, gw_store, gw_proto, OpenSSL::Crypto |
| `llm-gateway` | 可执行 | gw_gateway, CLI11::CLI11 |
| `fault-upstream` | 可执行 | gw_http, gw_proto |
| `gateway_unit_tests` | 可执行 + `gtest_discover_tests` | 各库, GTest::gtest_main |
