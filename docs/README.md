# 文档索引

本项目是本机明文 HTTP LLM 网关，支持 OpenAI / Anthropic 双协议。快速开始见 [项目 README](../README.md)，开发规则见 [AGENTS.md](../AGENTS.md)。

## 使用与维护

- [运行与配置](operations.md)：模型入口、网关启动、数据库部署、鉴权、DAgent 配置和排错入口。
- [构建与验证](validation.md)：纯逻辑单测、真实功能验收、故障注入上游和压测方法。
- [里程碑与验收记录](acceptance/history.md)：已完成的 G00–G15 历史状态、问题修复和证据位置。历史过程不作为当前待办或启动指令。

## 实现摘要

| 文档 | 内容 |
| --- | --- |
| [网络与连接](design/net.md) | Reactor、线程归属、对象所有权、退出顺序 |
| [HTTP 与 SSE](design/http.md) | 接口、解析、连接、错误体 |
| [代理状态机](design/proxy.md) | 重试、降级、熔断、取消与背压 |
| [双协议转换](design/protocol.md) | 请求映射、中立流事件、工具调用和 usage |
| [限流与配额](design/ratelimit.md) | 预留/结算、Redis 配额、PG 账本 |
| [响应缓存](design/cache.md) | 缓存键、LRU、TTL、容量与回放 |
| [可观测性](design/observability.md) | 指标、访问日志、用量与成本口径 |
| [API Key 管理](design/management.md) | 本机无登录页面、动态密钥、用量与持久化 |
| [压测结果](design/benchmark.md) | 实测数据、测量方法与结论边界 |

## 详细约束

这些文档保留行为、协议、线程、所有权和状态机合同。代码片段说明设计意图，具体类型与函数签名以源码为准；实现摘要记录实际采用的方案，验收记录提供运行证据。

| 文档 | 内容 |
| --- | --- |
| [架构](reference/architecture.md) | 模块依赖、线程模型、生命周期、目录与构建 |
| [Reactor](reference/net-reactor.md) | EventLoop、Transport、Connector、Resolver、连接池 |
| [HTTP](reference/http.md) | HTTP/1.1、SSE、路由、错误和 URL |
| [代理会话](reference/proxy-session.md) | 准入、状态转移、超时、重试、取消、收尾 |
| [协议转换](reference/protocol-translation.md) | OpenAI ↔ Anthropic 的逐字段和逐事件映射 |
| [策略与配置](reference/policies.md) | 限流、缓存、计量、指标及基础配置字段 |
| [账本与配额](reference/usage-ledger.md) | PG/Redis、迁移、计费、截断、查询与可选配置 |

## 资料维护

本期设计与任务：[API Key 管理页面](../nexttodo/README.md)；实现结论见 [G16–G20 验收](acceptance/api-key-management.md)。

行为或线程约束变化时更新对应参考文档；实现和使用方式变化时更新设计摘要与运行说明。
验收记录注明日期、构建/单测/真实运行结果、配置和证据位置，并区分通过、未验证和环境阻塞。
`temp/` 是本机原始材料目录，未纳入 Git；正式文档保留结论、方法与证据索引，不嵌入真实密钥和数据库密码。
