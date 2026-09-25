# 代理请求状态机

`ProxySession` 独占一次请求的计量、重试上下文、解码/编码器与取消状态。状态依次为 admitting、acquiring、await_head、await_first_event、streaming/backoff、done。准入按鉴权、解析与路由、账本健康、配额、限流、缓存、尝试计划执行。

每个新目标都重新编码请求，独立创建协议和计量对象。旧异步回调带 attempt epoch，不能污染新尝试。响应提交后不能换目标；上游异常以入口协议流内错误收尾，不输出成功终止标记。未提交时依据 HTTP/网络错误分类退避重试，目标耗尽则降级。

`qwen` 当前依序使用 qwen36、nemotron、gemma4。临时不可达端口验收中，分别 attempts=3 到 Nemotron、attempts=5 到 Gemma4；DAgent 两轮对话在熔断打开后从 3 次尝试降到 1 次。半开状态只有一个在途探测，经过冷却时间不能自动再放行。

背压高水位触发后暂停上游读取和 idle 超时，启动 stall 计时；下游排空后恢复。取消先向上游 FIN，再非阻塞排空已有计量字节。Anthropic 排空分别保留最后 message_start 和 message_delta；final 上游取消没有计数时记 null/unreported，不伪造零。

真实 DAgent Esc 已产生 cancelled 访问记录，随后模型槽位为空。100 个慢读者均触发背压，RSS 峰值 22.25 MiB，见 [压测](benchmark.md)。


规格与历史记录：[实施任务](../../nexttodo/implementation-tasks.md)。所有数值为 2026-09-25 本机实测，不代表远端部署结果。
