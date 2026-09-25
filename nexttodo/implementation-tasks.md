# 实施任务与真实验收

先读 [README](README.md) 的执行规则与环境准备。每个任务按"交付 → 单测 → 真实验收 → 完成标准"推进，
验收材料放 `temp/<任务编号>/`，完成后在 §6 追加执行记录。

## 1. 任务图与状态

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

## 2. 通用约定

构建与运行：

```bash
cmake --preset dev && cmake --build --preset dev -j
ctest --preset dev --output-on-failure
./build/dev/llm-gateway -c home/gateway.json          # 可执行文件统一输出到 build/<preset>/
```

发请求用的环境变量（写进 `temp/env.sh`）：

```bash
export GW=http://127.0.0.1:8080
export AUTH='Authorization: Bearer gk-dagent'
export JSON='Content-Type: application/json'
```

需要改配置做验收时，复制一份到 `temp/<任务编号>/gateway.json` 再改，不改 `home/gateway.json` 的默认值。
所有验收都要保存：使用的配置、请求命令、响应（或其摘要）、网关日志片段。

## 3. 逐项任务

### G00：环境与工程骨架

交付：

- `sudo apt install -y libssl-dev libpq-dev`（两者都在 G00 装好，CMake 从一开始就找它们，不设可选开关），可选 `wrk`；
- 根 `CMakeLists.txt`、`CMakePresets.json`（dev/release/asan/tsan，含 build/test presets）、`cmake/deps.cmake`、`src/CMakeLists.txt`、`test/CMakeLists.txt`，内容见 [architecture.md §6](architecture.md#6-构建)；
- 复制 `~/DAgent/src/public/lib/nlohmann/` 到 `src/public/lib/nlohmann/`；
- `AGENTS.md`：写入 [README §3](README.md#3-执行规则) 的执行规则；`.gitignore` 加 `build/`、`temp/`、`logs/`、`data/`、`deploy/.env`；
- `home/gateway.json`：先放 [policies.md §5](policies.md#5-配置-homegatewayjson) 的示例配置。

完成标准：`cmake --preset dev` 成功，空库目标能构建，`ctest --preset dev` 能运行（0 个测试）。

### G01：base 与 Buffer

交付：`base/{noncopyable,timestamp,log,current_thread,strings}`、`net/buffer`。
- `log`：`gw::log::init(level, file)`，其余代码直接用 spdlog 宏；
- `strings`：`toLower`、`trim`、`iequals`、`splitOnce`（HTTP 头解析要用）。

单测 `buffer_test`：append/retrieve、prepend、`ensureWritableBytes` 前移与扩容、`findCRLF` 跨扩容、`readFd` 用 pipe 写入 100KB（超过 64KB 栈缓冲）后一次读全。

完成标准：单测通过，asan preset 下通过。

### G02：Reactor 核心

交付：`net/{event_loop,channel,poller,timer_queue,timer_id,event_loop_thread,event_loop_thread_pool,callbacks}`，语义见 [net-reactor.md §2–4](net-reactor.md#2-eventloop)。

单测 `timer_queue_test`（真实 loop，在单测线程里跑）：
- runAfter 顺序正确；runEvery 触发次数；
- 在回调里 cancel 自己（runEvery 不再触发）、cancel 另一个未到期定时器；
- 另一个线程 `runInLoop` 1 万次，计数正确；`quit()` 跨线程生效；
- EventLoopThreadPool(4) 的 `getNextLoop` round-robin，线程初始化回调在对应线程执行。

完成标准：单测在 dev 与 **tsan** preset 下都通过。

### G03：TCP 服务端连接

交付：`net/{inet_address,socket,acceptor,tcp_server,tcp_connection,transport}`（先只有 PlainTransport），语义见 [net-reactor.md §7–8](net-reactor.md#7-tcpconnection)。

临时验证方式：在 `llm-gateway` 的 main 里先挂一个 echo 回调（G04 替换掉，不另建 demo 目标）。

真实验收：
- `nc 127.0.0.1 8080` 回显；8 个 io 线程下 `for i in $(seq 200); do echo hi | nc -q1 127.0.0.1 8080 & done` 全部回显，日志中连接分布到不同 loop；
- 水位：把 high water 设为 64KB，用 `nc` 发送但不读取的客户端（`yes | nc 127.0.0.1 8080 > /dev/null` 配合 `kill -STOP` 暂停 nc）触发 high water 日志；`kill -CONT` 后出现 write complete 日志；
- `ulimit -n 64` 后开 100 个连接，EMFILE 处理生效且 CPU 不空转（`top` 截图）。

完成标准：以上三项有日志证据；asan 下无报告。

### G04：HTTP 服务端 + 配置 + main

交付：`http/{http_request,request_parser,response_writer,http_server,url}`、`gateway/{config,context}`、`app/main.cpp`（CLI11：`-c/--config`，`--io-threads` 覆盖配置）、signalfd 优雅退出（[architecture.md §4](architecture.md#4-优雅退出)），路由先实现 `/healthz` 与 404/405。

单测：
- `request_parser_test`：完整请求、逐字节喂入等价、pipelining 两个请求连在一起、431/413/411/400 各一例、`Expect: 100-continue`、HTTP/1.0 keep-alive 判断；
- `url_test`：[http.md §9](http.md#9-url) 的例子 + IPv6。
- 配置加载：`env:` 替换、引用不存在的上游报错（可在 `config_test` 里用临时文件）。

真实验收：
- `curl -v $GW/healthz` 200；`curl -v $GW/nope` 404；`curl -v --http1.1 $GW/healthz $GW/healthz` 看到 "Re-using existing connection"；
- `wrk -t4 -c256 -d15s $GW/healthz` 记录 QPS 与延迟（release preset），作为 G15 的基线；
- 运行中 `kill -TERM`，进程在 drain 后退出，日志完整。

### G05：上游客户端

交付：`net/{resolver,connector}`、`http/{response_parser,chunked,upstream_client,upstream_pool}`，语义见 [net-reactor.md §9–11](net-reactor.md#9-resolver)、[http.md §6–7](http.md#6-上游客户端upstreamclient)。

单测：
- `response_parser_test`：Content-Length / chunked / 连接关闭定界三种；1xx 跳过；逐字节等价；错误体截断；
- `chunked_test`：带扩展的 size 行、跨块切分、非法十六进制、trailer。

真实验收（此时还没有代理逻辑，在 main 里加一个临时的 `--probe URL` 命令行选项，发一次 GET 并打印状态与体长度；G06 完成后删除）：
- `--probe http://127.0.0.1:10010/health`、`/v1/models` 返回 200；
- `--probe http://localhost:10010/health` 走 Resolver 线程（日志显示解析耗时，第二次命中缓存）；
- 连接被拒：`--probe http://127.0.0.1:1/` 报 refused；连接超时：`--probe http://10.255.255.1/` 在 connect 超时后报 timeout；
- 连续两次 probe 同一上游，第二次 `reused=true`；等待超过 `pool_idle_seconds` 后再次 probe，`reused=false`。

### G06：OpenAI 透传代理

交付：`gateway/{auth,router,proxy_session,usage_meter}`（本任务只实现 Acquiring → AwaitHead → AwaitFirstEvent → Streaming → Done 主路径，无重试）、
`http/sse`、`proto/{neutral,openai,token_estimate}` 中透传需要的部分（OpenAI 解码器、[protocol-translation.md §6](protocol-translation.md#6-透传时的最小改写) 的改写）、`/v1/models`。
计量按 [policies.md §4](policies.md#4-token-计量) 实现：`metering: llamacpp` 的注入与删除、UsageMeter。
准入（[proxy-session.md §1](proxy-session.md#1-输入与准入)）本任务只实现第 1、2、3、7 步；其余步骤由后续任务加入：
4 限流 → G10，3a/3b 账本与配额 → G11，6 熔断与尝试计划 → G12，5 缓存 → G13。未实现的步骤直接跳过，不写占位逻辑。

单测：
- `sse_test`（与 DAgent SseParser 同样的用例：CR/LF/CRLF、BOM、注释、多行 data、逐字节等价）；OpenAI 解码器对录制样本的解码（见 G14 的样本录制方法，这里先录 OpenAI 的）；
- `usage_meter_test`：用带 `timings_per_token` 与 `return_progress` 录制的真实 Qwen3.6 流，逐事件喂入，检查每一步的累计值与样本里的字段一致，结束时等于末块 `usage`；
  截断在任意事件处时，meter 的值等于截断前最后一个 `timings`（或 `prompt_progress`）。样本录制：

  ```bash
  curl -sN http://127.0.0.1:10010/v1/chat/completions -H "$JSON" -d '{"model":"Qwen3.6-35B-A3B","stream":true,
    "stream_options":{"include_usage":true},"timings_per_token":true,"return_progress":true,
    "messages":[{"role":"user","content":"用三句话介绍 epoll"}]}' > test/unit/data/openai_metering.sse
  ```

真实验收（Qwen3.6 运行中）：

```bash
# 流式
curl -sN $GW/v1/chat/completions -H "$AUTH" -H "$JSON" \
  -d '{"model":"qwen","stream":true,"messages":[{"role":"user","content":"用三句话介绍 epoll"}]}'
# 非流式
curl -s $GW/v1/chat/completions -H "$AUTH" -H "$JSON" \
  -d '{"model":"qwen","messages":[{"role":"user","content":"1+1=?"}],"max_tokens":256}'
# 工具调用（流式）：带一个 get_weather 工具，问"北京天气"
```

- 与直连 `http://127.0.0.1:10010/v1/chat/completions`（model 改为 `Qwen3.6-35B-A3B`）的输出结构一致：role 块、reasoning_content、content、tool_calls 分片、finish_reason、`[DONE]`；
- 客户端没传 `stream_options` 时收不到 usage 块；传了 `include_usage:true` 时收到；
- 下游输出里没有 `timings`、`prompt_progress`（`grep -c`为 0）；客户端自己传 `timings_per_token:true` 时有；
- **计量精确**：客户端传 `include_usage:true`，网关日志（G09 之前先用 debug 日志）记下的 prompt/cached/completion 与客户端收到的末块 `usage` 逐一相等；
  非流式请求与响应体里的 `usage` 相等；
- 错误 key → 401；未知 model → 404；
- 上游 400 原样透传：发一个超过单槽 131072 token 的 prompt（例如用 `python3 -c` 生成约 600KB 的文本），
  llama.cpp 返回上下文超长的 400，网关不重试、状态码与 message 原样到达（DAgent 靠这段文字触发压缩）；
- 同一 curl 命令连续两个请求复用下游连接；网关日志显示第二次上游连接 `reused=true`。

### G07：背压、取消、超时

交付：[proxy-session.md §3–5](proxy-session.md#3-超时) 的全部内容。

真实验收（配置复制到 `temp/G07/gateway.json`，`high_water_bytes` 改为 4096，`stall_ms` 改为 10000）：

1. **背压**：请求长输出（"从 1 数到 3000，每个数字一行"，`max_tokens: 8000`），`curl -sN --limit-rate 300 … > /dev/null`。
   证据：网关日志的 pause/resume；暂停期间 `ss -tni '( dport = :10010 )'` 显示上游 socket 的 Recv-Q 堆积；网关 RSS（`ps -o rss= -p <pid>` 每秒采样）保持平稳。
2. **stall**：`curl -sN … | sleep 60`（管道不读，curl 被阻塞后不再读 socket）→ 约 10 秒后网关日志 `downstream_stalled`，上游连接被关闭，`/slots` 显示槽位释放。
3. **流式中取消**：`timeout 5 curl -sN …`（长输出请求）→ curl 退出后立即 `curl -s http://127.0.0.1:10010/slots | jq '.[] | {id, is_processing}'` 全部 false；
   网关日志 `cancelled`、stage=streaming、`cancel_latency_us`；llama-server 日志显示任务被取消（如果它的输出有记录）。
   **取消时的计量**：客户端请求里自己带 `timings_per_token:true`，这样 curl 保存的输出里有 timings。网关记录的 `completion_tokens`
   应当 ≥ curl 收到的最后一个 `predicted_n`，且只多出排空阶段读到的那几个；`usage_source = upstream_partial`。
4. **预填充中取消**：约 60k token 的 prompt，`timeout 2 curl …` → `completion_tokens = 0`，`prompt_computed_tokens` 等于最后一次 `prompt_progress.processed`，并且小于 prompt 总数。
5. **建连中取消**：临时上游 `base_url: http://10.255.255.1/v1`、`connect_ms: 10000`，`timeout 1 curl …` → 日志 stage=acquiring，Connector 被取消，usage 全为 0（`rejected`）。
6. **ttft 超时**：`ttft_ms` 设为 50 → 请求失败返回 504（本任务还没有重试）。
7. **进度保活**：`ttft_ms` 设为 5000，发约 60k token 的 prompt（预填充超过 5 秒）→ 因为有进度事件而正常完成，没有超时。

完成标准：7 项证据齐全；asan 下重复 1、3、5 无报告。

#### G07 返工（2026-09-25 审查）

首次提交的 G07 被审查退回。下面每一项都是规格已锁定、但实现不符的地方，修完后按"返工验收"重新验收，状态才能再改为"真实验收通过"。
第 1–15 项已于 2026-09-25 全部修复并验收（第 6 项的真实复现按计划放到 G12），结果见执行记录"G07 返工"。

| # | 问题 | 现状（代码位置） | 要求（规格位置） |
| --- | --- | --- | --- |
| 1 | 同一轮 epoll 结果里释放后使用 | `TcpConnection::handleClose` 同步 remove channel，上游连接的最后一个引用随会话析构立即释放；`EventLoop::loop` 仍会对本轮后面的裸 `Channel*` 调 `handleEvent` | 连接析构推迟到本轮事件处理完（[net-reactor.md §7](net-reactor.md#7-tcpconnection)） |
| 2 | stall 被记成 `cancelled` | stall 回调先 `forceClose` 下游，同步触发 close 回调 → `cancel()` 先 finish；`temp/G07/gateway.log` 里 3 次 stall 的记录全是 `outcome=cancelled` | 先 finish(`downstream_stalled`) 再关下游（[proxy-session.md §4.2](proxy-session.md#42-背压规则)） |
| 3 | 背压暂停期间 idle 定时器照走 | `onDownstreamHighWater`/`onDownstreamWriteComplete` 不碰 idle 定时器；默认 `stall_ms = idle_ms = 60000` 时 idle 先到期，走 `failCommitted`，outcome 错成 `stream_interrupted`，上游不排空，Recv-Q 里的 token 不计量 | 暂停时停 idle，恢复时重新计时（[proxy-session.md §3](proxy-session.md#3-超时)、§4.2） |
| 4 | 优雅退出的轮询只跑一次 | `main.cpp` 的 drain checker 只由局部 `shared_ptr` 持有，signal 回调返回即析构，`weak_checker.lock()` 失败不再重排；有活跃会话时总是等满 `drain_seconds` | 活跃会话归零后约 50ms 内退出；`drain_seconds` 到期时对剩余会话执行 `cancel`（与客户端断开同一路径）再退出，现在是直接 quit（[architecture.md §4](architecture.md#4-优雅退出)） |
| 5 | 超时结束尝试没有排空 | ttft / idle / precommit / 提交后中断经 `failAttempt`/`failCommitted` → `finish` → `forceClose` | 统一用 `abortAndDrain()` 结束当前尝试（[proxy-session.md §5.2](proxy-session.md#52-各状态下的动作) 末段） |
| 6 | 复用连接的竞态没处理 | `Lease.reused` 只被设置，没人用 | `reused` 连接在收到任何响应字节前失败 → 新建连接重发一次，不计尝试、不计熔断（[net-reactor.md §11](net-reactor.md#11-upstreampoolgw_http每-loop-一个)） |
| 7 | `/v1/messages` 走进 OpenAI 路径 | Router 把 Anthropic 请求体按 OpenAI 解析并转发到 `/chat/completions` | G14 之前按未知路径返回 404（Anthropic 格式错误体） |
| 8 | 删除 TLS | `net/tls`、`TlsContext`、Transport 的 TLS 钩子、`kHandshaking`、连接池 https 分支、配置 `tls` 段仍在；连接池里握手失败时 acquire 永不回调且连接自持引用泄漏 | 按 [net-reactor.md §6](net-reactor.md#6-传输层) 删除；`https://` 的 `base_url` 配置加载报错（[http.md §9](http.md#9-url)）；连接池保证未取消的 acquire 恰好回调一次 |
| 9 | 下游 `SO_SNDBUF` 写死在 TcpServer | `tcp_server.cpp` 对所有连接设 64KB，net 层带了网关策略 | 改为 `HttpServerOptions` 选项，由配置 `stream.downstream_sndbuf_bytes`（默认 65536，0 表示不设置）传入（[policies.md §5](policies.md#5-配置-homegatewayjson)） |
| 10 | 执行记录前后矛盾 | G07 记录"偏离"写 `ttft_ms=1000`、第 7 项写 2000；"单测现有 54 例"与 71 例不符；asan 结果 `result_asan.txt`（23:41:04）早于最后一次代码修改（23:41:34） | 返工完成后重写 G07 记录，所有数字来自同一轮最终代码的运行 |
| 11 | 可执行文件位置不符 | 产物在 `build/<preset>/src/`，验收脚本也按这个路径写死 | 按 §2 统一输出到 `build/<preset>/`（设 `CMAKE_RUNTIME_OUTPUT_DIRECTORY`），脚本随之改 |
| 12 | 连接 fd 从不关闭（返工中发现） | `PlainTransport` 与 `TcpConnection` 都不 `close(fd)`，`forceClose` 只是移出 epoll；每个连接泄漏一个 fd，下游停在 CLOSE-WAIT，上游只靠 `SHUT_WR` 才让对端看到断开 | Transport 持有 fd、析构时关闭（[net-reactor.md §6](net-reactor.md#6-传输层)） |
| 13 | 排空逐事件解析 JSON 卡住 loop（返工中发现） | 取消时同步排空最多 1MB，每个 SSE 事件都解析 JSON；asan 压测下单次排空数十秒，同 loop 其他流的上游缓冲随之积满，形成连锁，loop 数分钟不处理新连接 | 只扫描分帧、取最后一个 JSON 事件计量（[policies.md §4.5](policies.md#45-取消时拿到最后的精确计数)） |
| 14 | 退出时 handler 已析构仍被调用（返工中发现） | `~HttpServer` 先销毁各 handler 成员、后销毁 TcpServer；`main` 里 Router 先于 HttpServer 析构，而 TcpServer 析构时 io loop 仍会对剩余连接回调 `onConnection` → Router | 先停 io 线程再析构 handler 所引用的对象（`HttpServer::stop()`） |
| 15 | 写下游失败时同步关闭导致重入（返工中发现） | `sendInLoop` 写失败时同步 `handleClose`，调用方正处在上游 SSE 解析回调里，close 回调 → `cancel()` → 排空再次喂同一个 SSE 解析器 | 写失败只标记并 `queueInLoop(handleClose)`，之后的发送直接丢弃 |

返工验收（配置仍放 `temp/G07/`，证据按项存档；原 7 项全部在最终代码上重跑）：

- **stall**：以 finish 记录（`proxy record`）里的 `outcome=downstream_stalled` 为准，不以 WARN 日志为准；
- **默认超时下的慢读者**：只把 `high_water_bytes` 改为 4096，`stall_ms`/`idle_ms` 保持 60000，客户端完全不读 → 约 60s 后 `outcome=downstream_stalled`，不是 `stream_interrupted`；
- **暂停不触发 idle**：`idle_ms: 5000`、`stall_ms: 30000`，长输出流式请求，`kill -STOP` curl 10 秒再 `kill -CONT` → 流正常读完、有 `[DONE]`，`outcome=ok`；
- **背压**：Recv-Q 堆积由验收脚本在暂停期间自动采样（不能为空），报告中的数值与证据文件一致；
- **预填充中取消**：网关 debug 日志打出每次 `prompt_progress.processed`，最后一次的值与记录里的 `prompt_computed_tokens` 相等；
- **优雅退出**：一个流式请求进行中 `kill -TERM` → 流正常结束（有 `[DONE]`），进程在流结束后 1 秒内退出（`drain_seconds` 保持 30），日志给出两个时间点；
- **UAF 压力**：asan 构建下，32 个并发长输出流式请求，每个在 1–3 秒内随机时刻被 `timeout` 杀掉，重复 10 轮 → sanitizer 报告 0，`/slots` 全部空闲；
- **路由**：`POST /v1/messages` → 404；**配置**：临时上游写 `https://…` → 启动失败，错误信息指出上游名；
- **复用重发**（第 6 项）：真实模型难以稳定复现，验收放到 G12，用 fault_upstream 的 `close_before_head` 让复用连接在首字节前断开，
  fault 日志里能看到两条连接、网关记录 `attempts=1`；
- asan 下重跑场景 1、3、5 与 stall；单测 dev / asan / tsan 全部通过。

### G08：已移出范围

项目只接本地明文上游，不实现 TLS，编号不复用。已有的 TLS 代码在 G07 返工第 8 项里删除。
原 G08 里与 TLS 无关的 `final` 计量取消验收挪到 G09。

### G09：可观测性

交付：`gateway/{request_record,metrics,pricing,access_log,admin}`，内容见 [policies.md §3](policies.md#3-可观测性)。

真实验收：
- 一次流式请求后，`logs/access.log` 最后一行 JSON 字段齐全；`ttft_ms` 与 `curl -w '%{time_starttransfer}'` 相差在几毫秒内；
- `curl $GW/metrics` 各指标存在，计数随请求增长；用 `promtool check metrics`（如已安装）或肉眼核对格式；
- 按 pricing 手算一次请求的成本，与记录一致；
- `/admin/requests?limit=5`、`/admin/stats` 返回合理数据；取消的请求 `usage_source = upstream_partial` 且 `attempts[]` 完整；
- `final` 计量的取消：模型 `qwen-final`（上游 `qwen36-final`，见 [policies.md §5](policies.md#5-配置-homegatewayjson)）流式请求中途 `timeout 5 curl` →
  `completion_tokens = null`、`usage_complete = false`、`usage_source = unreported`、`gateway_usage_unreported_total` 加一；槽位立即空闲；
- 记录与指标里任何地方都不出现 `token_estimate` 算出的数：把网关日志里准入时的预留量和最终记录对比，两者必须来源不同、数值无关；

### G10：限流

交付：`gateway/{token_bucket,rate_limiter}`，内容见 [policies.md §1](policies.md#1-限流)。

单测（注入假时钟）：
- `token_bucket_test`：补充速率、容量上限、大于容量的请求要求桶满、负债与恢复、`waitFor` 计算；
- `rate_limiter_test`：4 个桶任一不足即整体拒绝且不扣任何桶；结算多退少补；并发 16 线程 × 1000 次 acquire 总扣减数正确（tsan）。

真实验收（`temp/G10/gateway.json`：`gk-dagent` 的 rpm=5；模型 `qwen` 的 tpm=3000）：
- `seq 10 | xargs -P10 -I{} curl -s -o /dev/null -w '%{http_code}\n' …` → 5 个 200、5 个 429，429 带 Retry-After 与 scope 头；
- token 维度：`max_tokens: 2500` 的请求第一个通过、紧接着第二个 429（dimension=tokens）；第一个实际只用了几百 token，结算后等待更短时间即可通过；
- 取消的请求按上游报告的部分用量结算，其余预留退还（`/admin/stats` 或指标对比，差值等于记录里的真实用量）。

### G11：用量账本与配额

交付：内容见 [usage-ledger.md](usage-ledger.md)。
- `deploy/docker-compose.yml`、`deploy/.env.example`、`sql/migrations/001_init.sql`；
- `store/{resp,redis_client,pg_client}`（新库 `gw_store`）；ledger loop 线程；
- `gateway/{ledger,quota,usage_api}`；ProxySession 的准入配额检查、流式上报与截断、finish 时入账（[proxy-session.md](proxy-session.md) 中标注的步骤）。

准备（libpq-dev 已在 G00 安装）：`cp deploy/.env.example deploy/.env` 并填密码；`docker compose -f deploy/docker-compose.yml up -d`；
`export GW_PG_DSN="host=127.0.0.1 port=5433 dbname=gateway user=gateway password=…"`。

单测（不连数据库）：
- `resp_test`：简单字符串、错误、整数、空 bulk、嵌套数组、多个回复连在一起（流水线）、逐字节喂入等价；
- `quota_period_test`：按 UTC 偏移计算日/月键名、跨零点与跨月边界、到下个周期的 Retry-After 秒数；
- `usage_row_test`：UsageRow → 语句参数（NULL 字段、成本整数化、`billable_tokens` 计算、attempts JSON）。

真实验收（PG/Valkey 容器 + Qwen3.6；SQL 用 `psql "$GW_PG_DSN"` 执行）：

| # | 场景 | 证据 |
| --- | --- | --- |
| 1 | 启动两个网关实例（8080/8081，`instance_id` 不同）同时迁移 | `schema_migrations` 只有一行；两个实例都正常服务 |
| 2 | 发 20 个请求（流式、非流式、取消各几个） | `usage_records` 20 行，每行 token 数与访问日志逐一相等；取消的行 `usage_complete` 与 `attempts` 正确 |
| 3 | 汇总一致性 | 下面的核对 SQL 返回 0 行 |
| 4 | 客户端重复使用同一个 `x-request-id` 发两次 | 两行，`ledger_id` 不同 |
| 5 | `/v1/usage`、`/v1/usage/requests` 翻页 | 与 SQL 查询结果一致；`quota` 与 `valkey-cli GET` 一致 |
| 6 | 准入超额：临时配置 `daily_tokens` 小于已用量 | 429 `insufficient_quota`，Retry-After 为到本地零点的秒数 |
| 7 | 流式截断：`daily_tokens` = 已用 + 500，请求长输出 | 流以 `finish_reason:"length"` + `[DONE]` 正常结束；账本 `truncated=true`；单个流的超额量 ≤ 触发截断的那个 chunk 加排空阶段读到的量（用 PG 的 billable 与限额计算）；下一个请求 429 |
| 8 | 超大 prompt：剩余 10k，发 60k token 的 prompt | 首个 `prompt_progress` 报出总数时就判定超额，客户端直接收到 429，没有任何流内容；账本 `billable_tokens` 只含已处理的部分（通常接近 0），`input_tokens` 为 prompt 总数 |
| 9 | 多实例共享额度：两个实例上对同一个 key 并发 8 个流，限额设在中途 | 总超额在 §5.4 的上限内；全部结束后 `/admin/quota/reconcile` 差值为 0 |
| 10 | PG 故障：`docker compose stop postgres` | 新请求 503；恢复后积压写入，停机前已完成的请求在 PG 里一条不少（与访问日志行数对比） |
| 11 | Redis 故障：`docker compose stop valkey` | `on_redis_failure: reject` 时 503；改为 `allow` 时放行并计数 |
| 12 | Redis 重建：`valkey-cli FLUSHALL` 后重启网关 | 当日/当月计数从 PG 恢复，reconcile 差值为 0 |
| 13 | 压力下 SIGTERM | 退出前队列写完，PG 行数等于访问日志行数 |

```sql
-- 汇总与明细必须一致（应返回 0 行）
SELECT d.key_name, d.day, d.model, d.billable_tokens, s.billable
FROM usage_daily d
FULL JOIN (SELECT key_name, billing_day AS day, model, sum(billable_tokens) AS billable, count(*) AS n
           FROM usage_records GROUP BY 1, 2, 3) s USING (key_name, day, model)
WHERE d.billable_tokens IS DISTINCT FROM s.billable OR d.requests IS DISTINCT FROM s.n;
```

### G12：故障注入上游、重试、降级、熔断

交付：`test/fault_upstream/`（§4）、`gateway/{retry_policy,circuit_breaker}`，ProxySession 的 Backoff 状态，内容见 [proxy-session.md §6](proxy-session.md#6-重试降级与熔断)。

单测：`retry_policy_test`（退避上下界与抖动范围、Retry-After 秒数与 HTTP-date 解析、失败分类表逐项）；`circuit_breaker_test`（假时钟：阈值打开、冷却后半开只放行一个、探测成功关闭、失败冷却翻倍且有上限）。

真实验收：在 `temp/G12/gateway.json` 里为每个场景配置一个 fault 上游（故障模式写在 base_url 路径里，见 §4），模型 `fault-<场景>` 的目标链为 `[fault-<场景>, qwen36]`，
重试参数用默认值（`max_attempts: 2`），除非表里另有说明：

| # | fault 路径指令 | 期望 |
| --- | --- | --- |
| 1 | `status=503,times=1` | 200，attempts=2，upstream=fault |
| 2 | `status=429,retry_after=1,times=1` | 退避 ≥1s 后成功，attempts=2 |
| 3 | `status=429,retry_after=60` | 超过 `max_retry_after_ms`，立即降级，upstream=qwen36 |
| 4 | `status=500` | fault 上 2 次后降级，attempts=3，upstream=qwen36 |
| 5 | `status=400` | 400 透传，不重试 |
| 6 | `drop_after=0` | 200 头后首事件前断开 → 透明重试成功 |
| 7 | `drop_after=10` | 客户端收到 10 个事件 + 流内错误，没有 `[DONE]`，outcome=stream_interrupted |
| 8 | `hang_after=5`（idle_ms=3000） | 3 秒后流内错误 |
| 9 | `ttfb_ms=5000`（ttft_ms=2000） | 超时重试后降级到 qwen36 |
| 10 | 熔断：`status=500`，连续发 6 个请求 | 日志熔断打开；之后请求 attempts=1 直接走 qwen36；冷却后出现一次半开探测 |
| 11 | 取消：`rate=5,tokens=1000`，`timeout 3 curl …` | fault 日志的"peer closed"时刻与 curl 被杀时刻相差毫秒级；记录的 `completion_tokens` **等于** fault 日志里的"已发 token 数" |

计量核对（fault 的 token 数是确定的，可以做精确相等比较）：
- 场景 1、2、4、5：被拒绝的尝试在 `attempts[]` 里全部为 0、`source = rejected`；
- 场景 7：流中断的那次尝试 `completion = 10`，与 fault 日志一致；
- 场景 9：超时的那次尝试如果 fault 还没发出任何字节，就记 0；
- 请求级总量 = 各尝试之和；`gateway_wasted_tokens_total` 等于失败尝试的消耗之和。

真实模型场景：
- 模型 `qwen` 的主目标 qwen36 不可达且 nemotron 运行 → 请求降级到 nemotron；前两个目标不可达时降级到 gemma4。验收可在临时网关配置中将目标地址指向未监听的本机端口，不停止在用模型；若必须停服务，先确认没有其他使用者，再用 `~/LLM/llm.sh stop qwen36`。
- `~/LLM/llm.sh start qwen36` 启动后立即请求 → 加载期间收到 503 Loading model 被重试（日志中能看到 503 与退避），加载完成后成功或降级。

### G13：响应缓存

交付：`gateway/response_cache`，内容见 [policies.md §2](policies.md#2-响应缓存)。

单测 `lru_cache_test`：命中/未命中、LRU 淘汰顺序、字节预算、过期惰性删除、清扫、单条超限不写入、键规范化（字段顺序不同 → 同键；`user` 不同 → 同键；stream 不同 → 不同键）。

真实验收：
- `temperature: 0` 的同一请求发两次：第一次 `x-gateway-cache: MISS`，第二次 `HIT`，记录两次 latency 与 TTFT；流式与非流式各做一次；
- 不带 temperature 的请求不缓存（无 `x-gateway-cache` 头）；`x-gateway-cache: bypass` 绕过；
- `ttl_seconds: 5` → 6 秒后 MISS；`max_bytes` 调小到 64KB 后连续不同请求触发淘汰（指标可见）；
- `/metrics` 中命中率与 `gateway_cache_saved_cost_usd_total` 增长。

### G14：Anthropic 入口、上游与协议转换

交付：`proto/{anthropic,translator}`，`/v1/messages` 与 `/v1/messages/count_tokens` 路由，内容见 [protocol-translation.md](protocol-translation.md)。

样本录制（真实数据作为单测输入，放 `test/unit/data/`）：

```bash
# OpenAI 流（含 reasoning 与并行工具调用）
curl -sN http://127.0.0.1:10010/v1/chat/completions -H "$JSON" -d @temp/G14/openai_tools.json > test/unit/data/openai_tools.sse
# Anthropic 流
curl -sN http://127.0.0.1:10010/v1/messages -H "$JSON" -H 'anthropic-version: 2023-06-01' -d @temp/G14/anthropic_tools.json > test/unit/data/anthropic_tools.sse
```

单测 `translator_test`：
- 请求映射：system 提取、并行工具结果合并为一条 user 消息、tool_choice 四种、max_tokens 缺省、图片 → 错误；反方向同样；
- 流：两个录制样本分别经"解码 → 编码为另一种协议"，检查事件序列（块开关顺序、工具参数拼接后是合法 JSON、usage 累计值、finish 映射），再经对方解码器解回来，与直接解码的中立事件序列一致（往返等价）；
- 非流式响应互转；错误体互转。

真实验收：
- OpenAI 入口 → 模型 `qwen-anth`（Anthropic 上游）：普通对话、流式工具调用；
- Anthropic 入口 → 模型 `qwen`（OpenAI 上游）：同上；
- Anthropic 入口 → `qwen-anth`：透传；
- 若本机有 Python `openai` / `anthropic` SDK，用它们的流式接口读取网关输出，作为严格的格式校验（脚本放 `temp/G14/`）；
- Anthropic 入口 → `qwen`：`message_start` 与 `message_delta` 里的 `input_tokens + cache_read_input_tokens` 都等于网关记录的 `prompt_tokens`（上游报告的精确值，不是 0 也不是估算）；
- `/v1/messages/count_tokens` 经网关返回的值与直连 `http://127.0.0.1:10010/v1/messages/count_tokens` 相同。

### G15：DAgent 对接、压测、文档

**DAgent 对接**（仅改配置，改前备份 `~/DAgent/home/models.json` 并先征得确认）：在 `models` 中增加

```json
"gw-qwen":   { "kind": "openai-chat", "base_url": "http://127.0.0.1:8080/v1", "api_key": "gk-dagent", "model": "qwen",
               "context_window": 131072, "max_tokens": 8192, "include_usage": true },
"gw-claude": { "kind": "anthropic",   "base_url": "http://127.0.0.1:8080/v1", "api_key": "gk-dagent", "model": "qwen",
               "context_window": 131072, "max_tokens": 8192 }
```

`context_window` 不得超过 Qwen3.6 单槽 131072。场景（每个保存 DAgent 截图 + 网关访问日志片段）：

| # | 场景 | 证据 |
| --- | --- | --- |
| 1 | `gw-qwen` 多轮对话 + 读写文件工具调用 | 访问日志含 TTFT/token/成本；DAgent 显示 usage |
| 2 | 生成中按 Esc | 网关 `cancelled`，`/slots` 立即空闲 |
| 3 | `gk-dagent` tpm 调低到 20000 | DAgent 显示重试后成功；网关出现 429 记录 |
| 4 | 临时配置令 qwen36 不可达（实际停服务须确认无他人使用） | 下一轮 DAgent 无感继续，upstream=nemotron；前两目标不可达时 upstream=gemma4；熔断打开后跳过失败目标 |
| 5 | `gw-claude` 对话 + 工具调用 | Anthropic 入口 → OpenAI 上游转换全路径可用 |
| 6 | 一段 DAgent 会话结束后查 `curl -H 'Authorization: Bearer gk-dagent' $GW/v1/usage` | 本次会话的输入/输出/缓存命中 token 与 PG 明细一致 |
| 7 | 把 `gk-dagent` 的 `daily_tokens` 设在会话中途能用完的位置 | 当前回复在额度处以 finish=length 结束（记录 DAgent 实际如何呈现）；下一轮 DAgent 收到 429 `insufficient_quota`，因为 Retry-After 超过 300 秒而直接报错 |

**压测**（release preset，结果写进 `docs/design/benchmark.md`）：
- `wrk -t4 -c256 -d30s $GW/healthz`：QPS、p99；
- 缓存命中：wrk + Lua 脚本 POST 固定 `temperature:0` 请求：QPS、p99；
- 网关额外延迟：同一请求直连 Qwen3.6 与经网关各 20 次，比较 `time_starttransfer` 的中位数；
- 1000 个并发慢速流：fault 上游 `rate=2,tokens=100`，`seq 1000 | xargs -P1000 …` 发起，采样网关 RSS、CPU、`gateway_active_streams`；
- 慢读者：100 个 `--limit-rate 200` 客户端，确认 RSS 上界约等于 100 × high_water。

**文档**：`docs/design/{net,http,proxy,protocol,ratelimit,cache,observability,benchmark}.md`（写实现要点与实测数据），根目录 README（架构图、能力表、快速开始、DAgent 接入方法）。

## 4. 故障注入上游 fault_upstream

基于本项目的 Reactor 与 HTTP 服务端实现（同时也是对它们的又一次真实使用）。

```bash
./build/dev/fault-upstream --listen 127.0.0.1:19000 --threads 2
```

故障模式写在**路径前缀**里，网关侧只需要把上游的 base_url 配成对应前缀，不需要转发任何特殊请求头：

```text
POST /f/<指令,指令,...>/v1/chat/completions      → 输出 OpenAI 格式
POST /f/<指令,指令,...>/v1/messages              → 输出 Anthropic 格式
POST /v1/chat/completions                        → 无故障，正常输出
```

| 指令 | 含义 |
| --- | --- |
| `status=N` | 直接返回状态码 N，错误体按对应协议格式 |
| `times=K` | 同一个 `x-request-id` 的前 K 次请求故障，之后正常（默认一直故障）；状态按 request id 保存 60 秒 |
| `retry_after=S` | 附带 `Retry-After: S` |
| `ttfb_ms=M` | 延迟 M 毫秒再发响应头 |
| `close_before_head` | 读完请求后直接关闭连接 |
| `tokens=N` | 输出 N 个文本增量（默认 50，内容 `tok0 tok1 …`） |
| `rate=R` | 每秒 R 个事件（默认不限速） |
| `drop_after=K` | 发出 K 个事件后直接关闭连接（不发结束标记） |
| `hang_after=K` | 发出 K 个事件后停止发送但保持连接 |
| `error_after=K` | 发出 K 个事件后发送流内 error 事件并结束 |

- 自带确定性"分词"：prompt token = 所有消息文本按空白切分的词数，每个文本增量 = 1 个 token；
  支持 `timings_per_token` 与 `return_progress`，字段格式与 llama.cpp 相同，所以网关按 `metering: llamacpp` 对接它；
- 非流式请求返回完整 JSON 与按上述规则计算的 `usage`；
- 日志里记录每个请求实际发出的 token 数，作为网关计量的对照；
- 日志：每个连接的建立/关闭时刻（`CLOCK_REALTIME` 毫秒）、request id、已发事件数、"peer closed while streaming" 标记——这是取消传播延迟的对照依据；
- 响应头 `x-fault-served: <指令>` 方便在网关日志里核对。

## 5. 单测清单汇总

| 文件 | 任务 | 覆盖 |
| --- | --- | --- |
| `buffer_test` | G01 | Buffer 各操作、readFd |
| `timer_queue_test` | G02 | 定时器、跨线程任务、线程池 |
| `request_parser_test`、`url_test`、`config_test` | G04 | |
| `response_parser_test`、`chunked_test` | G05 | |
| `sse_test`、`usage_meter_test` | G06 | usage_meter 基于录制的真实流 |
| `token_bucket_test`、`rate_limiter_test` | G10 | |
| `resp_test`、`quota_period_test`、`usage_row_test` | G11 | 不连数据库；数据库行为只做真实验收 |
| `retry_policy_test`、`circuit_breaker_test` | G12 | |
| `lru_cache_test` | G13 | |
| `translator_test` | G14 | 基于录制的真实样本 |

所有解析器类单测都必须包含"逐字节喂入与整段喂入结果一致"的用例。

## 6. 执行记录


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
