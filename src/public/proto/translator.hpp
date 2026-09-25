#pragma once

#include <expected>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "proto/neutral.hpp"

namespace gw::proto {
std::expected<ChatRequest, std::string> decodeRequest(const nlohmann::json& body, Protocol protocol);
nlohmann::json encodeRequest(const ChatRequest& request, Protocol protocol, int default_max_tokens = 8192);
FinishReason decodeFinish(std::string_view reason, Protocol protocol);
std::string encodeFinish(FinishReason reason, Protocol protocol);
nlohmann::json translateResponse(const nlohmann::json& body, Protocol from, Protocol to, const std::string& model,
                                 const std::string& request_id);

// Pure protocol state: one instance for each attempt, never shared between
// requests.
class StreamDecoder {
   public:
    explicit StreamDecoder(Protocol protocol) : protocol_(protocol) {}
    std::vector<StreamEvent> feed(std::string_view event, std::string_view data);
    bool done() const { return done_; }
    const Usage& usage() const { return usage_; }
    const std::string& finishReason() const { return reason_; }

   private:
    struct Tool {
        int index;
        bool args = false;
        bool ended = false;
    };
    Protocol protocol_;
    bool done_ = false;
    Usage usage_;
    std::string reason_;
    std::map<int, Tool> tools_;
};

class StreamEncoder {
   public:
    StreamEncoder(Protocol protocol, std::string model, std::string id, bool wants_usage);
    void initialUsage(Usage usage) {
        if (!started_) usage_ = usage;
    }
    std::string feed(const StreamEvent& event);

   private:
    nlohmann::json chunk(nlohmann::json delta, nlohmann::json finish = nullptr) const;
    void start(std::string& out);
    void closeBlock(std::string& out);
    void textBlock(std::string& out, bool thinking, const std::string& text);
    void emitTool(std::string& out, int index);
    struct Tool {
        std::string id, name, args;
        bool ended = false;
        int block = -1;
    };
    Protocol protocol_;
    std::string model_, id_;
    std::int64_t created_;
    bool wants_usage_, started_ = false, done_ = false;
    Usage usage_;
    int next_block_ = 0, block_ = -1, active_tool_ = -1;
    std::string block_type_;
    std::map<int, Tool> tools_;
    std::vector<StreamEvent> deferred_;
};
}  // namespace gw::proto
