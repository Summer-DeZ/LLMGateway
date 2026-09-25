#include "gateway/management.hpp"

#include <charconv>
#include <chrono>
#include <regex>
#include <stdexcept>

#include "gateway/pricing.hpp"
#include "gateway/quota.hpp"
#include "gw_management_embedded.h"
#include "http/response_writer.hpp"

namespace gw::gateway {
namespace {
using Json = nlohmann::json;
int digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
std::string decode(std::string_view s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%') {
            if (i + 2 >= s.size() || digit(s[i + 1]) < 0 || digit(s[i + 2]) < 0)
                throw std::invalid_argument("invalid encoding");
            out.push_back(static_cast<char>(digit(s[i + 1]) * 16 + digit(s[i + 2])));
            i += 2;
        } else
            out.push_back(s[i] == '+' ? ' ' : s[i]);
    }
    return out;
}
std::string cursorEncode(const Json& value) {
    const auto s = value.dump();
    static constexpr char chars[] = "0123456789abcdef";
    std::string out;
    for (unsigned char c : s) {
        out.push_back(chars[c >> 4]);
        out.push_back(chars[c & 15]);
    }
    return out;
}
Json cursorDecode(const std::string& s, const std::string& scope) {
    if (s.size() > 2048 || s.size() % 2) throw std::invalid_argument("invalid cursor");
    std::string out;
    for (std::size_t i = 0; i < s.size(); i += 2) {
        if (digit(s[i]) < 0 || digit(s[i + 1]) < 0) throw std::invalid_argument("invalid cursor");
        out.push_back(static_cast<char>(digit(s[i]) * 16 + digit(s[i + 1])));
    }
    auto j = Json::parse(out);
    static const std::regex timestamp(
        R"(^[0-9]{4}-[0-9]{2}-[0-9]{2}[T ][0-9]{2}:[0-9]{2}:[0-9]{2}(\.[0-9]{1,6})?\+00(:00)?$)");
    if (!j.is_array() || j.size() != 3 || j.at(0) != scope || !j.at(1).is_string() || !j.at(2).is_string() ||
        !std::regex_match(j.at(1).get<std::string>(), timestamp))
        throw std::invalid_argument("invalid cursor");
    const auto ts = j.at(1).get<std::string>();
    managementDates(ts.substr(0, 10), ts.substr(0, 10));
    if (ts.substr(11, 2) > "23" || ts.substr(14, 2) > "59" || ts.substr(17, 2) > "59")
        throw std::invalid_argument("invalid cursor time");
    const auto id = j.at(2).get<std::string>();
    if (scope == "keys" ? !managementId(id)
                        : (id.size() != 32 || id.find_first_not_of("0123456789abcdef") != std::string::npos))
        throw std::invalid_argument("invalid cursor");
    return j;
}
std::chrono::sys_days date(std::string_view s) {
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') throw std::invalid_argument("invalid date");
    for (std::size_t i = 0; i < s.size(); ++i)
        if (i != 4 && i != 7 && (s[i] < '0' || s[i] > '9')) throw std::invalid_argument("invalid date");
    int y = 0, m = 0, d = 0;
    std::from_chars(s.data(), s.data() + 4, y);
    std::from_chars(s.data() + 5, s.data() + 7, m);
    std::from_chars(s.data() + 8, s.data() + 10, d);
    std::chrono::year_month_day v{std::chrono::year(y), std::chrono::month(static_cast<unsigned>(m)),
                                  std::chrono::day(static_cast<unsigned>(d))};
    if (y < 1 || !v.ok()) throw std::invalid_argument("invalid date");
    return std::chrono::sys_days(v);
}
http::Headers headers() {
    return {{"cache-control", "no-store"},
            {"x-content-type-options", "nosniff"},
            {"x-frame-options", "DENY"},
            {"referrer-policy", "no-referrer"},
            {"content-security-policy",
             "default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self'; img-src 'self' "
             "data:; frame-ancestors 'none'; base-uri 'none'; form-action 'self'"}};
}
Json error(std::string code, std::string message) {
    return {{"error", {{"code", code}, {"message", message}}}};
}
}  // namespace

std::string managementName(const Json& body) {
    if (!body.is_object() || body.size() != 1 || !body.contains("name") || !body["name"].is_string())
        throw std::invalid_argument("name required");
    auto s = body["name"].get<std::string>();
    auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) throw std::invalid_argument("empty name");
    s = s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
    std::size_t count = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        auto c = static_cast<unsigned char>(s[i]);
        if (c < 32 || c == 127 ||
            (c == 0xc2 && i + 1 < s.size() && static_cast<unsigned char>(s[i + 1]) < 0xa0))
            throw std::invalid_argument("control character in name");
        if ((c & 0xc0) != 0x80) ++count;
    }
    if (count < 1 || count > 64) throw std::invalid_argument("name must have 1–64 characters");
    return s;
}
std::map<std::string, std::string> managementQuery(std::string_view query) {
    if (query.size() > 4096) throw std::invalid_argument("query too long");
    std::map<std::string, std::string> out;
    while (!query.empty()) {
        auto end = query.find('&');
        auto part = query.substr(0, end);
        auto eq = part.find('=');
        if (eq == std::string_view::npos || eq == 0) throw std::invalid_argument("invalid query");
        if (!out.emplace(decode(part.substr(0, eq)), decode(part.substr(eq + 1))).second)
            throw std::invalid_argument("duplicate parameter");
        if (end == std::string_view::npos) break;
        query.remove_prefix(end + 1);
    }
    return out;
}
bool managementId(std::string_view id) {
    if (id.size() != 36) return false;
    for (std::size_t i = 0; i < id.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (id[i] != '-') return false;
        } else if (digit(id[i]) < 0)
            return false;
    }
    return true;
}
void managementDates(std::string_view start, std::string_view end) {
    auto a = date(start), b = date(end);
    if (a > b || (b - a).count() > 365) throw std::invalid_argument("date range must be 1–366 days");
}
bool managementSourceAllowed(const http::HttpRequest& r, std::string_view peer, std::uint16_t port) {
    if (peer != "127.0.0.1" && peer != "::1" && peer != "::ffff:127.0.0.1") return false;
    for (const auto field : {"host", "origin", "content-type", "x-gateway-management"}) {
        int count = 0;
        for (const auto& [name, value] : r.headers)
            if (name == field) ++count;
        if (count > 1) return false;
    }
    auto host = r.header("host");
    if (!host) return false;
    const auto suffix = ":" + std::to_string(port);
    if (*host != "localhost" + suffix && *host != "127.0.0.1" + suffix && *host != "[::1]" + suffix &&
        !(port == 80 && (*host == "localhost" || *host == "127.0.0.1" || *host == "[::1]")))
        return false;
    auto origin = r.header("origin");
    if (origin && *origin != "http://" + std::string(*host)) return false;
    if (auto site = r.header("sec-fetch-site"); site && *site == "cross-site") return false;
    if (r.method == "POST") {
        if (!origin || r.header("x-gateway-management") != "1") return false;
        auto type = r.header("content-type");
        if (!type || (*type != "application/json" && *type != "application/json; charset=utf-8"))
            return false;
    }
    return true;
}
ManagementApi::ManagementApi(GatewayContext& ctx, http::HttpServer& server, net::EventLoop* ledger_loop,
                             KeyStore& keys)
    : ctx_(ctx),
      server_(server),
      ledger_loop_(ledger_loop),
      keys_(keys),
      port_(parseListenAddress(ctx.config->server.listen)->second) {}
void ManagementApi::stopCompletions() {
    ledger_loop_->assertInLoopThread();
    stopping_ = true;
    keys_.stop();
}
void ManagementApi::reply(const net::TcpConnectionPtr& conn, int status, const Json& body) {
    http::ResponseWriter writer(conn, "", true);
    writer.writeFull(status, headers(), "application/json; charset=utf-8", body.dump());
    server_.finishRequest(conn);
}
void ManagementApi::query(const net::TcpConnectionPtr& conn, std::string stmt, store::PgParams params,
                          bool mutation,
                          std::function<void(const net::TcpConnectionPtr&, const Json&)> done) {
    if (pending.fetch_add(1) >= 128) {
        --pending;
        reply(conn, 503, error("unavailable", "管理请求繁忙，请稍后重试"));
        return;
    }
    auto* caller = conn->getLoop();
    auto weak = std::weak_ptr<net::TcpConnection>(conn);
    ledger_loop_->queueInLoop([this, caller, weak, stmt = std::move(stmt), params = std::move(params),
                               mutation, done = std::move(done)]() mutable {
        if (stopping_) {
            --pending;
            return;
        }
        keys_.execute(
            std::move(stmt), std::move(params), mutation,
            [this, caller, weak, done = std::move(done)](auto result) mutable {
                if (stopping_) {
                    --pending;
                    return;
                }
                caller->queueInLoop([this, weak, done = std::move(done), result = std::move(result)] {
                    if (auto c = weak.lock(); c && c->connected()) {
                        if (!result)
                            reply(c, 503, error("storage_unavailable", "存储暂不可用，请稍后刷新确认状态"));
                        else
                            try {
                                auto rows = Json::array();
                                for (const auto& row : result->rows)
                                    rows.push_back(Json::parse(*row.columns[0]));
                                done(c, rows);
                            } catch (...) {
                                reply(c, 500, error("internal_error", "无法处理管理请求"));
                            }
                    }
                    --pending;
                });
            });
    });
}
void ManagementApi::handle(const http::HttpRequest& r, const net::TcpConnectionPtr& conn) {
    if (!managementSourceAllowed(r, conn->peerAddress().toIp(), port_)) {
        reply(conn, 403, error("forbidden", "仅允许本机同源访问"));
        return;
    }
    if (r.path == "/manage" || r.path.starts_with("/manage/")) {
        if (r.method != "GET") {
            reply(conn, 405, error("method_not_allowed", "只支持 GET"));
            return;
        }
        std::string_view body, type;
        if (r.path == "/manage/" || r.path == "/manage" || r.path == "/manage/index.html") {
            body = kManagementHtml;
            type = "text/html; charset=utf-8";
        } else if (r.path == "/manage/app.js") {
            body = kManagementJs;
            type = "text/javascript; charset=utf-8";
        } else if (r.path == "/manage/style.css") {
            body = kManagementCss;
            type = "text/css; charset=utf-8";
        } else {
            reply(conn, 404, error("not_found", "页面不存在"));
            return;
        }
        http::ResponseWriter writer(conn, "", r.keepAlive());
        writer.writeFull(200, headers(), type, body);
        server_.finishRequest(conn);
        return;
    }
    if (ctx_.draining) {
        reply(conn, 503, error("unavailable", "网关正在退出"));
        return;
    }
    if (r.body.size() > 4096) {
        reply(conn, 413, error("body_too_large", "请求体过大"));
        return;
    }
    try {
        const std::string base = "/api/manage/keys";
        if (r.path != base && !r.path.starts_with(base + "/")) {
            reply(conn, 404, error("not_found", "接口不存在"));
            return;
        }
        auto q = managementQuery(r.query);
        const auto today = quotaBillingDay(wallMillis(), ctx_.config->quota.utc_offset_minutes);
        const auto offset = ctx_.config->quota.utc_offset_minutes;
        Json common = {{"as_of", formatWallMs(wallMillis())},
                       {"billing_timezone", {{"utc_offset_minutes", offset}}}};
        if (r.path == base && r.method == "POST") {
            if (!q.empty()) throw std::invalid_argument("unexpected query");
            auto name = managementName(Json::parse(r.body));
            auto secret = generateApiKey();
            auto id = generateKeyId();
            if (keys_.staticConflict(secret)) throw std::runtime_error("credential collision");
            store::PgParams params{id, name, keyDigest(secret), secret.substr(secret.size() - 4)};
            query(conn, "keys_create", std::move(params), true,
                  [this, secret = std::move(secret)](const auto& c, const Json& rows) {
                      reply(c, 201, {{"key", rows.at(0)}, {"secret", secret}});
                  });
            return;
        }
        std::string id, action;
        if (r.path != base) {
            auto rest = r.path.substr(base.size() + 1);
            auto slash = rest.find('/');
            if (slash == std::string::npos) {
                reply(conn, 404, error("not_found", "接口不存在"));
                return;
            }
            id = rest.substr(0, slash);
            action = rest.substr(slash + 1);
            if (!managementId(id)) throw std::invalid_argument("invalid key id");
        }
        if (action == "disable") {
            if (r.method != "POST") {
                reply(conn, 405, error("method_not_allowed", "只支持 POST"));
                return;
            }
            if (!q.empty() || Json::parse(r.body) != Json::object())
                throw std::invalid_argument("empty object required");
            query(conn, "keys_disable", {id}, true, [this](const auto& c, const Json& rows) {
                if (rows.empty())
                    reply(c, 404, error("not_found", "密钥不存在"));
                else
                    reply(c, 200, {{"key", rows[0]}});
            });
            return;
        }
        if (r.method != "GET") {
            reply(conn, 405, error("method_not_allowed", "只支持 GET"));
            return;
        }
        for (const auto& [key, value] : q)
            if (key != "limit" && key != "cursor" && (id.empty() || (key != "start" && key != "end")))
                throw std::invalid_argument("unknown parameter");
        int limit = 50;
        if (q.contains("limit")) {
            const auto& s = q.at("limit");
            auto parsed = std::from_chars(s.data(), s.data() + s.size(), limit);
            if (parsed.ec != std::errc() || parsed.ptr != s.data() + s.size() || limit < 1 || limit > 100)
                throw std::invalid_argument("invalid limit");
        }
        std::optional<std::string> before, before_id;
        auto start = q.contains("start") ? q.at("start") : today,
             end = q.contains("end") ? q.at("end") : start;
        managementDates(start, end);
        const auto scope = id.empty() ? "keys" : id + ":" + start + ":" + end;
        if (q.contains("cursor")) {
            auto v = cursorDecode(q.at("cursor"), scope);
            before = v[1].get<std::string>();
            before_id = v[2].get<std::string>();
        }
        if (id.empty()) {
            query(conn, "keys_list", {today, before, before_id, std::to_string(limit + 1)}, false,
                  [this, common, limit](const auto& c, Json rows) mutable {
                      Json cursor = nullptr;
                      if (rows.size() > static_cast<std::size_t>(limit)) {
                          rows.erase(rows.end() - 1);
                          const auto& last = rows.back();
                          cursor = cursorEncode(Json::array({"keys", last["created_at"], last["id"]}));
                      }
                      common["items"] = rows;
                      common["next_cursor"] = cursor;
                      reply(c, 200, common);
                  });
            return;
        }
        if (action != "usage" && action != "requests") {
            reply(conn, 404, error("not_found", "接口不存在"));
            return;
        }
        if (action == "usage" && (q.contains("limit") || q.contains("cursor")))
            throw std::invalid_argument("unexpected pagination");
        query(conn, "keys_get", {id}, false,
              [this, id, action, start, end, limit, before, before_id, scope, common](
                  const auto& c, const Json& keys) mutable {
                  if (keys.empty()) {
                      reply(c, 404, error("not_found", "密钥不存在"));
                      return;
                  }
                  common["key"] = keys[0];
                  common["start"] = start;
                  common["end"] = end;
                  if (action == "usage")
                      query(c, "keys_usage", {id, start, end}, false,
                            [this, common](const auto& c2, const Json& rows) mutable {
                                common["days"] = Json::array();
                                for (const auto& row : rows) {
                                    if (row["day"].is_null()) {
                                        common["total"] = row;
                                        common["total"].erase("day");
                                    } else
                                        common["days"].push_back(row);
                                }
                                reply(c2, 200, common);
                            });
                  else
                      query(c, "keys_requests",
                            {id, start, end, before, before_id, std::to_string(limit + 1)}, false,
                            [this, common, limit, scope](const auto& c2, Json rows) mutable {
                                Json cursor = nullptr;
                                if (rows.size() > static_cast<std::size_t>(limit)) {
                                    rows.erase(rows.end() - 1);
                                    const auto& last = rows.back();
                                    cursor = cursorEncode(
                                        Json::array({scope, last["finished_at"], last["ledger_id"]}));
                                }
                                common["requests"] = rows;
                                common["next_cursor"] = cursor;
                                reply(c2, 200, common);
                            });
              });
    } catch (const std::invalid_argument&) {
        reply(conn, 400, error("invalid_request", "请检查名称、日期范围或分页参数"));
    } catch (const Json::exception&) {
        reply(conn, 400, error("invalid_request", "请求格式不正确"));
    } catch (...) {
        reply(conn, 500, error("internal_error", "无法处理管理请求"));
    }
}
}  // namespace gw::gateway
