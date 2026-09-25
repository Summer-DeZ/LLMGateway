# 运行与配置

所有相对路径和命令均以仓库根目录为基准。构建和验证见 [验证指南](validation.md)，完整字段见 [策略与配置](reference/policies.md#5-配置-homegatewayjson)。

## 启动网关

```bash
cmake --preset dev
cmake --build --preset dev -j
./build/dev/llm-gateway -c home/gateway.json
```

默认监听 `0.0.0.0:8080`，仅接受明文 HTTP 上游。配置在启动时加载并校验，不支持热加载。
运行日志默认写入 `logs/gateway.log`，结构化访问日志写入 `logs/access.log`。配置中的 `env:NAME` 字符串从环境变量读取，缺失会导致启动失败。

SIGINT/SIGTERM 触发优雅退出，等待活动请求及账本在途批次排空，最长受 `shutdown.drain_seconds` 约束。
管理页面交付时启动了用户级临时服务 `llmgateway-management.service`，监听 8080；未启用开机自启。

## API Key 管理页

访问 `http://127.0.0.1:8080/manage/`，无需登录，可创建、复制和禁用密钥，并按密钥查看今日、累计、按日和请求用量。只显示通过本页创建的密钥，静态配置中的示例身份继续有效。

独立启动（同一端口已有网关时不要重复启动）：

```bash
./deploy/run-management.sh
```

脚本使用 `home/management.json`，读取环境中的 GW_PG_DSN，或从 PGPASSWORD / GW_PG_PASSWORD / deploy/.env 获取现有网关数据库凭据；不启动数据库或模型。默认连接本机 5433 的 gateway 数据库。原 home/gateway.json 不变，管理功能缺省关闭。

用户级服务状态可用 `systemctl --user status llmgateway-management.service` 查看，日志位于 `logs/management-gateway.log` 与 `logs/management-access.log`。临时服务结束后也可用上面的脚本重新启动。

完整密钥仅创建时显示，关闭后不可恢复；遗失时禁用并重新创建。禁用不取消已通过鉴权的调用，因此在途请求结束后用量仍可能增长。页面只显示已入账数据，上游未完整上报会单独标注。

页面和管理 API 强制仅本机同源访问，不将这些路径通过反向代理对外公开。本机其他进程拥有同等管理能力，无管理员账号或密码。

实现与数据口径见 [API Key 管理](design/management.md)，真实验证见 [G16–G20](acceptance/api-key-management.md)。

## 本机模型入口

以下入口来自 2026-09-25 的本机配置；启动前用 `~/LLM/llm.sh status` 和模型服务的 `/v1/models` 确认当前状态。

| 网关独立别名 | 上游模型标识 | 地址 |
| --- | --- | --- |
| `qwen36` | `Qwen3.6-35B-A3B` | `http://127.0.0.1:10010/v1` |
| `nemotron` | `Nemotron-3.5-Lightning-30B-A3B` | `http://127.0.0.1:10011/v1` |
| `gemma4` | `gemma-4-26B-A4B-it` | `http://127.0.0.1:10012/v1` |

`qwen` 的目标链为 qwen36 → nemotron → gemma4；独立别名只使用指定模型。
`qwen-anth` 指向 Qwen3.6 原生 Anthropic 接口，`qwen-final` 使用同一模型但只依赖最终 usage。
模型由 `~/LLM/llm.sh` 管理，网关不自动加载模型；不修改、停止或重启他人正在使用的服务。

## 客户端请求

仓库的 `gk-dagent`、`gk-bench` 是示例身份。真实凭据按 [执行规则](../AGENTS.md) 管理，不写入文档或验收日志。

```bash
export GW=http://127.0.0.1:8080
export AUTH='Authorization: Bearer gk-dagent'
curl -s "$GW/healthz"
curl -s "$GW/v1/models" -H "$AUTH"
curl -sN "$GW/v1/messages" -H "$AUTH" -H 'Content-Type: application/json' \
  -d '{"model":"qwen","stream":true,"max_tokens":2048,"messages":[{"role":"user","content":"你好"}]}'
```

OpenAI 入口为 `/v1/chat/completions`；Anthropic 入口为 `/v1/messages`，要求正数 `max_tokens`。
`/v1/messages/count_tokens` 不要求 max_tokens，转发到声明支持计数的上游，无此能力返回 501。
详细请求映射、跨协议限制见 [协议转换](reference/protocol-translation.md)。

## 账本与配额

**默认 `home/gateway.json` 未启用 PostgreSQL/Redis。** 使用独立 PostgreSQL 16（本机 5433）和 Valkey 8（6380），不复用其他项目的数据服务。

首次使用时将 `deploy/.env.example` 复制为 `deploy/.env`，填入密码并限制该文件权限；已有文件不要覆盖。`deploy/.env` 已由 Git 忽略。

```bash
docker compose --env-file deploy/.env -f deploy/docker-compose.yml up -d
```

从配置副本启用下列段，保留已有其他配置。`GW_PG_DSN` 必须通过当前进程环境提供；它使用 libpq 连接串格式。密码可由环境中的 `PGPASSWORD` 提供，或包含在环境变量 DSN 中；不要把解析后的值写回 JSON 或日志。

```json
{
  "postgres": { "dsn": "env:GW_PG_DSN", "connect_timeout_ms": 3000 },
  "redis": { "host": "127.0.0.1", "port": 6380, "command_timeout_ms": 50 },
  "ledger": { "max_backlog": 100000, "batch_rows": 500, "batch_interval_ms": 200 },
  "quota": { "utc_offset_minutes": 480, "report_every_tokens": 32, "report_interval_ms": 100, "on_redis_failure": "reject" }
}
```

在需要限制的 `users` 条目中加入 `quota.daily_tokens` / `quota.monthly_tokens`；0 或缺省表示不限。
配置 quota 必须配置 Redis；配置 Redis 必须同时配置 PG，以支持启动重建。启动时迁移和预编译失败会拒绝服务；运行时账本不健康也返回 503。
部署、数据模型和全部字段见 [账本参考](reference/usage-ledger.md)。

```bash
curl -s "$GW/v1/usage" -H "$AUTH"
curl -s "$GW/v1/usage/requests" -H "$AUTH"
```

费用默认使用 Qwen 的演示单价；Nemotron/Gemma4 未配置单价时费用为0，不代表商业计费价格。

## DAgent

配置片段和启动命令见 [项目 README 的 DAgent 对接](../README.md#dagent-对接)。上下文窗口不得超过上游单槽容量（当前为131072）。
2026-09-25 的验收曾备份 `~/DAgent/home/models.json` 为 `models.json.G15.bak` 并加入两个网关配置；这些是本机历史操作，其他安装需自行配置。
验证时使用隔离的 DAgent home/工作目录和临时网关端口，避免影响既有会话。

## 排查入口

| 现象 | 核对项 |
| --- | --- |
| 建连失败、所有目标不可用 | 模型服务状态、端口和实际模型标识；访问日志中的各次尝试与熔断状态 |
| 429 | 区分速率限制和 insufficient_quota；查看 Retry-After、`/v1/usage` 和账户额度 |
| 503 | 模型加载状态、账本健康、Redis策略和网关运行日志 |
| 流中断 | `[DONE]` / message_stop 是否到达，访问日志的 outcome、取消与超时记录 |
| 用量未知 | final 上游在结束前取消可能没有计数；null/unreported 不等于零消耗 |
| 成本为0 | 是否配置实际服务上游的 pricing；是否命中缓存 |

`/metrics`、`/admin/stats`、`/admin/requests` 默认只允许本机访问。指标与账本口径见 [可观测性](design/observability.md)。
