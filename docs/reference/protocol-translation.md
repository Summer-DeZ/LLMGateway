# proto：OpenAI ↔ Anthropic 协议转换

本文保留对外行为、线程归属、所有权和状态机约束；接口片段用于说明设计，具体签名以源代码为准。
实现摘要、运行说明和历史验收见 [文档索引](../README.md)。


模块 `gw_proto`，纯逻辑、不碰网络，全部可单测。映射规则与 DAgent 的
`~/DAgent/src/private/llm/provider_chat.cpp`、`provider_anthropic.cpp` 保持一致（system 合并、并行工具结果合并、usage 累计值等）。

## 1. 四种组合

| 入口 \ 上游 | openai-chat | anthropic |
| --- | --- | --- |
| OpenAI `/v1/chat/completions` | 透传（改写 model、强制 include_usage） | 转换 |
| Anthropic `/v1/messages` | 转换 | 透传（改写 model） |

- 透传：请求体只做 §6 的最小改写；响应流逐事件观察 + 重新格式化，不改内容。
- 转换：`入口请求 → 中立 ChatRequest → 上游请求`；`上游流事件 → 中立 StreamEvent → 入口流事件`。
- 同一模型别名的目标链可以混合两种上游，所以每次**尝试**都要按该目标的 kind 重新编码请求、新建流转换器（转换器有状态，不能跨尝试复用）。

## 2. 中立模型（`proto/neutral.hpp`）

```cpp
enum class Role { system, user, assistant, tool };
struct ToolCall { std::string id, name, arguments; };             // arguments 为 JSON 字符串
struct Message {
    Role role;
    std::string content;                   // 纯文本（多个 text 块按 "\n" 连接）
    std::string reasoning;                 // 历史思考，默认不发给上游
    std::vector<ToolCall> tool_calls;      // assistant
    std::string tool_call_id;              // tool
};
struct ToolSpec { std::string name, description; nlohmann::json parameters; };
struct ChatRequest {
    std::string model;                     // 上游真实模型名（编码前由路由填入）
    std::vector<Message> messages;         // system 也在这里，编码为 Anthropic 时提取到顶层
    std::vector<ToolSpec> tools;
    nlohmann::json tool_choice;            // null 表示未指定
    std::optional<int> max_tokens;
    std::optional<double> temperature, top_p;
    std::vector<std::string> stop;
    bool stream = false;
    bool client_wants_usage = false;       // OpenAI stream_options.include_usage 的原值
    std::string user;                      // OpenAI user / Anthropic metadata.user_id
};

struct TextDelta { std::string text; };
struct ReasoningDelta { std::string text; };
struct ToolCallBegin { int index; std::string id, name; };   // index：中立的工具序号，从 0 连续编号
struct ToolCallArgs { int index; std::string fragment; };
struct ToolCallEnd { int index; };
struct Usage { std::int64_t prompt = 0, completion = 0, cached = 0; };   // 均为累计值
enum class FinishReason { stop, length, tool_calls, content_filter, error };
struct Finish { FinishReason reason; std::string raw; };      // 终止事件，之后不再有事件
using StreamEvent = std::variant<TextDelta, ReasoningDelta, ToolCallBegin, ToolCallArgs, ToolCallEnd, Usage, Finish>;
```

**终止约定**（与 DAgent 一致）：解码器只在上游**结束标记**处（OpenAI `[DONE]`、Anthropic `message_stop`）
依次交付：未关闭的 `ToolCallEnd` → `Usage` → `Finish`。这样编码器拿到 Finish 时 usage 一定已经就绪。
上游 `error` 事件 → `Finish{error, message}`。

"首 token"（TTFT 用）：第一个 `TextDelta` / `ReasoningDelta` / `ToolCallBegin`，文本为空的不算。

## 3. 请求映射

### 3.1 OpenAI → 中立 → Anthropic

| OpenAI | Anthropic | 说明 |
| --- | --- | --- |
| `messages[role=system/developer].content` | 顶层 `system`（多条用 `\n\n` 连接） | |
| `messages[role=user].content`（字符串或 text 部件数组） | `{"role":"user","content":[{"type":"text","text":…}]}` | 图片部件：跨协议不支持 → 400 |
| `assistant.content` + `assistant.tool_calls[]` | `assistant` 的 `text` 块 + `tool_use` 块（`input` = `json::parse(arguments)`，解析失败用 `{}`） | content 为 null/空时不生成 text 块 |
| `tool` 消息（`tool_call_id`、`content`） | `user` 消息里的 `tool_result` 块（`tool_use_id`、`content`） | **相邻的多个 tool 消息合并进同一条 user 消息**（并行工具调用必须如此） |
| 连续同角色消息 | 合并到同一条消息的 content 数组 | Anthropic 要求 user/assistant 交替 |
| `tools[].function{name,description,parameters}` | `tools[]{name,description,input_schema}` | |
| `tool_choice`: `"auto"` / `"none"` / `"required"` / `{"type":"function","function":{"name":X}}` | `{"type":"auto"}` / 不发 tools / `{"type":"any"}` / `{"type":"tool","name":X}` | |
| `max_tokens` 或 `max_completion_tokens` | `max_tokens`（必填；都缺时用上游配置 `default_max_tokens`，默认 8192） | |
| `temperature`（0–2） | `temperature`（截断到 0–1） | |
| `top_p`、`stop`（字符串或数组） | `top_p`、`stop_sequences` | |
| `user` | `metadata.user_id` | |
| `stream`、`stream_options.include_usage` | `stream`（Anthropic 流里总有 usage） | 记下 `client_wants_usage` |
| `n > 1` | — | 400（不支持） |
| `response_format` 为 json_object/json_schema | — | 跨协议 400；透传时保留 |
| `logprobs`、`presence_penalty`、`frequency_penalty`、`seed` | — | 丢弃 |
| assistant 历史的 `reasoning_content` | — | 丢弃（DAgent 默认也不回传） |

### 3.2 Anthropic → 中立 → OpenAI

| Anthropic | OpenAI | 说明 |
| --- | --- | --- |
| 顶层 `system`（字符串或 text 块数组） | 首条 `{"role":"system","content":…}` | |
| `user` 的 `text` 块 | `user` 消息 content 字符串 | |
| `user` 的 `tool_result` 块 | 每块一条 `{"role":"tool","tool_call_id":…,"content":…}`，放在同一条 user 消息里其他文本**之前** | `content` 为块数组时取其中 text 连接；`is_error:true` 时内容前加 `"[tool error] "` |
| `assistant` 的 `text` 块 / `tool_use` 块 | `content` / `tool_calls[]{id,type:"function",function{name,arguments:input.dump()}}` | 纯工具调用时 `content: null` |
| `assistant` 的 `thinking` / `redacted_thinking` 块 | 丢弃 | |
| `tools[]{name,description,input_schema}` | `tools[]{type:"function",function{name,description,parameters}}` | |
| `tool_choice` `auto` / `any` / `tool{name}` / `none` | `"auto"` / `"required"` / `{"type":"function","function":{"name"}}` / `"none"` | |
| `max_tokens` | `max_tokens` | |
| `stop_sequences`、`temperature`、`top_p` | `stop`、`temperature`、`top_p` | |
| `metadata.user_id` | `user` | |
| `stream` | `stream` + `stream_options.include_usage=true` | |
| `image`、`document` 块 | — | 400 |
| `cache_control`、`thinking` 配置、`top_k` | — | 丢弃（`top_k` 可放进上游 `extra_body` 规则，默认丢弃） |

## 4. 流式事件转换

实现为两层：`Decoder`（上游协议 SSE 事件 → 中立事件）与 `Encoder`（中立事件 → 入口协议 SSE 事件）。
`StreamTranslator = Decoder + Encoder`；透传时不需要 Encoder，但仍用 Decoder 观察 usage 和首 token。

### 4.1 OpenAI 解码器

| 上游 chunk | 中立事件 |
| --- | --- |
| `choices[0].delta.content` 非空 | `TextDelta` |
| `choices[0].delta.reasoning_content`（或 `reasoning`，前者优先） | `ReasoningDelta` |
| `delta.tool_calls[k]` 首次出现该 `index` | `ToolCallBegin{中立序号, id, function.name}`；若同块带 `arguments` 再发 `ToolCallArgs` |
| 同一 `index` 的后续 `function.arguments` | `ToolCallArgs` |
| `choices[0].finish_reason` | 记住 raw，暂不发 |
| `usage` 字段（通常在 `choices: []` 的最后一块） | 记住 usage：`prompt_tokens`、`completion_tokens`、`prompt_tokens_details.cached_tokens` |
| `data: [DONE]` | 依次发：未关闭工具的 `ToolCallEnd` → `Usage` → `Finish` |
| 顶层 `error` 对象 | `Finish{error, message}` |

finish_reason：`stop`→stop、`length`→length、`tool_calls`/`function_call`→tool_calls、`content_filter`→content_filter，其他→error（raw 保留原文）。

### 4.2 Anthropic 解码器

| 上游事件 | 中立事件 |
| --- | --- |
| `message_start` | 记 usage：`cached = cache_read_input_tokens`，`prompt = input_tokens + cache_read + cache_creation`，`completion = output_tokens` |
| `content_block_start` type=`tool_use` | `ToolCallBegin`（块 index → 中立工具序号的映射表）；`input` 非空对象时补发 `ToolCallArgs{input.dump()}` |
| `content_block_start` type=`text` 且带初始 text | `TextDelta` |
| `content_block_delta` `text_delta` / `thinking_delta` / `input_json_delta` | `TextDelta` / `ReasoningDelta` / `ToolCallArgs` |
| `content_block_delta` `signature_delta` | 忽略 |
| `content_block_stop`（工具块） | `ToolCallEnd`（没有收到任何参数片段时先补 `ToolCallArgs{"{}"}`） |
| `message_delta` | 记 `stop_reason`；`usage.output_tokens` 是**累计值**，覆盖而不是相加 |
| `message_stop` | `Usage` → `Finish` |
| `ping` | 忽略（但算作收到字节） |
| `error` | `Finish{error, error.message}` |

stop_reason：`end_turn`/`stop_sequence`→stop、`max_tokens`→length、`tool_use`→tool_calls、`refusal`→content_filter，其他→error。

### 4.3 OpenAI 编码器（入口为 OpenAI）

公共字段：`"id":"chatcmpl-<request_id>"`、`"object":"chat.completion.chunk"`、`"created":<unix 秒>`、`"model":"<客户端请求的别名>"`。

| 中立事件 | 输出 `data:` |
| --- | --- |
| （首个事件之前） | `{"choices":[{"index":0,"delta":{"role":"assistant","content":""},"finish_reason":null}]}` |
| `TextDelta` | `delta:{"content":t}` |
| `ReasoningDelta` | `delta:{"reasoning_content":t}` |
| `ToolCallBegin` | `delta:{"tool_calls":[{"index":i,"id":id,"type":"function","function":{"name":n,"arguments":""}}]}` |
| `ToolCallArgs` | `delta:{"tool_calls":[{"index":i,"function":{"arguments":f}}]}` |
| `ToolCallEnd` | 不输出 |
| `Usage` | 暂存 |
| `Finish` | `delta:{}` + `finish_reason`；若 `client_wants_usage` 再发 `{"choices":[],"usage":{"prompt_tokens","completion_tokens","total_tokens","prompt_tokens_details":{"cached_tokens"}}}`；最后 `data: [DONE]` |
| `Finish{error}` | 按 [proxy-session.md §6.3](proxy-session.md#63-提交后的上游失败) 发流内错误，不发 `[DONE]` |

### 4.4 Anthropic 编码器（入口为 Anthropic）

| 中立事件 | 输出（`event:` 名与 `data.type` 相同） |
| --- | --- |
| （首个事件之前） | `message_start`：`{"type":"message_start","message":{"id":"msg_<request_id>","type":"message","role":"assistant","model":"<别名>","content":[],"stop_reason":null,"stop_sequence":null,"usage":{"input_tokens":<P - C>,"cache_read_input_tokens":<C>,"output_tokens":0}}}`，P/C 取自 UsageMeter |
| `TextDelta` | 当前没有打开的 text 块 → 先关闭其他打开的块，`content_block_start{index:k, content_block:{type:"text",text:""}}`；然后 `content_block_delta{index:k, delta:{type:"text_delta",text}}` |
| `ReasoningDelta` | 同上，块类型 `thinking`（`{"type":"thinking","thinking":""}`），delta `thinking_delta` |
| `ToolCallBegin` | 关闭打开的块，`content_block_start{index:k, content_block:{type:"tool_use",id,name,input:{}}}` |
| `ToolCallArgs` | `content_block_delta{index:k, delta:{type:"input_json_delta",partial_json:f}}` |
| `ToolCallEnd` | `content_block_stop{index:k}` |
| `Usage` | 暂存 |
| `Finish` | 关闭打开的块；`message_delta{"delta":{"stop_reason":R,"stop_sequence":null},"usage":{"input_tokens":P-C,"cache_read_input_tokens":C,"output_tokens":O}}`（P 为 prompt 总数，C 为缓存命中，O 为输出，均取自最终 usage）；`message_stop` |
| `Finish{error}` | `event: error` + `{"type":"error","error":{"type":"api_error","message":…}}` |

- 块 index `k` 从 0 开始，每打开一个新块加一。当前工具块的参数即时转发；并行工具交错时暂存其他工具参数，待当前工具关闭后依序输出，避免向已关闭块追加参数。工具期间的文本暂存至工具结束；单工具暂存上限 4MiB。Anthropic 透传配额截断跟踪实际已发送块索引，不能使用转换编码器的内部索引收尾。
- `message_start` 里的 token 数必须是上游报告的精确值（[policies.md §4.7](policies.md#47-其他用到-token-数的地方)）。
  `metering: llamacpp` 的上游在第一个 token 之前一定已经发过进度事件或 timings，所以 commit 时已知。
  `final` 计量的 OpenAI 上游（如 `qwen36-final`）此时还不知道，写 0，精确值在 `message_delta.usage.input_tokens` 里给出。
  **已知影响**：DAgent 的 anthropic provider 只从 `message_start` 读 prompt token。所以 `gw-claude` 背后如果是 `final` 计量的 OpenAI 上游，
  DAgent 会看到 0。背后是 Qwen3.6 时没有这个问题。
- 不发 `ping`、不发 `signature_delta`（没有签名；DAgent 允许签名为空）。

## 5. 非流式响应映射

| OpenAI `chat.completion` | Anthropic `message` |
| --- | --- |
| `id`（`chatcmpl-<request_id>`） | `id`（`msg_<request_id>`） |
| `choices[0].message.content` | `content[]` 中的 `text` 块 |
| `choices[0].message.reasoning_content` | `thinking` 块（放在 text 前面） |
| `choices[0].message.tool_calls[]` | `tool_use` 块（`input` = 解析后的 arguments） |
| `choices[0].finish_reason` | `stop_reason`（映射同 §4） |
| `usage.prompt_tokens / completion_tokens / prompt_tokens_details.cached_tokens` | `usage.input_tokens（不含缓存部分）/ output_tokens / cache_read_input_tokens` |
| `model` | `model`（都写客户端请求的别名） |

## 6. 透传时的最小改写

| 入口=上游 | 请求改写 | 响应处理 |
| --- | --- | --- |
| OpenAI | `model` ← 上游真实模型；`stream:true` 时强制 `stream_options.include_usage=true`；按上游 `metering` 注入计量开关（[policies.md §4.2](policies.md#42-上游计量能力-metering)）；上游配置的 `extra_body` 只补缺失的顶层字段 | 客户端原本没要 usage 时，丢弃 `choices` 为空且只含 `usage` 的那一块；删掉网关注入的 `timings`/`prompt_progress`，丢弃只含进度的块；其余事件原样 |
| Anthropic | `model` ← 上游真实模型；`extra_body` 同上；`anthropic-version` 缺省补 `2023-06-01`，客户端的 `anthropic-beta` 头原样转发 | 原样 |

透传不改写流里的 `model` 字段（它会显示上游的真实模型名），客户端可从响应头 `x-gateway-upstream` 得知实际服务的上游。

## 7. 错误体转换

上游非 2xx 且不可重试时：同协议原样透传；跨协议时从上游错误体里提取 message（OpenAI `error.message`、Anthropic `error.message`，
都取不到就用原始文本前 1KB），再按 [http.md §5](http.md#5-错误体格式按入口协议) 生成入口协议的错误体，**状态码保持上游原值**，message 文本不改。

## 8. 计量字段与 token 估算

- 解码器除了产出中立事件，还要把 llama.cpp 的 `timings`、`prompt_progress`，vLLM 的逐块 `usage`，以及 Anthropic `message_start` 的 usage 交给 UsageMeter（[policies.md §4.3](policies.md#43-usagemeter)）。
  只含 `prompt_progress` 的 chunk 不产出任何中立事件。
- 跨协议转换时，编码器本来就不会输出这些字段；透传时由 PassthroughObserver 删除（客户端自己请求了则保留）。
- `proto/token_estimate.hpp` 使用与 DAgent `TokenEstimator` 相同的启发式：ASCII 字节 / 4 + 非 ASCII 码点数 × 1，每条消息额外 +4，工具定义按 JSON 文本计算。
  **它只用于限流预留**，不能出现在任何记录、指标、成本或返回给客户端的 usage 里。
