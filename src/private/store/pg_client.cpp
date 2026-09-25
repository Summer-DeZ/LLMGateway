#include "store/pg_client.hpp"

#include <libpq-fe.h>

#include "base/log.hpp"

namespace gw::store {

namespace {
constexpr int kMaxBackoffAttempt = 8;  // 100ms × 2^7 = 12.8s → 封顶 15s
}  // namespace

PgClient::PgClient(net::EventLoop* loop, std::string dsn, std::chrono::milliseconds connect_timeout)
    : loop_(loop), dsn_(std::move(dsn)), connect_timeout_(connect_timeout) {}

void PgClient::prepare(std::vector<PgPreparedStatement> statements,
                       std::function<void(std::expected<void, PgError>)> on_done) {
    loop_->assertInLoopThread();
    if (!connected_) {
        if (on_done) on_done(std::unexpected(PgError{"not connected", "", true}));
        return;
    }
    statements_ = std::move(statements);
    if (statements_.empty()) {
        if (on_done) on_done({});
        return;
    }
    if (PQenterPipelineMode(conn_) == 0) {
        if (on_done) on_done(std::unexpected(PgError{"enterPipelineMode failed (prepare)", "", false}));
        return;
    }
    in_pipeline_ = true;
    auto batch = std::make_shared<BatchContext>();
    for (const auto& stmt : statements_) {
        if (PQsendPrepare(conn_, stmt.name.c_str(), stmt.sql.c_str(), stmt.nparams, nullptr) == 0) {
            handleConnectionLost(std::string("sendPrepare failed: ") + PQerrorMessage(conn_));
            return;
        }
        auto state = std::make_unique<QueryState>();
        state->batch = batch;
        state->cb = [name = stmt.name](std::expected<PgResult, PgError> r) {
            if (!r) SPDLOG_WARN("pg prepare {} failed: {}", name, r.error().message);
        };
        queue_.push_back(std::move(state));
    }
    auto marker = std::make_unique<QueryState>();
    marker->batch_end_marker = true;
    marker->batch = batch;
    marker->batch_done = [this, on_done = std::move(on_done)](std::expected<void, PgError> r) {
        if (in_pipeline_) {
            PQexitPipelineMode(conn_);
            in_pipeline_ = false;
        }
        if (on_done) on_done(std::move(r));
    };
    if (PQpipelineSync(conn_) == 0) {
        queue_.pop_back();
        in_pipeline_ = false;
        handleConnectionLost("pipelineSync failed (prepare)");
        return;
    }
    queue_.push_back(std::move(marker));
    sendFlush();
    consumeResults();
}

PgClient::~PgClient() {
    // 必须在所属 loop 线程上析构：先摘掉 Channel（否则 Channel 析构断言 poller 仍持有它），再关连接
    if (channel_) {
        channel_->disableAll();
        if (channel_registered_) {
            channel_->remove();
            channel_registered_ = false;
        }
        channel_.reset();
    }
    if (conn_ != nullptr) PQfinish(conn_);
}

void PgClient::connect(std::function<void(std::expected<void, PgError>)> on_ready) {
    loop_->assertInLoopThread();
    on_ready_ = std::move(on_ready);
    conn_ = PQconnectStart(dsn_.c_str());
    if (conn_ == nullptr || PQstatus(conn_) == CONNECTION_BAD) {
        PgError err{"PQconnectStart failed", "", true};
        if (on_ready_) on_ready_(std::unexpected(err));
        return;
    }
    connecting_ = true;
    ensureChannel();
    pollConnect();
}

void PgClient::ensureChannel() {
    if (channel_ && channel_registered_) {
        // libpq 建连期间 socket 可能变化；这里简单起见重新挂一次
        if (channel_->fd() == PQsocket(conn_)) return;
        channel_->disableAll();
        channel_->remove();
        channel_registered_ = false;
    }
    channel_ = std::make_unique<net::Channel>(loop_, PQsocket(conn_));
    std::weak_ptr<PgClient> weak = shared_from_this();
    channel_->setReadCallback([weak] {
        if (auto self = weak.lock()) self->onReadable();
    });
    channel_->setWriteCallback([weak] {
        if (auto self = weak.lock()) self->onWritable();
    });
    channel_->setCloseCallback([weak] {
        if (auto self = weak.lock()) self->handleConnectionLost("socket closed");
    });
    channel_->setErrorCallback([weak] {
        if (auto self = weak.lock()) self->handleConnectionLost("socket error");
    });
    channel_->tie(shared_from_this());
    channel_registered_ = true;
}

void PgClient::pollConnect() {
    loop_->assertInLoopThread();
    auto status = PQconnectPoll(conn_);
    switch (status) {
        case PGRES_POLLING_OK:
            connecting_ = false;
            afterReady();
            return;
        case PGRES_POLLING_READING:
            updateChannel(true, false);
            return;
        case PGRES_POLLING_WRITING:
            updateChannel(false, true);
            return;
        case PGRES_POLLING_FAILED:
        default: {
            std::string msg = PQerrorMessage(conn_);
            connecting_ = false;
            handleConnectionLost("connect failed: " + msg);
            return;
        }
    }
}

void PgClient::afterReady() {
    PQsetnonblocking(conn_, 1);
    connected_ = true;
    reconnect_attempt_ = 0;
    ensureChannel();
    updateChannel(true, false);
    if (on_ready_) {
        auto cb = std::move(on_ready_);
        cb({});
    }
}

void PgClient::updateChannel(bool reading, bool writing) {
    if (!channel_) return;
    if (reading) channel_->enableReading();
    if (writing) {
        channel_->enableWriting();
    } else {
        channel_->disableWriting();
    }
}

void PgClient::execute(std::string_view stmt_name, PgParams params, PgQueryCallback cb) {
    loop_->assertInLoopThread();
    if (!connected_) {
        if (cb) cb(std::unexpected(PgError{"not connected", "", true}));
        return;
    }
    pushQuery(std::string(stmt_name), std::move(params), std::move(cb), false, nullptr);
    sendFlush();
    consumeResults();
}

void PgClient::pipeline(std::vector<PgCall> calls, std::function<void(std::expected<void, PgError>)> done) {
    loop_->assertInLoopThread();
    if (!connected_) {
        if (done) done(std::unexpected(PgError{"not connected", "", true}));
        return;
    }
    if (calls.empty()) {
        if (done) done({});
        return;
    }
    if (PQenterPipelineMode(conn_) == 0) {
        if (done) done(std::unexpected(PgError{"enterPipelineMode failed", "", false}));
        return;
    }
    in_pipeline_ = true;
    auto batch = std::make_shared<BatchContext>();
    for (auto& call : calls) {
        pushQuery(call.stmt_name, std::move(call.params), std::move(call.cb), false, nullptr);
        if (!queue_.empty() && queue_.back()) queue_.back()->batch = batch;
    }
    auto marker = std::make_unique<QueryState>();
    marker->batch_end_marker = true;
    marker->batch = batch;
    marker->batch_done = [this, done = std::move(done)](std::expected<void, PgError> r) {
        if (in_pipeline_) {
            PQexitPipelineMode(conn_);
            in_pipeline_ = false;
        }
        if (done) done(std::move(r));
    };
    if (PQpipelineSync(conn_) == 0) {
        queue_.pop_back();
        in_pipeline_ = false;
        done(std::unexpected(PgError{"pipelineSync failed", "", true}));
        handleConnectionLost("pipelineSync failed");
        return;
    }
    queue_.push_back(std::move(marker));
    sendFlush();
    consumeResults();
}

void PgClient::rawQuery(std::string sql, PgQueryCallback cb) {
    loop_->assertInLoopThread();
    if (!connected_) {
        if (cb) cb(std::unexpected(PgError{"not connected", "", true}));
        return;
    }
    pushRawQuery(sql, std::move(cb));
    sendFlush();
    consumeResults();
}

void PgClient::rawBatch(std::vector<std::string> sqls, std::function<void(std::expected<void, PgError>)> done) {
    loop_->assertInLoopThread();
    if (!connected_) {
        if (done) done(std::unexpected(PgError{"not connected", "", true}));
        return;
    }
    if (sqls.empty()) {
        if (done) done({});
        return;
    }
    if (PQenterPipelineMode(conn_) == 0) {
        if (done) done(std::unexpected(PgError{"enterPipelineMode failed", "", false}));
        return;
    }
    in_pipeline_ = true;
    auto batch = std::make_shared<BatchContext>();
    for (auto& sql : sqls) {
        pushRawQuery(sql, {});
        if (!queue_.empty() && queue_.back()) queue_.back()->batch = batch;
    }
    auto marker = std::make_unique<QueryState>();
    marker->batch_end_marker = true;
    marker->batch = batch;
    marker->batch_done = [this, done = std::move(done)](std::expected<void, PgError> r) {
        if (in_pipeline_) {
            PQexitPipelineMode(conn_);
            in_pipeline_ = false;
        }
        if (done) done(std::move(r));
    };
    if (PQpipelineSync(conn_) == 0) {
        queue_.pop_back();
        in_pipeline_ = false;
        done(std::unexpected(PgError{"pipelineSync failed", "", true}));
        handleConnectionLost("pipelineSync failed");
        return;
    }
    queue_.push_back(std::move(marker));
    sendFlush();
    consumeResults();
}

void PgClient::pushRawQuery(const std::string& sql, PgQueryCallback cb) {
    auto state = std::make_unique<QueryState>();
    state->cb = std::move(cb);
    // pipeline 模式下 PQsendQuery 不被允许，原生语句走无参数的扩展协议（文本结果）
    if (PQsendQueryParams(conn_, sql.c_str(), 0, nullptr, nullptr, nullptr, nullptr, 0) == 0) {
        state->failed = true;
        state->error = PgError{PQerrorMessage(conn_), "", true};
        if (state->cb) state->cb(std::unexpected(state->error));
        return;
    }
    queue_.push_back(std::move(state));
}

void PgClient::pushQuery(const std::string& stmt_name, const PgParams& params, PgQueryCallback cb,
                         bool batch_end_marker, std::function<void(std::expected<void, PgError>)> batch_done) {
    auto state = std::make_unique<QueryState>();
    state->cb = std::move(cb);
    state->batch_end_marker = batch_end_marker;
    state->batch_done = std::move(batch_done);

    if (!batch_end_marker) {
        const PgPreparedStatement* found = nullptr;
        for (const auto& s : statements_) {
            if (s.name == stmt_name) {
                found = &s;
                break;
            }
        }
        if (found == nullptr) {
            state->failed = true;
            state->error = PgError{"unknown prepared statement: " + stmt_name, "", false};
            if (state->cb) state->cb(std::unexpected(state->error));
            return;  // 不入队（已经失败回调）
        }
        std::vector<const char*> values;
        values.reserve(params.size());
        for (const auto& p : params) {
            values.push_back(p ? p->c_str() : nullptr);
        }
        if (PQsendQueryPrepared(conn_, found->name.c_str(), static_cast<int>(values.size()), values.data(), nullptr,
                                nullptr, 0) == 0) {
            state->failed = true;
            state->error = PgError{PQerrorMessage(conn_), "", true};
            if (state->cb) state->cb(std::unexpected(state->error));
            return;
        }
    }
    queue_.push_back(std::move(state));
}

void PgClient::sendFlush() {
    if (conn_ == nullptr) return;
    int flush = PQflush(conn_);
    if (flush == 1) {
        updateChannel(true, true);  // 没写完：关注可写
    } else if (flush == 0) {
        updateChannel(true, false);
    } else {
        handleConnectionLost("PQflush failed");
    }
}

void PgClient::onWritable() {
    if (!connected_ && connecting_) {
        pollConnect();
        return;
    }
    sendFlush();
}

void PgClient::onReadable() {
    if (!connected_ && connecting_) {
        pollConnect();
        return;
    }
    if (PQconsumeInput(conn_) == 0) {
        handleConnectionLost("consumeInput failed");
        return;
    }
    consumeResults();
}

void PgClient::consumeResults() {
    loop_->assertInLoopThread();
    while (connected_) {
        if (PQisBusy(conn_)) return;  // 需要更多输入
        PGresult* res = PQgetResult(conn_);
        if (res == nullptr) {
            // 一条查询的结果结束
            if (!queue_.empty() && !queue_.front()->batch_end_marker) {
                finishFront();
                continue;
            }
            if (queue_.empty()) return;
            // marker 前出现 NULL：等 SYNC
            return;
        }
        ExecStatusType st = PQresultStatus(res);
        if (st == PGRES_PIPELINE_SYNC || st == PGRES_PIPELINE_ABORTED) {
            PQclear(res);
            if (!queue_.empty() && queue_.front()->batch_end_marker) {
                finishBatch(*queue_.front());
            }
            continue;
        }
        if (queue_.empty()) {
            PQclear(res);
            continue;
        }
        QueryState& state = *queue_.front();
        switch (st) {
            case PGRES_TUPLES_OK: {
                int rows = PQntuples(res);
                int cols = PQnfields(res);
                for (int r = 0; r < rows; ++r) {
                    PgRow row;
                    row.columns.resize(static_cast<std::size_t>(cols));
                    for (int c = 0; c < cols; ++c) {
                        if (PQgetisnull(res, r, c)) continue;
                        row.columns[static_cast<std::size_t>(c)] = std::string(PQgetvalue(res, r, c));
                    }
                    state.acc.rows.push_back(std::move(row));
                }
                break;
            }
            case PGRES_COMMAND_OK:
            case PGRES_EMPTY_QUERY:
                state.acc.status = PQcmdStatus(res);
                break;
            case PGRES_FATAL_ERROR:
            case PGRES_BAD_RESPONSE: {
                if (!state.failed) {
                    state.failed = true;
                    const char* sqlstate = PQresultErrorField(res, PG_DIAG_SQLSTATE);
                    state.error = PgError{PQresultErrorMessage(res), sqlstate ? sqlstate : "",
                                          st == PGRES_BAD_RESPONSE};
                    if (state.batch) {
                        state.batch->failed = true;
                        if (state.batch->first_error.empty()) state.batch->first_error = state.error.message;
                    }
                }
                break;
            }
            case PGRES_NONFATAL_ERROR:
                break;  // NOTICE/WARNING：忽略
            default:
                break;
        }
        PQclear(res);
    }
}

void PgClient::finishFront() {
    auto state = std::move(queue_.front());
    queue_.pop_front();
    if (state->cb) {
        if (state->failed) state->cb(std::unexpected(state->error));
        else state->cb(std::move(state->acc));
    }
}

void PgClient::finishBatch(QueryState& marker) {
    auto batch = marker.batch;
    auto done = std::move(marker.batch_done);
    queue_.pop_front();
    if (done) {
        if (batch && batch->failed) {
            done(std::unexpected(PgError{batch->first_error, "", false}));
        } else {
            done({});
        }
    }
}

void PgClient::handleConnectionLost(const std::string& reason) {
    loop_->assertInLoopThread();
    bool was_connected = connected_;
    connected_ = false;
    connecting_ = false;
    in_pipeline_ = false;
    if (channel_) {
        channel_->disableAll();
        if (channel_registered_) {
            channel_->remove();
            channel_registered_ = false;
        }
    }
    auto queue = std::move(queue_);
    queue_.clear();
    // 未完成的请求全部失败回调
    for (auto& state : queue) {
        if (state->batch_end_marker) {
            if (state->batch_done) state->batch_done(std::unexpected(PgError{reason, "", true}));
            continue;
        }
        if (state->cb) state->cb(std::unexpected(PgError{reason, "", true}));
    }
    if (on_ready_) {
        // 首次连接就失败：只回调一次，之后静默重连
        auto cb = std::move(on_ready_);
        cb(std::unexpected(PgError{reason, "", true}));
    }
    if (conn_ != nullptr) {
        PQfinish(conn_);
        conn_ = nullptr;
    }
    SPDLOG_WARN("pg connection lost ({}): {}", was_connected ? "was connected" : "connecting", reason);
    scheduleReconnect();
}

void PgClient::scheduleReconnect() {
    if (reconnect_attempt_ > kMaxBackoffAttempt) reconnect_attempt_ = kMaxBackoffAttempt;
    auto delay = std::chrono::milliseconds(100 * (1 << reconnect_attempt_));
    if (delay > std::chrono::seconds(15)) delay = std::chrono::seconds(15);
    ++reconnect_attempt_;
    auto weak = weak_from_this();
    loop_->runAfter(delay, [weak] {
        if (auto self = weak.lock()) {
            self->on_ready_ = [weak](std::expected<void, PgError> r) {
                // 重连成功（on_ready_ 只用于首次，这里重连后 re-prepare 用）
                (void)r;
            };
            auto stmts = self->statements_;
            // 先连接（on_ready_ 占位），连接成功后重新预编译
            self->on_ready_ = [weak, stmts](std::expected<void, PgError> r) mutable {
                if (!r || !weak.lock() || stmts.empty()) return;
                weak.lock()->prepare(std::move(stmts), {});
            };
            self->conn_ = PQconnectStart(self->dsn_.c_str());
            if (self->conn_ == nullptr || PQstatus(self->conn_) == CONNECTION_BAD) {
                self->handleConnectionLost("PQconnectStart failed on reconnect");
                return;
            }
            self->connecting_ = true;
            self->ensureChannel();
            self->pollConnect();
        }
    });
}

}  // namespace gw::store
