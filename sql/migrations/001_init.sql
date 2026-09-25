-- 001_init.sql：用量账本初始 schema（与 gateway/ledger.cpp 内嵌副本保持一致）
CREATE TABLE usage_records (
    ledger_id          text        PRIMARY KEY,   -- 网关生成的唯一 ID（不用客户端的 x-request-id）
    client_request_id  text,
    gateway_instance   text        NOT NULL,      -- 哪个网关实例写的
    key_name           text        NOT NULL,      -- users.*.name；绝不保存 key 原文
    billing_day        date        NOT NULL,      -- 按 started_at 与配置的 UTC 偏移计算
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
    billable_tokens    bigint      NOT NULL,      -- 各尝试 consumed 之和；consumed 为 null 的尝试按 0 计
    usage_complete     boolean     NOT NULL,
    cost_micro_usd     bigint,                    -- 百万分之一美元
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
    unreported_requests bigint NOT NULL,
    truncated_requests  bigint NOT NULL,
    PRIMARY KEY (key_name, day, model)
);
