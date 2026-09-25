# OpenAI 与 Anthropic

请求走“入口 JSON → ChatRequest → 上游 JSON”；同协议仅改 model、必要 usage/计量选项及缺失的 extra_body。系统消息合并、相邻工具结果合并、tool_choice 映射、文本块拼接、max_tokens 默认、温度范围处理与错误映射均有纯逻辑单测。跨协议图片/document、n>1、JSON response_format 显式拒绝；同协议保留原始能力。

每次尝试拥有独立 StreamDecoder/StreamEncoder。中立事件覆盖文本、思考、工具开始/参数/结束、累计 usage 与 finish。只在 `[DONE]` 或 message_stop 发终止事件，之前先结束工具并发送最终 usage。已终止的解码器忽略迟到事件。

Anthropic 编码器在首个输出前从 UsageMeter 获取精确输入计数；最终 message_delta 同样携带输入、缓存和输出值。工具并行交错时，当前工具流式输出，其他工具参数暂存后顺序开块，避免向关闭块追加参数。透传的配额截断跟踪原始实际 block index，与转换编码器状态分离。

验收：真实 Qwen 录制两份带并行工具的样本，双向解码/编码/解码后的正文、思考、工具、参数与 usage 等价。OpenAI/Anthropic SDK 的四组合对话、SSE、工具调用并回传结果共 15/15 通过；边界错误及原生 Anthropic 配额截断也通过。Anthropic SDK 1.8.0 的 temperature 通过 extra_body 发送。

当前限制：跨协议不传图片或供应商专属推理签名；final OpenAI 上游在 message_start 前没有输入计数时返回 0，最终 message_delta 补齐。原生同协议模型名保持上游真实值，转换输出使用客户端别名。


规格与历史记录：[实施任务](../../nexttodo/implementation-tasks.md)。所有数值为 2026-09-25 本机实测，不代表远端部署结果。
