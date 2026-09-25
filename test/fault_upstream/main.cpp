// 故障注入上游（规格 implementation-tasks.md §4）：基于本项目的 HttpServer。
// 故障模式写在路径前缀里：POST /f/<指令,指令,...>/v1/chat/completions 或 /v1/messages
#include <CLI/CLI.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "base/log.hpp"
#include "base/random.hpp"
#include "base/timestamp.hpp"
#include "http/http_server.hpp"
#include "http/response_writer.hpp"
#include "net/event_loop.hpp"
#include "net/inet_address.hpp"

using namespace std::chrono_literals;

namespace {

// ===== 指令 =====
struct FaultConfig {
    int status = 0;
    int times = -1;  // -1 = 一直故障
    std::optional<int> retry_after;
    int ttfb_ms = 0;
    bool close_before_head = false;
    int tokens = 50;
    int rate = 0;  // 每秒事件数；0 = 不限速
    int drop_after = -1;
    int hang_after = -1;
    int error_after = -1;
    std::string raw;
};

std::optional<FaultConfig> parseFaultInstructions(const std::string& segment) {
    FaultConfig cfg;
    cfg.raw = segment;
    std::size_t pos = 0;
    while (pos <= segment.size()) {
        std::size_t comma = segment.find(',', pos);
        std::string item = segment.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        if (!item.empty()) {
            if (item == "close_before_head") {
                cfg.close_before_head = true;
            } else if (item.rfind("status=", 0) == 0) {
                cfg.status = std::atoi(item.c_str() + 7);
            } else if (item.rfind("times=", 0) == 0) {
                cfg.times = std::atoi(item.c_str() + 6);
            } else if (item.rfind("retry_after=", 0) == 0) {
                cfg.retry_after = std::atoi(item.c_str() + 12);
            } else if (item.rfind("ttfb_ms=", 0) == 0) {
                cfg.ttfb_ms = std::atoi(item.c_str() + 8);
            } else if (item.rfind("tokens=", 0) == 0) {
                cfg.tokens = std::atoi(item.c_str() + 7);
            } else if (item.rfind("rate=", 0) == 0) {
                cfg.rate = std::atoi(item.c_str() + 5);
            } else if (item.rfind("drop_after=", 0) == 0) {
                cfg.drop_after = std::atoi(item.c_str() + 11);
            } else if (item.rfind("hang_after=", 0) == 0) {
                cfg.hang_after = std::atoi(item.c_str() + 11);
            } else if (item.rfind("error_after=", 0) == 0) {
                cfg.error_after = std::atoi(item.c_str() + 12);
            } else {
                return std::nullopt;
            }
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return cfg;
}

// times=K：同一个 x-request-id 的前 K 次故障（状态保存 60 秒）
class TimesRegistry {
public:
    bool shouldFault(const std::string& request_id, int times) {
        if (times < 0) return true;
        std::scoped_lock lock(mutex_);
        expire(gw::nowSteady());
        auto& entry = entries_[request_id];
        entry.first = gw::nowSteady();
        bool fault = entry.second < times;
        ++entry.second;
        return fault;
    }

private:
    void expire(gw::Timestamp now) {
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (now - it->second.first > 60s) it = entries_.erase(it);
            else ++it;
        }
    }
    std::mutex mutex_;
    std::map<std::string, std::pair<gw::Timestamp, int>> entries_;
};

// ===== 确定性分词：按空白切分的词数 =====
std::int64_t countWords(const std::string& text) {
    std::int64_t count = 0;
    bool in_word = false;
    for (unsigned char c : text) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            in_word = false;
        } else if (!in_word) {
            in_word = true;
            ++count;
        }
    }
    return count;
}

std::int64_t countMessagesWords(const nlohmann::json& body) {
    std::int64_t total = 0;
    if (!body.is_object()) return 0;
    auto it = body.find("messages");
    if (it == body.end() || !it->is_array()) return 0;
    for (const auto& m : *it) {
        if (!m.is_object()) continue;
        auto c = m.find("content");
        if (c == m.end()) continue;
        if (c->is_string()) {
            total += countWords(c->get_ref<const std::string&>());
        } else if (c->is_array()) {
            for (const auto& part : *c) {
                if (part.is_object()) {
                    if (auto t = part.find("text"); t != part.end() && t->is_string()) {
                        total += countWords(t->get_ref<const std::string&>());
                    }
                }
            }
        }
    }
    return total;
}

std::mutex& logMutex() {
    static std::mutex mutex;
    return mutex;
}

void faultLog(const std::string& request_id, const std::string& message) {
    std::scoped_lock lock(logMutex());
    std::printf("%lld %s %s\n", static_cast<long long>(gw::wallMillis()), request_id.c_str(), message.c_str());
    std::fflush(stdout);
}

std::string openAiErrorBody(int status, const std::string& message) {
    nlohmann::json j;
    j["error"]["message"] = message;
    j["error"]["type"] = "fault_error";
    j["error"]["code"] = status;
    return j.dump();
}

std::string anthropicErrorBody(int status, const std::string& message) {
    (void)status;
    nlohmann::json j;
    j["type"] = "error";
    j["error"]["type"] = "api_error";
    j["error"]["message"] = message;
    return j.dump();
}

}  // namespace

int main(int argc, char** argv) {
    CLI::App app{"fault-upstream"};
    std::string listen = "127.0.0.1:19000";
    int threads = 2;
    app.add_option("--listen", listen, "listen address");
    app.add_option("--threads", threads, "io threads");
    CLI11_PARSE(app, argc, argv);

    auto colon = listen.rfind(':');
    if (colon == std::string::npos) {
        std::fprintf(stderr, "invalid listen address\n");
        return 1;
    }
    std::string host = listen.substr(0, colon);
    auto port = static_cast<std::uint16_t>(std::atoi(listen.c_str() + colon + 1));

    gw::log::init("info", "", "");

    gw::net::EventLoop loop;
    gw::http::HttpServerOptions options;
    options.max_body_bytes = 16 * 1024 * 1024;
    gw::http::HttpServer server(&loop, gw::net::InetAddress(host, port), "fault", options);
    server.setThreadNum(threads);

    // 每个请求一个共享上下文（异步流跨定时器持有）
    struct RequestCtx : std::enable_shared_from_this<RequestCtx> {
        gw::http::HttpServer* server = nullptr;
        gw::net::TcpConnectionPtr conn;
        FaultConfig fault;
        std::string request_id;
        bool anthropic = false;
        bool stream = false;
        bool faulting = false;
        std::int64_t prompt_tokens = 0;
        nlohmann::json body;
        bool timings = false;
        bool progress = false;
        bool include_usage = false;
        bool keep_alive = true;
        // 流状态
        int sent = 0;
        bool finished = false;
        bool peer_closed = false;
        gw::net::TimerId timer;

        void log(const std::string& msg) { faultLog(request_id, msg); }

        void sendFull(int status, const std::string& text, bool close) {
            auto headers = gw::http::Headers{{"x-fault-served", fault.raw}};
            if (fault.retry_after) headers.emplace_back("retry-after", std::to_string(*fault.retry_after));
            gw::http::ResponseWriter writer(conn, request_id, !close && keep_alive);
            writer.writeFull(status, headers, "application/json", text);
            server->finishRequest(conn);
            if (close) conn->shutdown();
        }

    };

    auto times_registry = std::make_shared<TimesRegistry>();
    std::mutex ctx_mutex;
    std::map<gw::net::TcpConnection*, std::shared_ptr<RequestCtx>> active_streams;

    // 流式事件生成（std::function 以便在定时器里自引用）
    std::function<void(const std::shared_ptr<RequestCtx>&)> emit_next;
    emit_next = [&ctx_mutex, &active_streams, &emit_next](const std::shared_ptr<RequestCtx>& ctx) {
        if (ctx->peer_closed || ctx->finished) return;
        if (!ctx->conn->connected()) {
            ctx->peer_closed = true;
            ctx->log("peer closed while streaming sent=" + std::to_string(ctx->sent));
            return;
        }
        gw::http::ResponseWriter writer(ctx->conn, ctx->request_id, true);

        // 结束
        if (ctx->sent >= ctx->fault.tokens) {
            if (ctx->anthropic) {
                nlohmann::json delta;
                delta["type"] = "message_delta";
                delta["delta"] = {{"stop_reason", "end_turn"}};
                delta["usage"] = {{"output_tokens", ctx->sent}};
                writer.writeChunk("event: message_delta\ndata: " + delta.dump() + "\n\n");
                writer.writeChunk("event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n");
            } else {
                nlohmann::json final_chunk;
                final_chunk["choices"] = nlohmann::json::array(
                    {nlohmann::json{{"delta", nlohmann::json::object()}, {"finish_reason", "stop"}, {"index", 0}}});
                final_chunk["timings"] = {
                    {"cache_n", 0}, {"prompt_n", ctx->prompt_tokens}, {"predicted_n", ctx->sent}};
                writer.writeChunk("data: " + final_chunk.dump() + "\n\n");
                if (ctx->include_usage) {
                    nlohmann::json usage_chunk;
                    usage_chunk["choices"] = nlohmann::json::array();
                    usage_chunk["usage"] = {{"prompt_tokens", ctx->prompt_tokens},
                                            {"completion_tokens", ctx->sent},
                                            {"total_tokens", ctx->prompt_tokens + ctx->sent},
                                            {"prompt_tokens_details", {{"cached_tokens", 0}}}};
                    writer.writeChunk("data: " + usage_chunk.dump() + "\n\n");
                }
                writer.writeChunk("data: [DONE]\n\n");
            }
            writer.endStream();
            ctx->finished = true;
            ctx->log("stream done sent=" + std::to_string(ctx->sent));
            {
                std::scoped_lock lock(ctx_mutex);
                active_streams.erase(ctx->conn.get());
            }
            ctx->server->finishRequest(ctx->conn);
            return;
        }

        // error_after：流内 error 并结束
        if (ctx->fault.error_after >= 0 && ctx->sent >= ctx->fault.error_after) {
            if (ctx->anthropic) {
                writer.writeChunk(
                    "event: error\ndata: {\"type\":\"error\",\"error\":{\"type\":\"api_error\",\"message\":"
                    "\"fault injected\"}}\n\n");
            } else {
                writer.writeChunk("data: {\"error\":{\"message\":\"fault injected\",\"type\":\"fault_error\"}}\n\n");
            }
            writer.endStream();
            ctx->finished = true;
            ctx->log("stream error_after sent=" + std::to_string(ctx->sent));
            {
                std::scoped_lock lock(ctx_mutex);
                active_streams.erase(ctx->conn.get());
            }
            ctx->server->finishRequest(ctx->conn);
            return;
        }

        // drop_after：直接断开（不发结束标记）
        if (ctx->fault.drop_after >= 0 && ctx->sent >= ctx->fault.drop_after) {
            ctx->log("drop_after sent=" + std::to_string(ctx->sent));
            ctx->finished = true;
            {
                std::scoped_lock lock(ctx_mutex);
                active_streams.erase(ctx->conn.get());
            }
            ctx->conn->forceClose();
            return;
        }
        // hang_after：保持连接但停止发送
        if (ctx->fault.hang_after >= 0 && ctx->sent >= ctx->fault.hang_after) {
            ctx->log("hang_after sent=" + std::to_string(ctx->sent));
            return;
        }

        // 正常发一个文本增量（每个增量 = 1 token）
        std::string piece = "tok" + std::to_string(ctx->sent);
        if (ctx->anthropic) {
            if (ctx->sent == 0) {
                nlohmann::json start;
                start["type"] = "message_start";
                start["message"] = {{"id", "msg_fault"},
                                    {"type", "message"},
                                    {"role", "assistant"},
                                    {"content", nlohmann::json::array()},
                                    {"model", "fault"},
                                    {"usage", {{"input_tokens", ctx->prompt_tokens}, {"output_tokens", 0}}}};
                writer.writeChunk("event: message_start\ndata: " + start.dump() + "\n\n");
                nlohmann::json block;
                block["type"] = "content_block_start";
                block["index"] = 0;
                block["content_block"] = {{"type", "text"}, {"text", ""}};
                writer.writeChunk("event: content_block_start\ndata: " + block.dump() + "\n\n");
            }
            nlohmann::json delta;
            delta["type"] = "content_block_delta";
            delta["index"] = 0;
            delta["delta"] = {{"type", "text_delta"}, {"text", piece + " "}};
            writer.writeChunk("event: content_block_delta\ndata: " + delta.dump() + "\n\n");
        } else {
            nlohmann::json chunk;
            chunk["choices"] =
                nlohmann::json::array({nlohmann::json{{"delta", {{"content", piece + " "}}}, {"index", 0}}});
            if (ctx->timings) {
                chunk["timings"] = {
                    {"cache_n", 0}, {"prompt_n", ctx->prompt_tokens}, {"predicted_n", ctx->sent + 1}};
            }
            writer.writeChunk("data: " + chunk.dump() + "\n\n");
            if (ctx->progress) {
                nlohmann::json prog;
                prog["prompt_progress"] = {
                    {"total", ctx->prompt_tokens}, {"cache", 0}, {"processed", ctx->prompt_tokens}};
                writer.writeChunk("data: " + prog.dump() + "\n\n");
            }
        }
        ++ctx->sent;
        auto* loop = ctx->conn->getLoop();
        if (ctx->fault.rate > 0) {
            auto delay = std::chrono::milliseconds(std::max<std::int64_t>(1000 / ctx->fault.rate, 1));
            std::weak_ptr<RequestCtx> weak = ctx;
            ctx->timer = loop->runAfter(delay, [weak, &emit_next] {
                if (auto self = weak.lock()) emit_next(self);
            });
        } else {
            loop->queueInLoop([ctx, &emit_next] { emit_next(ctx); });
        }
    };

    auto respond_impl = [&ctx_mutex, &active_streams, &emit_next](const std::shared_ptr<RequestCtx>& ctx) {
        if (ctx->faulting && ctx->fault.status != 0) {
            ctx->log("fault status=" + std::to_string(ctx->fault.status));
            ctx->sendFull(ctx->fault.status,
                          ctx->anthropic ? anthropicErrorBody(ctx->fault.status, "fault injected")
                                         : openAiErrorBody(ctx->fault.status, "fault injected"),
                          false);
            return;
        }
        if (!ctx->stream) {
            std::string content;
            for (int i = 0; i < ctx->fault.tokens; ++i) {
                if (!content.empty()) content.push_back(' ');
                content += "tok" + std::to_string(i);
            }
            nlohmann::json out;
            if (ctx->anthropic) {
                out["id"] = "msg_fault";
                out["type"] = "message";
                out["role"] = "assistant";
                out["model"] = "fault";
                out["content"] = nlohmann::json::array({nlohmann::json{{"type", "text"}, {"text", content}}});
                out["stop_reason"] = "end_turn";
                out["usage"] = {{"input_tokens", ctx->prompt_tokens}, {"output_tokens", ctx->fault.tokens}};
            } else {
                out["id"] = "chatcmpl-fault";
                out["object"] = "chat.completion";
                out["model"] = "fault";
                out["choices"] = nlohmann::json::array(
                    {nlohmann::json{{"index", 0},
                                    {"finish_reason", "stop"},
                                    {"message", {{"role", "assistant"}, {"content", content}}}}});
                out["usage"] = {{"prompt_tokens", ctx->prompt_tokens},
                                {"completion_tokens", ctx->fault.tokens},
                                {"total_tokens", ctx->prompt_tokens + ctx->fault.tokens},
                                {"prompt_tokens_details", {{"cached_tokens", 0}}}};
            }
            ctx->log("nonstream sent tokens=" + std::to_string(ctx->fault.tokens));
            ctx->sendFull(200, out.dump(), false);
            return;
        }

        // 流式：开头写响应头 + 可选的首个进度事件，然后逐事件发
        {
            gw::http::ResponseWriter writer(ctx->conn, ctx->request_id, true);
            gw::http::Headers headers{{"x-fault-served", ctx->fault.raw}};
            writer.writeStreamHead(200, headers);
            if (ctx->progress) {
                nlohmann::json prog;
                prog["prompt_progress"] = {{"total", ctx->prompt_tokens}, {"cache", 0}, {"processed", 0}};
                writer.writeChunk("data: " + prog.dump() + "\n\n");
            }
        }
        {
            std::scoped_lock lock(ctx_mutex);
            active_streams[ctx->conn.get()] = ctx;
        }
        emit_next(ctx);
    };

    server.setRequestHandler(
        [&server, times_registry, &respond_impl](const gw::http::HttpRequest& request,
                                                 const gw::net::TcpConnectionPtr& conn) {
            auto ctx = std::make_shared<RequestCtx>();
            ctx->server = &server;
            ctx->conn = conn;
            ctx->request_id = request.header("x-request-id") ? std::string(*request.header("x-request-id"))
                                                             : gw::randomHex(8);
            ctx->keep_alive = request.keepAlive();

            if (request.method != "POST") {
                ctx->sendFull(405, "{\"error\":\"method not allowed\"}", false);
                return;
            }
            // 路径：/f/<instructions>/v1/... 或 /v1/...
            std::string api_path = request.path;
            FaultConfig fault;
            if (request.path.rfind("/f/", 0) == 0) {
                std::size_t slash = request.path.find('/', 3);
                if (slash == std::string::npos) {
                    ctx->sendFull(400, "{\"error\":\"bad fault path\"}", true);
                    return;
                }
                auto parsed = parseFaultInstructions(request.path.substr(3, slash - 3));
                if (!parsed) {
                    ctx->sendFull(400, "{\"error\":\"unknown fault instruction\"}", true);
                    return;
                }
                fault = *parsed;
                api_path = request.path.substr(slash);
            }
            ctx->fault = fault;
            ctx->anthropic = api_path == "/v1/messages";
            try {
                ctx->body = nlohmann::json::parse(request.body);
                ctx->stream = ctx->body.value("stream", false);
                ctx->timings = ctx->body.value("timings_per_token", false);
                ctx->progress = ctx->body.value("return_progress", false);
                if (!ctx->anthropic) {
                    auto so = ctx->body.find("stream_options");
                    ctx->include_usage = so != ctx->body.end() && so->is_object() && so->value("include_usage", false);
                }
            } catch (...) {
                ctx->stream = false;
            }
            ctx->prompt_tokens = countMessagesWords(ctx->body);
            ctx->log("conn open path=" + request.path + " prompt_tokens=" + std::to_string(ctx->prompt_tokens));

            if (fault.close_before_head) {
                ctx->log("close_before_head");
                conn->forceClose();
                return;
            }
            ctx->faulting = times_registry->shouldFault(ctx->request_id, fault.times);
            if (fault.ttfb_ms > 0) {
                std::weak_ptr<RequestCtx> weak = ctx;
                conn->getLoop()->runAfter(std::chrono::milliseconds(fault.ttfb_ms), [weak, &respond_impl] {
                    if (auto self = weak.lock(); self && self->conn->connected()) respond_impl(self);
                });
            } else {
                respond_impl(ctx);
            }
        });

    server.setCloseHandler([&ctx_mutex, &active_streams](const gw::net::TcpConnectionPtr& conn) {
        std::shared_ptr<RequestCtx> ctx;
        {
            std::scoped_lock lock(ctx_mutex);
            auto it = active_streams.find(conn.get());
            if (it != active_streams.end()) {
                ctx = it->second;
                active_streams.erase(it);
            }
        }
        if (ctx && !ctx->finished && !ctx->peer_closed) {
            ctx->peer_closed = true;
            faultLog(ctx->request_id, "conn close while streaming sent=" + std::to_string(ctx->sent));
        }
    });

    server.start();
    std::printf("fault-upstream listening on %s (%d threads)\n", server.listenAddress().c_str(), threads);
    std::fflush(stdout);
    loop.loop();
    server.stop();
    gw::log::shutdown();
    return 0;
}
