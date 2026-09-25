#pragma once

#include <atomic>

#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/noncopyable.hpp"
#include "net/channel.hpp"
#include "net/event_loop.hpp"

struct pg_conn;
typedef struct pg_conn PGconn;

namespace gw::store {

struct PgError {
    std::string message;
    std::string sqlstate;
    bool connection_lost = false;
};

struct PgRow {
    std::vector<std::optional<std::string>> columns;  // 文本格式；NULL → nullopt
};
struct PgResult {
    std::vector<PgRow> rows;
    std::string status;  // 命令标签，如 "INSERT 0 1"
};

struct PgPreparedStatement {
    std::string name;
    std::string sql;
    int nparams = 0;
};

using PgParams = std::vector<std::optional<std::string>>;
using PgQueryCallback = std::function<void(std::expected<PgResult, PgError>)>;

// 单条查询（execute）或一个流水线批的成员（pipeline）
struct PgCall {
    std::string stmt_name;
    PgParams params;
    PgQueryCallback cb;
};

// libpq 异步接口接入 Reactor。只在所属 EventLoop 线程上使用（账本专用 loop），不加锁。
// 断线时未完成请求全部失败回调，之后自动重连并重新预编译语句。
class PgClient : noncopyable, public std::enable_shared_from_this<PgClient> {
public:
    PgClient(net::EventLoop* loop, std::string dsn, std::chrono::milliseconds connect_timeout);
    ~PgClient();

    // 异步建连；on_ready 在本 loop 上执行（成功一次；之后断线自动重连，不再回调）
    void connect(std::function<void(std::expected<void, PgError>)> on_ready);
    bool connected() const { return connected_; }

    // 预编译语句（连接建立后调用；可在迁移建表之后）。重复调用会替换语句集
    void prepare(std::vector<PgPreparedStatement> statements,
                 std::function<void(std::expected<void, PgError>)> on_done);

    void execute(std::string_view stmt_name, PgParams params, PgQueryCallback cb);
    // 一个事务里的一批：PQenterPipelineMode + 多条 + PQpipelineSync；done 在 SYNC 完成后调用
    void pipeline(std::vector<PgCall> calls, std::function<void(std::expected<void, PgError>)> done);
    // 原生 SQL 批（迁移等一次性语句；每条一个结果）。事务语句不能预编译，走这里
    void rawBatch(std::vector<std::string> sqls, std::function<void(std::expected<void, PgError>)> done);
    // 单条原生 SQL（非 pipeline，可读取结果；用于迁移版本检查等）
    void rawQuery(std::string sql, PgQueryCallback cb);

private:
    struct BatchContext {
        bool failed = false;
        std::string first_error;
    };
    struct QueryState {
        PgResult acc;
        PgError error;
        bool failed = false;
        bool batch_end_marker = false;
        std::shared_ptr<BatchContext> batch;
        PgQueryCallback cb;
        std::function<void(std::expected<void, PgError>)> batch_done;
    };

    void pollConnect();
    void afterReady();
    void ensureChannel();
    void updateChannel(bool reading, bool writing);
    void onReadable();
    void onWritable();
    void sendFlush();
    void consumeResults();
    void finishFront();
    void finishBatch(QueryState& marker);
    void pushQuery(const std::string& stmt_name, const PgParams& params, PgQueryCallback cb, bool batch_end_marker,
                   std::function<void(std::expected<void, PgError>)> batch_done);
    void pushRawQuery(const std::string& sql, PgQueryCallback cb);
    void handleConnectionLost(const std::string& reason);
    void scheduleReconnect();

    net::EventLoop* loop_;
    std::string dsn_;
    std::chrono::milliseconds connect_timeout_;
    std::vector<PgPreparedStatement> statements_;
    PGconn* conn_ = nullptr;
    std::unique_ptr<net::Channel> channel_;
    std::deque<std::unique_ptr<QueryState>> queue_;
    std::atomic<bool> connected_{false};
    bool connecting_ = false;
    bool in_pipeline_ = false;
    bool channel_registered_ = false;
    int reconnect_attempt_ = 0;
    std::function<void(std::expected<void, PgError>)> on_ready_;
};

}  // namespace gw::store
