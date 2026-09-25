# 限流、配额与账本

限流为用户/模型各自的 RPM、TPM 令牌桶，固定顺序获取涉及的桶锁，任何桶拒绝时不扣其他桶。准入按启发式输入预估+max_tokens 预留，结束按真实 consumed 结算；未知消耗不退还预留。估算值不进入访问日志、usage、成本或账本。

Redis 共享日/月累计额度；PG 保存每请求记录及日汇总。准入读 Redis，流中按累计计数增量上报。Redis 回复更新剩余额度时扣除该回复之后尚在途的增量，避免过期额度快照。未输出前超额统一中止、结算尝试并返回 429；输出后超额以 length/max_tokens 和真实 usage 正常收尾。下一请求收到 insufficient_quota 与 Retry-After。

本机 llama.cpp 的 prompt_progress.processed 是包含缓存的已填充位置，computed=max(0,processed-cache)。直接把 processed 当新计算量会重复计算缓存；本次真实 DAgent 配额场景暴露并修复此问题，新增回归和缓存命中进度录制。

实测：DAgent 在 TPM=20000 下先收到 429、释放占用额度后重试成功；日额度 4700 时，输入 3840 + 输出 861 触发截断（超额 1 token），下一轮 429 且不进行长重试。两入口文件操作和多轮会话的访问日志与 PG 输入/输出逐条相等。PG 表锁延迟写入时退出仍等待提交。

部署密码来自环境变量或 deploy/.env。默认 home/gateway.json 未启用 PG/Redis；启用示例见 [usage-ledger](../reference/usage-ledger.md#8-配置)。配额限制是累计观测后的边界，流式分片/投机解码及并发上报可能产生小幅超额，不承诺绝对硬 token 截断。


设计约束：[参考文档](../README.md#详细约束)。历史证据：[验收记录](../acceptance/history.md)。所有数值为 2026-09-25 本机实测，不代表远端部署结果。
