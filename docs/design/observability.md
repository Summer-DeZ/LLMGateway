# 可观测性与计量

每请求 RequestRecord 记录实际上游、全部尝试、结局、HTTP 状态、TTFT、延迟、token、成本、缓存和背压。Prometheus `/metrics` 与 `/admin/stats`、`/admin/requests` 读取有界快照。结构化访问日志异步写入并轮转，账本持久记录由独立 PostgreSQL writer 批量写入。

消耗 consumed=cached+prompt_computed+completion；projected=prompt+completion 只用于提前配额判断。未知字段为 null，usage_complete=false；建连失败等明确未推理的尝试为 rejected/0；取消后的已知部分为 upstream_partial。final 上游取消无 usage 时为 unreported。

Qwen3.6 保留原演示单价，新模型未配置单价时成本为 0；这不代表配置了商业计费价格。缓存命中记录原 usage 以说明复用量，但实际消耗/费用为 0，并独立统计节省成本。

实测：入口更新验收的 22 条完整响应与访问日志逐条一致；G15 DAgent 的完整请求与 PG 明细一致。缓存压测日志轮转保留有限历史，高吞吐评估以 wrk 输出及指标为准，不把轮转后的日志行数当全部请求量。默认 PG 未启用时只有访问日志，不声称数据库计费已开启。


设计约束：[参考文档](../README.md#详细约束)。历史证据：[验收记录](../acceptance/history.md)。所有数值为 2026-09-25 本机实测，不代表远端部署结果。
