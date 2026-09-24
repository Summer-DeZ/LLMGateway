# net：Reactor、传输层、TLS 与上游连接

模块 `gw_net`（Resolver/Connector/TLS 也在这里），HTTP 相关的连接池在 `gw_http`。以下接口是**建议签名**：
语义锁定，命名可以调整。参考实现思路：陈硕《Linux 多线程服务端编程》与 muduo 源码（`muduo/net/`）。

## 1. 文件与类

| 头文件 | 类 | 要点 |
| --- | --- | --- |
| `event_loop.hpp` | `EventLoop` | epoll 循环、eventfd 唤醒、跨线程任务队列、定时器入口、上下文槽 |
| `channel.hpp` | `Channel` | fd 与关注事件、回调分发、`tie` |
| `poller.hpp` | `EpollPoller` | 水平触发 epoll；`updateChannel/removeChannel` |
| `timer_queue.hpp` `timer_id.hpp` | `TimerQueue` `TimerId` | 单个 timerfd 驱动的有序定时器集合 |
| `event_loop_thread.hpp` `event_loop_thread_pool.hpp` | | 线程初始化回调、round-robin `getNextLoop()` |
| `buffer.hpp` | `Buffer` | prependable + readable + writable；`readFd` 用 readv + 64KB 栈缓冲 |
| `inet_address.hpp` `socket.hpp` | | IPv4/IPv6 地址、RAII socket、`setTcpNoDelay/setKeepAlive/setReuseAddr` |
| `acceptor.hpp` `tcp_server.hpp` | | 监听、`accept4(SOCK_NONBLOCK|SOCK_CLOEXEC)`、EMFILE 处理 |
| `transport.hpp` | `Transport` `PlainTransport` | 明文/TLS 统一读写接口 |
| `tls.hpp` | `TlsContext` `TlsTransport` | OpenSSL 集成（G08） |
| `tcp_connection.hpp` | `TcpConnection` | 服务端与客户端共用；水位回调、暂停/恢复读 |
| `resolver.hpp` | `Resolver` | 独立线程 `getaddrinfo` + 缓存 |
| `connector.hpp` | `Connector` | 非阻塞 connect、多地址、超时 |
| `callbacks.hpp` | 回调类型别名 | |

## 2. EventLoop

```cpp
class EventLoop : noncopyable {
public:
    EventLoop();                                  // 每线程至多一个；构造时记录线程 id
    void loop();                                  // 直到 quit()
    void quit();                                  // 可跨线程调用；非本线程时 wakeup()
    void runInLoop(std::function<void()> f);      // 本线程直接执行，否则 queueInLoop
    void queueInLoop(std::function<void()> f);    // 加锁入队；非本线程或正在执行 pending 时 wakeup()
    TimerId runAt(Timestamp t, std::function<void()> f);
    TimerId runAfter(Duration d, std::function<void()> f);
    TimerId runEvery(Duration d, std::function<void()> f);
    void cancel(TimerId id);                      // 线程安全；对已触发/已取消的 id 无副作用
    void updateChannel(Channel*); void removeChannel(Channel*);
    bool isInLoopThread() const; void assertInLoopThread() const;  // 违规直接 abort，并打印两个线程 id
    std::any& context();                          // 放本 loop 的 UpstreamPool 等
    Timestamp pollReturnTime() const;
};
```

循环体：`poll(10s)` → 依次 `handleEvent` 活跃 Channel → `doPendingFunctors()`（先 swap 出队列再执行，执行期间新入队的任务需要 wakeup 下一轮）。

## 3. Channel 与 EpollPoller

- 水平触发（LT），不要用 ET：TLS 与暂停读都依赖"只要有数据就会再次通知"。
- 关注事件：`EPOLLIN | EPOLLPRI | EPOLLRDHUP`（读）、`EPOLLOUT`（写）。
- `handleEvent` 分发顺序：`EPOLLHUP && !EPOLLIN` → close；`EPOLLERR` → error；`EPOLLIN|EPOLLPRI|EPOLLRDHUP` → read；`EPOLLOUT` → write。
  带 `EPOLLRDHUP` 且同时有 `EPOLLIN` 时先 read（把剩余数据读完），read 返回 0 时再走 close。
- `tie(const std::shared_ptr<void>&)`：handleEvent 期间 lock 住所有者，防止回调里对象被析构。
- Poller 用 `index` 标记 new/added/deleted 三态，`disableAll()` 后 `EPOLL_CTL_DEL`。

## 4. TimerQueue

- 一个 `timerfd`（`CLOCK_MONOTONIC`, `TFD_NONBLOCK|TFD_CLOEXEC`）挂在 Channel 上；定时器存 `std::set<std::pair<Timestamp, Timer*>>` 与按序号索引的活跃集合。
- `TimerId{Timer*, sequence}`：用 sequence 防止地址复用导致误取消。
- **在定时器回调里取消自己或其他定时器**：处理到期列表期间把被取消的 id 记入 `cancelingTimers_`，重复定时器重新插入前检查它。
- Timestamp 统一用 `std::chrono::steady_clock`；只有日志与记录里的墙钟时间用 `system_clock`。
- 为了单测，提供一个 `Clock` 接口（`now()`），令牌桶等策略组件通过它取时间；Reactor 自身直接用 steady_clock。

## 5. Buffer

```text
| prependable(8) | readable | writable |
0        readerIndex   writerIndex    size
```

- `readFd(fd, &savedErrno)`：`readv` 两段——Buffer 可写区 + 栈上 64KB，读到栈上的部分再 append，一次系统调用读够。
- 需要的查找函数：`findCRLF()`、`findDoubleCRLF()`、`findEOL()`；`retrieve(n)`、`retrieveAll()`、`retrieveAsString(n)`；`peek()`、`readableBytes()`。
- `ensureWritableBytes(n)`：先尝试把 readable 前移腾空间，不够再扩容。TLS 读用它为 `SSL_read_ex` 准备连续空间。
- 可以提供 `shrink(reserve)`：长连接空闲时把大缓冲收缩，避免一次大响应之后长期占内存。

## 6. TLS 传输

### 6.1 Transport 接口

```cpp
enum class IoStatus { ok, would_block, eof, error };
struct IoResult { IoStatus status; std::size_t bytes = 0; };

enum class HandshakeStatus { done, want_read, want_write, failed };

class Transport {
public:
    virtual ~Transport() = default;
    virtual HandshakeStatus handshake() = 0;            // 明文：直接 done
    virtual IoResult readInto(Buffer& in) = 0;          // 读到 would_block/eof/error 或本轮上限（64KB）为止
    virtual IoResult write(const char* data, std::size_t n) = 0;
    virtual bool hasBufferedInput() const = 0;          // TLS：SSL_has_pending；明文：false
    virtual bool readWantsWrite() const { return false; }   // TLS 读时报 WANT_WRITE
    virtual bool writeWantsRead() const { return false; }   // TLS 写时报 WANT_READ
    virtual void shutdownWrite() = 0;                   // 明文：::shutdown(SHUT_WR)；TLS：SSL_shutdown 一次后 SHUT_WR
    virtual std::string lastError() const = 0;          // 分类与日志用
};
```

`TcpConnection` 只和 `Transport` 打交道。明文实现就是 `Buffer::readFd` 与 `::write`。

### 6.2 TlsContext（整个进程一个，跨线程只读共享）

| 步骤 | OpenSSL 调用 |
| --- | --- |
| 创建 | `SSL_CTX_new(TLS_client_method())` |
| 最低版本 | `SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION)` |
| 证书校验 | `SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr)`；`SSL_CTX_set_default_verify_paths(ctx)`；配置了 `ca_file` 时 `SSL_CTX_load_verify_locations` |
| ALPN | `SSL_CTX_set_alpn_protos(ctx, "\x08http/1.1", 9)`——只说 HTTP/1.1，避免服务端选 h2 |
| 会话恢复 | `SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_CLIENT | SSL_SESS_CACHE_NO_INTERNAL_STORE)`；`SSL_CTX_sess_set_new_cb` 回调里按 `host:port` 存 `SSL_SESSION*`（加锁；替换旧值时 `SSL_SESSION_free`；回调返回 1 表示接管引用） |

TLS 1.3 的会话票据在握手**之后**才到达，所以必须用 new-session 回调捕获，不能在握手完成时 `SSL_get1_session`。

### 6.3 TlsTransport（每连接一个，只在所属 loop 线程使用）

建立：

```text
SSL_new(ctx) → SSL_set_fd(fd) → SSL_set_connect_state
→ 主机名：SSL_set_tlsext_host_name(ssl, host)（SNI；IP 字面量跳过）
          SSL_set1_host(ssl, host)（证书主机名校验；IP 字面量改用 X509_VERIFY_PARAM_set1_ip_asc）
→ SSL_set_mode(ssl, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER)
→ 有缓存会话时 SSL_set_session(ssl, sess)
```

| 操作 | 实现 | 结果映射 |
| --- | --- | --- |
| 握手 | `SSL_do_handshake` | 1 → done；`SSL_ERROR_WANT_READ` → want_read（只关注读）；`WANT_WRITE` → want_write（关注写）；其他 → failed，错误文本拼上 `X509_verify_cert_error_string(SSL_get_verify_result(ssl))` |
| 读 | 循环 `ensureWritableBytes(16KB)` + `SSL_read_ex`，直到 WANT_READ 或本轮读满 64KB | `SSL_ERROR_ZERO_RETURN` → eof（对端 close_notify）；`SSL_ERROR_SYSCALL`/`SSL_R_UNEXPECTED_EOF_WHILE_READING` → eof 并标记"非正常关闭"；WANT_WRITE → 置 `readWantsWrite`，让连接关注写事件，写就绪时先重试读 |
| 写 | `SSL_write_ex(ssl, outputBuffer.peek(), readable)` | 成功返回实际写入字节；WANT_WRITE → would_block；WANT_READ → 置 `writeWantsRead` |
| 关闭 | `SSL_shutdown` 调用一次（发 close_notify，不等对端），然后关闭 fd | — |

必须遵守的 OpenSSL 细节：

1. **每次调用 `SSL_*` 之前 `ERR_clear_error()`**。错误队列是线程级的，残留错误会让 `SSL_get_error` 返回错误的分类。
2. WANT_WRITE 之后重试 `SSL_write` 必须传相同的数据（可以更长）。因为 Buffer 可能扩容挪位，所以要 `ACCEPT_MOVING_WRITE_BUFFER`。
3. 暂停读（背压）时 SSL 内部可能已经解密了数据，socket 却是空的，LT epoll 不会再通知。
   `startRead()` 时如果 `hasBufferedInput()` 为真，要 `queueInLoop(handleRead)` 主动读一次。
   本轮读满 64KB 提前退出时同理：SSL 里还有数据，socket 可能已经空了。
4. 握手耗时（connect 完成 → 握手完成）和 `SSL_session_reused(ssl)` 写进连接的统计，G08 验收要用。
5. 不做 HTTP/2，不做 OCSP，不做客户端证书；代理环境变量不支持（在文档里说明）。

### 6.4 错误分类

| 类别 | 例子 | 是否可重试 |
| --- | --- | --- |
| `tls_verify` | 证书过期、主机名不匹配、未知 CA | 否（配置/安全问题） |
| `tls_handshake` | 对端握手中断、协议版本不匹配 | 是（可能是网络抖动），计入熔断 |
| `tls_io` | 读写中 SSL_ERROR_SSL | 是 |

## 7. TcpConnection

状态：`kHandshaking → kConnected → kDisconnecting → kDisconnected`（服务端明文连接直接从 kConnected 开始）。

```cpp
class TcpConnection : noncopyable, public std::enable_shared_from_this<TcpConnection> {
public:
    TcpConnection(EventLoop*, std::string name, int fd, InetAddress local, InetAddress peer,
                  std::unique_ptr<Transport>);
    void send(std::string_view data);                 // 跨线程安全（非本线程时复制后 runInLoop）
    void send(Buffer* buf);
    void shutdown();                                  // 输出缓冲写完后再半关闭
    void forceClose();                                // 立即关闭，丢弃输出缓冲
    // 立即 transport->shutdownWrite()（不等输出缓冲）→ 同步循环 readInto 直到 would_block/eof/读满 limit，
    // 每段数据交给 onData → 关闭。只在 loop 线程调用；之后不再触发 message/close 以外的回调。取消与计量用，见 proxy-session.md §5.2
    void abortAndDrain(const std::function<void(std::string_view)>& onData, std::size_t limit = 1 << 20);
    void startRead(); void stopRead(); bool isReading() const;
    void setTcpNoDelay(bool);
    std::size_t outputBytes() const;                  // 背压与指标用
    void setConnectionCallback(ConnectionCallback);   // 上线（握手完成）/ 下线各一次
    void setMessageCallback(MessageCallback);         // (conn, Buffer*, Timestamp)
    void setWriteCompleteCallback(WriteCompleteCallback);          // 输出缓冲清空时
    void setHighWaterMarkCallback(HighWaterMarkCallback, std::size_t mark);
    void setCloseCallback(CloseCallback);             // 所有者（TcpServer/连接池）用来移除引用
    const std::string& lastError() const;             // 握手失败、transport 错误
    std::any& context();
    void connectEstablished(); void connectDestroyed();
};
```

发送路径（在 loop 线程内）：

1. 输出缓冲为空且未关注写 → 直接 `transport->write`；
2. 剩余部分追加到输出缓冲，关注写事件；
3. 水位检查：`old < mark && old + remaining >= mark` 时 `queueInLoop(highWaterMarkCallback)`——**只在向上穿越时触发一次**；
4. handleWrite 把输出缓冲写空后取消写关注，`queueInLoop(writeCompleteCallback)`；若处于 kDisconnecting 则执行半关闭。

读路径：`transport->readInto(inputBuffer)`；有数据 → messageCallback；eof → handleClose；error → handleError 后 handleClose。

必须做：

- **所有 socket 设置 `TCP_NODELAY`**。SSE 每个事件只有几十到几百字节，Nagle 与延迟 ACK 叠加会给每个 token 增加最多约 40ms 延迟，TTFT 和逐 token 延迟都会被拉高。
- 服务端连接开启 `SO_KEEPALIVE`。
- 流式期间**下游连接始终保持读关注**，否则无法及时发现客户端断开（取消传播依赖它）。
  客户端在流式期间发来的多余字节只进输入缓冲；超过 64KB 视为异常直接关闭。

## 8. Acceptor / TcpServer / EventLoopThreadPool

- Acceptor 预留一个 `/dev/null` fd 处理 EMFILE：accept 失败时关掉它、accept 后立即关闭新连接、再打开它，避免 LT 下忙等。
- `TcpServer::newConnection` 在 main loop 上创建 `TcpConnection`（选中的 io loop），再 `ioLoop->runInLoop(connectEstablished)`。
- `EventLoopThreadPool(baseLoop, n)`：`start(ThreadInitCallback)`，在每个 io 线程里构造该 loop 的 `UpstreamPool` 并放入 `loop->context()`。
- 连接移除：`removeConnection` 回到 main loop 删表项，再 `queueInLoop(connectDestroyed)` 回到 io loop。

## 9. Resolver

```cpp
class Resolver : noncopyable {
public:
    using Result = std::expected<std::vector<InetAddress>, std::string>;
    explicit Resolver(std::chrono::seconds cache_ttl = std::chrono::seconds{60});
    ~Resolver();                                  // 停止工作线程；未完成的请求直接丢弃
    // cb 在 loop 线程上执行；host 是 IP 字面量时不进入工作线程，直接 loop->queueInLoop
    void resolve(std::string host, std::uint16_t port, EventLoop* loop, std::function<void(Result)> cb);
};
```

- 单工作线程 + 队列；`getaddrinfo(AI_ADDRCONFIG, SOCK_STREAM)`；结果按 `host:port` 缓存 `cache_ttl`，失败结果缓存 5 秒，避免错误配置时打爆 DNS。
- 不支持取消；会话侧回调捕获 `weak_ptr`，会话已结束则丢弃结果。
- 为什么不用 `getaddrinfo_a`：它的信号/线程通知模型和 Reactor 结合更麻烦。单线程 + 缓存对网关足够，文档里说明取舍。

## 10. Connector

```cpp
enum class ConnectErrorKind { resolve, refused, unreachable, timeout, cancelled };
struct ConnectError { ConnectErrorKind kind; std::string message; };

class Connector : noncopyable, public std::enable_shared_from_this<Connector> {
public:
    Connector(EventLoop*, std::vector<InetAddress> addrs, std::chrono::milliseconds timeout);
    void start(std::function<void(int fd, InetAddress peer)> onConnected,
               std::function<void(ConnectError)> onFailed);
    void cancel();                    // 关闭正在连接的 fd，不再回调
};
```

- 每个地址：`socket(SOCK_NONBLOCK|SOCK_CLOEXEC)` → `connect`。立即成功或 `EINPROGRESS` → 关注写；可写时 `getsockopt(SO_ERROR)` 判断结果；
  还要检查自连接（本地地址 == 对端地址，连 127.0.0.1 的空闲端口时可能出现）。
- 当前地址失败 → 试下一个地址；全部失败 → `onFailed`（取最后一个错误）。
- 整体超时用 `runAfter(timeout)`；超时即关闭当前 fd 并 `onFailed(timeout)`。
- 成功时先 `channel.disableAll(); channel.remove()`，把 fd 交出去，**不要关闭**。
- Connector 不做重试退避；重试策略在 ProxySession（见 [proxy-session.md §6](proxy-session.md#6-重试降级与熔断)）。

## 11. UpstreamPool（`gw_http`，每 loop 一个）

```cpp
struct UpstreamKey { bool tls; std::string host; std::uint16_t port; auto operator<=>(const UpstreamKey&) const = default; };

struct AcquireError { enum Kind { resolve, connect_refused, connect_timeout, tls_verify, tls_handshake, cancelled } kind; std::string message; };

class UpstreamPool : noncopyable {
public:
    UpstreamPool(EventLoop*, Resolver&, TlsContext*, PoolOptions);
    // 回调在本 loop 上执行；返回的连接已完成 TLS 握手、处于 kConnected
    std::shared_ptr<AcquireHandle> acquire(const UpstreamKey&, std::chrono::milliseconds connect_timeout,
        std::function<void(std::expected<Lease, AcquireError>)> cb);
    void release(Lease);              // 仅在连接可复用时调用，否则 lease 析构时直接关闭
    std::size_t idleCount() const;
};
struct Lease { std::shared_ptr<TcpConnection> conn; bool reused; Duration connect_time; Duration tls_time; bool tls_resumed; };
```

- 空闲连接按 key 存 LIFO 栈，每个 key 最多 `max_idle_per_host`（默认 16）；空闲超过 `idle_timeout` 关闭。
- 空闲连接要保持读关注：对端关闭时自动从池中移除；空闲期间收到任何字节也视为异常，直接关闭。
- `AcquireHandle::cancel()`：会话在建连阶段被取消时调用，停止 Resolver 回调的后续动作和 Connector。
- **复用连接的竞态**：llama.cpp 底层的 cpp-httplib 默认 keep-alive 空闲超时只有几秒（以实测为准），可能正好在我们复用它时关闭。
  规则：`Lease.reused == true` 的连接在**收到任何响应字节之前**失败（EOF/ECONNRESET/EPIPE），立即换一个新建连接重发一次，
  不计入重试次数、不计入熔断。每个上游的 `pool_idle_seconds` 默认 4 秒，比对端超时短。

## 12. 易错点清单

| 现象 | 原因 | 对策 |
| --- | --- | --- |
| 流式每个 token 都慢 40ms | Nagle + 延迟 ACK | 两端都 `TCP_NODELAY` |
| 背压恢复后流卡住不动 | TLS 缓冲里有数据，socket 已空，LT 不再通知 | `startRead` 时检查 `SSL_has_pending` |
| 偶发 `SSL_get_error` 分类错误 | 线程错误队列有残留 | 每次调用前 `ERR_clear_error()` |
| 取消后又收到上游回调导致 crash | 回调捕获了裸指针/强引用 | 统一捕获 `weak_ptr`；Channel `tie` |
| 定时器回调里取消自身无效或 double free | 到期处理期间的取消没记录 | `cancelingTimers_` |
| 复用连接偶发 "empty reply" | 对端刚好关闭空闲连接 | 未收到字节前失败 → 新连接重发一次 |
| 大量连接后 accept 空转 100% CPU | EMFILE + LT | 备用 fd 技巧 |
| 下游断开没发现，上游继续生成 | 流式期间关掉了下游读关注 | 下游始终保持读关注 |
