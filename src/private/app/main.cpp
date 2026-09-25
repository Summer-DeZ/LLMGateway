#include <signal.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <CLI/CLI.hpp>
#include <chrono>
#include <cstdio>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <utility>

#include "base/log.hpp"
#include "base/random.hpp"
#include "gateway/access_log.hpp"
#include "gateway/admin.hpp"
#include "gateway/circuit_breaker.hpp"
#include "gateway/config.hpp"
#include "gateway/context.hpp"
#include "gateway/ledger.hpp"
#include "gateway/metrics.hpp"
#include "gateway/quota.hpp"
#include "gateway/rate_limiter.hpp"
#include "gateway/response_cache.hpp"
#include "gateway/router.hpp"
#include "gateway/token_bucket.hpp"
#include "gateway/usage_api.hpp"
#include "http/http_server.hpp"
#include "http/response_writer.hpp"
#include "http/upstream_pool.hpp"
#include "net/channel.hpp"
#include "net/event_loop.hpp"
#include "net/event_loop_thread.hpp"
#include "net/inet_address.hpp"
#include "net/resolver.hpp"
#include "store/pg_client.hpp"
#include "store/redis_client.hpp"

using namespace std::chrono_literals;

int main(int argc, char** argv) {
    CLI::App app{"llm-gateway"};
    std::string config_path;
    int io_threads = 0;
    app.add_option("-c,--config", config_path, "config file path")->required();
    app.add_option("--io-threads", io_threads, "override server.io_threads");
    CLI11_PARSE(app, argc, argv);

    // 必须在创建任何线程（spdlog 线程池、io 线程）之前屏蔽信号
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    ::sigprocmask(SIG_BLOCK, &mask, nullptr);

    auto loaded = gw::gateway::Config::load(config_path);
    if (!loaded) {
        std::fprintf(stderr, "config error: %s\n", loaded.error().c_str());
        return 1;
    }
    std::shared_ptr<gw::gateway::Config> config = *loaded;
    if (io_threads > 0) config->server.io_threads = io_threads;

    gw::log::init(config->log.level, config->log.file, config->log.access_file);

    auto listen = gw::gateway::parseListenAddress(config->server.listen);
    if (!listen) {
        std::fprintf(stderr, "invalid listen address: %s\n", config->server.listen.c_str());
        return 1;
    }

    gw::net::EventLoop loop;
    gw::gateway::GatewayContext ctx{config};

    gw::net::Resolver resolver(std::chrono::seconds(config->dns.cache_ttl_seconds));
    ctx.resolver = &resolver;

    gw::gateway::Metrics metrics(*config);
    gw::gateway::AccessLog access_log;
    gw::gateway::RecentRequests recent;
    gw::gateway::SteadyClock clock;
    gw::gateway::RateLimiter rate_limiter(*config, clock);
    gw::gateway::QuotaManager quota_manager(*config, &metrics);
    gw::gateway::CircuitBreakerSet breakers(*config, clock);
    gw::gateway::ResponseCache response_cache(config->cache);
    breakers.setStateCallback([&metrics](const std::string& upstream, gw::gateway::CircuitBreaker::State state) {
        int value = state == gw::gateway::CircuitBreaker::State::open
                        ? 1
                        : (state == gw::gateway::CircuitBreaker::State::half_open ? 2 : 0);
        metrics.setCircuitState(upstream, value);
        SPDLOG_WARN("circuit breaker {} -> {}", upstream, gw::gateway::CircuitBreaker::stateName(state));
    });
    ctx.metrics = &metrics;
    ctx.access_log = &access_log;
    ctx.recent = &recent;
    ctx.rate_limiter = &rate_limiter;
    ctx.quota_manager = &quota_manager;
    ctx.breakers = &breakers;
    ctx.response_cache = &response_cache;

    // 每 io loop 一个 UpstreamPool（+ 可选 RedisClient），登记下来供主循环刷新空闲连接指标
    std::mutex pools_mu;
    std::vector<std::pair<gw::net::EventLoop*, gw::http::UpstreamPool*>> pools;

    std::shared_ptr<gw::store::PgClient> pg_write, pg_read;
    std::shared_ptr<gw::gateway::LedgerWriter> ledger_writer;
    std::shared_ptr<gw::store::RedisClient> ledger_redis;
    std::unique_ptr<gw::net::EventLoopThread> ledger_thread;
    std::unique_ptr<gw::gateway::UsageApi> usage_api;
    gw::net::EventLoop* ledger_loop_ptr = nullptr;

    gw::http::HttpServerOptions options;
    options.max_body_bytes = config->server.max_body_bytes;
    options.high_water_bytes = config->stream.high_water_bytes;
    options.header_timeout = config->server.header_timeout;
    options.body_timeout = config->server.body_timeout;
    options.keepalive_idle = config->server.keepalive_idle;
    options.downstream_sndbuf_bytes = config->stream.downstream_sndbuf_bytes;

    gw::http::HttpServer server(&loop, gw::net::InetAddress(listen->first, listen->second), "gateway", options);
    server.setThreadNum(config->server.io_threads);
    server.setThreadInitCallback([&config, &resolver, &quota_manager, &pools_mu, &pools](gw::net::EventLoop* io_loop) {
        gw::http::PoolOptions pool_options;
        pool_options.idle_timeout = std::chrono::seconds(4);
        pool_options.idle_timeout_for = [config](const gw::http::UpstreamKey& key) {
            for (const auto& [name, upstream] : config->upstreams) {
                if (upstream.url.host == key.host && upstream.url.port == key.port) {
                    return std::chrono::seconds(upstream.pool_idle_seconds);
                }
            }
            return std::chrono::seconds(4);
        };
        auto pool = std::make_shared<gw::http::UpstreamPool>(io_loop, &resolver, pool_options);
        std::shared_ptr<gw::store::RedisClient> redis;
        if (config->redis) {
            gw::store::RedisOptions opts;
            opts.host = config->redis->host;
            opts.port = config->redis->port;
            opts.password = config->redis->password;
            opts.command_timeout = config->redis->command_timeout;
            redis = std::make_shared<gw::store::RedisClient>(io_loop, resolver, opts);
            redis->connect();
        }
        {
            std::scoped_lock lock(pools_mu);
            pools.emplace_back(io_loop, pool.get());
        }
        io_loop->context() = std::make_shared<gw::gateway::IoLoopServices>(
            gw::gateway::IoLoopServices{std::move(pool), std::move(redis)});
    });

    // ===== 账本与配额（配置了 postgres 段才启动；PG/Redis 启动连接失败 → 非零退出）=====
    if (config->postgres) {
        ledger_thread = std::make_unique<gw::net::EventLoopThread>(nullptr, "ledger-loop");
        gw::net::EventLoop* ledger_loop = ledger_thread->startLoop();
        ledger_loop_ptr = ledger_loop;
        pg_write = std::make_shared<gw::store::PgClient>(ledger_loop, config->postgres->dsn,
                                                         config->postgres->connect_timeout);
        pg_read = std::make_shared<gw::store::PgClient>(ledger_loop, config->postgres->dsn,
                                                        config->postgres->connect_timeout);
        ledger_writer = std::make_shared<gw::gateway::LedgerWriter>(ledger_loop, pg_write, config->ledger,
                                                                    config->instance_id, &metrics);
        ctx.ledger_writer = ledger_writer.get();
        if (config->redis) {
            gw::store::RedisOptions opts;
            opts.host = config->redis->host;
            opts.port = config->redis->port;
            opts.password = config->redis->password;
            opts.command_timeout = config->redis->command_timeout;
            ledger_redis = std::make_shared<gw::store::RedisClient>(ledger_loop, resolver, opts);
            ledger_loop->queueInLoop([r = ledger_redis.get()] { r->connect(); });
        }
        usage_api = std::make_unique<gw::gateway::UsageApi>(ctx, server, ledger_loop, pg_read, &quota_manager);

        auto fail_startup = [](const std::string& reason) {
            SPDLOG_CRITICAL("startup failed: {}", reason);
            gw::log::shutdown();
            std::exit(1);
        };
        auto on_pg_ready = [&pg_write, &pg_read, &ledger_writer, &ledger_redis, &quota_manager, &config,
                            fail_startup](std::expected<void, gw::store::PgError> r) {
            if (!r) fail_startup("pg connect failed: " + r.error().message);
            // 迁移（advisory lock 保证多实例只有一个执行）
            gw::gateway::runMigrations(*pg_write, [&](std::expected<void, std::string> mr) {
                if (!mr) fail_startup("migration failed: " + mr.error());
                SPDLOG_INFO("ledger: schema ready");
                pg_write->prepare(gw::gateway::ledgerWriteStatements(),
                                  [fail_startup](std::expected<void, gw::store::PgError> r) {
                                      if (!r) fail_startup("pg write prepare failed: " + r.error().message);
                                  });
                ledger_writer->start();
                // 配额启动重建：对每个配置了 quota 的 key，SET NX 当日/当月 PG 汇总
                if (!ledger_redis || !config->redis) return;
                auto users = std::make_shared<std::vector<const gw::gateway::UserConfig*>>();
                for (const auto& [k, u] : config->users) {
                    if (u.quota) users->push_back(&u);
                }
                auto rebuild_next = std::make_shared<std::function<void(std::size_t)>>();
                *rebuild_next = [rebuild_next, users, &pg_read, &ledger_redis, &quota_manager, &config](
                                    std::size_t i) {
                    if (i >= users->size()) {
                        SPDLOG_INFO("ledger: quota rebuild done ({} keys)", users->size());
                        return;
                    }
                    const auto* user = (*users)[i];
                    const auto& qc = config->quota;
                    int64_t now = gw::wallMillis();
                    int64_t day = gw::gateway::quotaDayOf(now, qc.utc_offset_minutes);
                    int64_t month = gw::gateway::quotaMonthOf(now, qc.utc_offset_minutes);
                    std::string billing_day = gw::gateway::quotaBillingDay(now, qc.utc_offset_minutes);
                    pg_read->execute("usage_day_sum", {user->name, billing_day},
                                     [rebuild_next, users, i, user, &pg_read, &ledger_redis, &quota_manager, day,
                                      month, day_ttl = gw::gateway::kQuotaDayTtlSeconds,
                                      month_ttl = gw::gateway::kQuotaMonthTtlSeconds](
                                         std::expected<gw::store::PgResult, gw::store::PgError> d) {
                                         int64_t used_day = 0;
                                         if (d && !d->rows.empty()) {
                                             used_day = std::llround(std::stod(d->rows[0].columns[0].value_or("0")));
                                         }
                                         char month_first[16];
                                         std::snprintf(month_first, sizeof(month_first), "%04lld-%02lld-01",
                                                       static_cast<long long>(month / 100),
                                                       static_cast<long long>(month % 100));
                                         pg_read->execute(
                                             "usage_month_sum", {user->name, month_first},
                                             [rebuild_next, users, i, user, &ledger_redis, &quota_manager, day,
                                              month, used_day, day_ttl, month_ttl](
                                                 std::expected<gw::store::PgResult, gw::store::PgError> m) {
                                                 int64_t used_month = 0;
                                                 if (m && !m->rows.empty()) {
                                                     used_month = std::llround(
                                                         std::stod(m->rows[0].columns[0].value_or("0")));
                                                 }
                                                 // SET NX EX：不覆盖其他实例正在累加的计数
                                                 auto* redis = ledger_redis.get();
                                                 redis->command(
                                                     {"SET", gw::gateway::quotaDayKey(user->name, day),
                                                      std::to_string(used_day), "NX", "EX",
                                                      std::to_string(day_ttl)},
                                                     [rebuild_next, users, i, user, redis, month, used_month,
                                                      month_ttl](gw::store::RedisClient::Reply) {
                                                         redis->command(
                                                             {"SET",
                                                              gw::gateway::quotaMonthKey(user->name, month),
                                                              std::to_string(used_month), "NX", "EX",
                                                              std::to_string(month_ttl)},
                                                             [rebuild_next, users, i, user](
                                                                 gw::store::RedisClient::Reply) {
                                                                 SPDLOG_INFO("quota rebuild {} (#{})", user->name, i);
                                                                 (*rebuild_next)(i + 1);
                                                             });
                                                     });
                                             });
                                     });
                };
                // 读连接在迁移之后连接 + 预编译（语句引用 usage_* 表）；就绪后再做配额重建
                pg_read->connect([&pg_read, fail_startup, rebuild_next](std::expected<void, gw::store::PgError> r) {
                    if (!r) fail_startup("pg read connect failed: " + r.error().message);
                    pg_read->prepare(gw::gateway::ledgerReadStatements(),
                                     [fail_startup, rebuild_next](std::expected<void, gw::store::PgError> rr) {
                                         if (!rr) fail_startup("pg read prepare failed: " + rr.error().message);
                                         SPDLOG_INFO("ledger: read connection ready");
                                         (*rebuild_next)(0);
                                     });
                });
            });
        };
        ledger_loop->queueInLoop([&pg_write, on_pg_ready] { pg_write->connect(on_pg_ready); });
    }

    gw::gateway::Router router(ctx, server, usage_api.get());
    server.setRequestHandler(
        [&router](const gw::http::HttpRequest& request, const gw::net::TcpConnectionPtr& conn) {
            router.handle(request, conn);
        });
    server.setCloseHandler([&router](const gw::net::TcpConnectionPtr& conn) { router.onConnectionClosed(conn); });

    server.start();
    SPDLOG_INFO("llm-gateway listening on {} ({} io threads), upstreams={} models={}", server.listenAddress(),
                config->server.io_threads, config->upstreams.size(), config->models.size());

    // 主循环每 30 秒：清扫缓存过期项并刷新 gauge
    loop.runEvery(30s, [&response_cache, &metrics] {
        response_cache.sweep();
        auto [bytes, entries] = response_cache.stats();
        metrics.setCacheGauges(bytes, entries);
    });

    // 主循环每秒：刷新账本积压 + 汇总各 loop 连接池的空闲连接数
    loop.runEvery(1s, [&config, &metrics, &pools_mu, &pools, &ledger_writer] {
        if (ledger_writer) metrics.setLedgerBacklog(ledger_writer->backlog());
        std::vector<std::pair<gw::net::EventLoop*, gw::http::UpstreamPool*>> snapshot;
        {
            std::scoped_lock lock(pools_mu);
            snapshot = pools;
        }
        for (const auto& [name, upstream] : config->upstreams) {
            (void)name;
            // 池按 host:port 组织；共享同一地址的上游（如 qwen36 / qwen36-final）报告同一个池计数
            std::size_t total = 0;
            for (auto& [loop_ptr, pool] : snapshot) {
                (void)loop_ptr;
                if (pool->servesKey({upstream.url.host, upstream.url.port})) total += pool->idleCount();
            }
            metrics.setUpstreamIdle(upstream.name, total);
        }
    });

    int sig_fd = ::signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    gw::net::Channel sig_channel(&loop, sig_fd);
    sig_channel.setReadCallback([&] {
        struct signalfd_siginfo si;
        ssize_t n = ::read(sig_fd, &si, sizeof(si));
        (void)n;
        if (ctx.draining.exchange(true)) return;
        SPDLOG_INFO("shutdown signal received, draining (max {}s)", config->shutdown.drain_seconds);
        server.stopAccepting();
        server.drain();

        // 活跃会话归零后退出；loop 退出后定时器随之失效，不需要取消
        loop.runEvery(50ms, [&loop, &ctx] {
            if (ctx.active_sessions.load() == 0) loop.quit();
        });
        loop.runAfter(std::chrono::seconds(config->shutdown.drain_seconds), [&loop, &ctx, &router] {
            int active = ctx.active_sessions.load();
            if (active > 0) {
                SPDLOG_WARN("drain timeout, cancelling {} active sessions", active);
                router.cancelAll("gateway shutdown");
            }
            // 取消在各 io loop 上异步完成，归零后由上面的轮询退出；这里兜底
            loop.runAfter(5s, [&loop] { loop.quit(); });
        });
    });
    sig_channel.enableReading();

    loop.loop();

    // 先停 io 线程（剩余连接的 connectDestroyed 会回调 Router），再让 Router 等对象析构
    server.stop();
    sig_channel.disableAll();
    sig_channel.remove();
    ::close(sig_fd);

    // 账本：退出前把队列写完（最长 drain_seconds），再停 ledger loop。
    // 账本相关对象（PgClient/RedisClient/LedgerWriter/UsageApi）持有 ledger loop 的 Channel 与 Timer，
    // 必须在 loop 线程上、loop 退出之前销毁，否则会访问已析构的 EventLoop
    if (ledger_writer) ledger_writer->flushAndStop(config->shutdown.drain_seconds);
    if (ledger_loop_ptr != nullptr) {
        std::promise<void> cleaned;
        auto cleanup_done = cleaned.get_future();
        ledger_loop_ptr->queueInLoop([&] {
            usage_api.reset();
            ledger_writer.reset();
            ledger_redis.reset();
            pg_read.reset();
            pg_write.reset();
            cleaned.set_value();
        });
        cleanup_done.wait();
        ledger_thread.reset();
    }

    SPDLOG_INFO("gateway stopped");
    gw::log::shutdown();
    return 0;
}
