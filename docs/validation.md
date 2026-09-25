# 构建与验证

所有命令在仓库根目录执行。执行规则见 [AGENTS.md](../AGENTS.md)，已完成验收和历史证据见 [验收记录](acceptance/history.md)。
构建通过仅证明构建成功；功能结论需要真实模型、数据库或客户端运行证据。

## 构建与纯逻辑单测

```bash
cmake --preset dev
cmake --build --preset dev -j
ctest --preset dev --output-on-failure
```

提供 dev/release/asan/tsan 预设。单测位于 `test/unit/`，覆盖解析器、令牌桶、LRU、协议转换和计量等纯逻辑组件，不在单测中启动模型。
`test/unit/data/` 保存真实协议录制样本；解析器需覆盖逐字节输入与整段输入的等价性。
144 项通过是 2026-09-25 的验收结果，后续以实际运行输出为准。

## 真实功能验证

临时配置、请求、脚本、日志、响应和截图放在 `temp/<任务编号或变更名称>/`；从 `home/gateway.json` 复制配置，只在临时文件中调整端口、限额和故障目标。
真实密钥和数据库密码由环境变量或 `deploy/.env` 提供，不保存到验收材料。

| 范围 | 核对内容 |
| --- | --- |
| 模型入口 | 先确认 `/health` 和 `/v1/models` 的实际端口、模型标识，再验证独立路由与别名 |
| 双协议 | 两种入口 × 两种上游的对话、SSE 结束标记、工具调用与结果回传；精确 count_tokens；错误体 |
| 计量 | 响应 usage、各次尝试、访问日志与 PG 明细核对；取消时保留部分用量或未知值，不能伪造零 |
| 缓存 | 流式/非流式各自 MISS→HIT，响应一致且命中不访问上游；bypass、TTL、字节上限与淘汰 |
| 重试与降级 | 可重试错误、退避、不可重试错误、提交后中断、熔断单探测；真实模型最终响应 |
| 生命周期 | 下游取消、上游槽位释放、慢读背压、超时；SIGTERM 等待在途账本提交 |
| 配额 | 预输出阶段429、流中截断、下一请求拒绝；缓存命中和在途Redis增量不重复计费 |
| DAgent | 双协议文件工具、多轮会话、Esc取消、429重试、降级、用量显示、额度耗尽呈现 |

不停止或重启他人正在使用的模型。降级可在临时网关配置中将指定目标改到不可达的本机端口，真实备用模型保持运行。
修改 DAgent 正式配置前备份并确认授权；验收使用隔离的 DAgent home 和工作目录。SDK 参数以实际安装版本为准。

## 压测

使用 release 构建，记录 CPU/内存、线程数、数据库和限流开关、缓存预热、客户端接收缓冲等条件。
覆盖 healthz、真实响应缓存、直连/网关延迟、1000 并发流和100慢读者；具体命令、数据和测量边界见 [压测报告](design/benchmark.md)。
慢读者必须实际限制接收缓冲，否则客户端库自动预读会掩盖背压。TTFB 差值包含响应提交时机，不直接等于网关 CPU 开销。

## 故障注入上游

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
