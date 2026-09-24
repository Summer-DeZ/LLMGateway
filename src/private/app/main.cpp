#include <signal.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <CLI/CLI.hpp>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>

#include "base/log.hpp"
#include "base/random.hpp"
#include "gateway/config.hpp"
#include "gateway/context.hpp"
#include "gateway/router.hpp"
#include "http/http_server.hpp"
#include "http/response_writer.hpp"
#include "http/upstream_pool.hpp"
#include "net/channel.hpp"
#include "net/event_loop.hpp"
#include "net/inet_address.hpp"
#include "net/resolver.hpp"
#include "net/tls.hpp"

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
    std::unique_ptr<gw::net::TlsContext> tls_context;
    {
        auto created = gw::net::TlsContext::create(config->tls.verify_peer, config->tls.ca_file);
        if (!created) {
            std::fprintf(stderr, "tls init failed: %s\n", created.error().c_str());
            return 1;
        }
        tls_context = std::move(*created);
    }
    ctx.resolver = &resolver;
    ctx.tls_context = tls_context.get();

    gw::http::HttpServerOptions options;
    options.max_body_bytes = config->server.max_body_bytes;
    options.high_water_bytes = config->stream.high_water_bytes;
    options.header_timeout = config->server.header_timeout;
    options.body_timeout = config->server.body_timeout;
    options.keepalive_idle = config->server.keepalive_idle;

    gw::http::HttpServer server(&loop, gw::net::InetAddress(listen->first, listen->second), "gateway", options);
    server.setThreadNum(config->server.io_threads);
    server.setThreadInitCallback([&config, tls = tls_context.get(), &resolver](gw::net::EventLoop* io_loop) {
        gw::http::PoolOptions pool_options;
        pool_options.idle_timeout = std::chrono::seconds(4);
        pool_options.idle_timeout_for = [config](const gw::http::UpstreamKey& key) {
            for (const auto& [name, upstream] : config->upstreams) {
                if (upstream.url.host == key.host && upstream.url.port == key.port && upstream.url.tls == key.tls) {
                    return std::chrono::seconds(upstream.pool_idle_seconds);
                }
            }
            return std::chrono::seconds(4);
        };
        io_loop->context() = std::make_shared<gw::http::UpstreamPool>(io_loop, &resolver, tls, pool_options);
    });

    gw::gateway::Router router(ctx, server);
    server.setRequestHandler(
        [&router](const gw::http::HttpRequest& request, const gw::net::TcpConnectionPtr& conn) {
            router.handle(request, conn);
        });
    server.setCloseHandler([&router](const gw::net::TcpConnectionPtr& conn) { router.onConnectionClosed(conn); });

    server.start();
    SPDLOG_INFO("llm-gateway listening on {} ({} io threads), upstreams={} models={}", server.listenAddress(),
                config->server.io_threads, config->upstreams.size(), config->models.size());

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

        auto checker = std::make_shared<std::function<void()>>();
        std::weak_ptr<std::function<void()>> weak_checker = checker;
        *checker = [&loop, &ctx, weak_checker] {
            if (ctx.active_sessions.load() == 0) {
                loop.quit();
                return;
            }
            if (auto fn = weak_checker.lock()) loop.runAfter(50ms, *fn);
        };
        loop.runAfter(50ms, *checker);
        loop.runAfter(std::chrono::seconds(config->shutdown.drain_seconds), [&loop, &ctx] {
            if (ctx.active_sessions.load() > 0) {
                SPDLOG_WARN("drain timeout, forcing shutdown with {} active sessions", ctx.active_sessions.load());
            }
            loop.quit();
        });
    });
    sig_channel.enableReading();

    loop.loop();

    sig_channel.disableAll();
    sig_channel.remove();
    ::close(sig_fd);

    SPDLOG_INFO("gateway stopped");
    gw::log::shutdown();
    return 0;
}
