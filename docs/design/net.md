# Reactor 与连接所有权

`gw_net` 实现 epoll、eventfd、timerfd、EventLoop、Channel、TcpConnection、Connector 与 Resolver。主线程负责监听与装配，每个 IO 线程拥有自己的 EventLoop、上游连接池和 RedisClient。一次代理请求的上下游 I/O 全部归属同一 IO loop；DNS 查询在线程外完成，再投递结果回原 loop。

连接关闭先取消定时器、摘除 Channel，再关闭 fd；上游 Lease 只有协议完整结束且允许复用时才归还池。账本有独立 loop 和 PostgreSQL 写/读连接。跨线程的连接健康状态使用原子发布，账本队列与 in-flight 数受锁保护。

退出顺序：停止接收 → 排空/取消会话 → 停 IO 线程 → 等待账本排空（含已出队未提交批次）→ ledger loop 内销毁依赖对象 → 清理完成屏障 → join。真实 PG 表锁阻塞提交时，SIGTERM 等待约 3.475 秒后退出，账单仍存在。

1000 条并发慢速故障流实测全部结束、活动流峰值 1000，网关 RSS 峰值 28.88 MiB。详见 [压测](benchmark.md)。


规格与历史记录：[实施任务](../../nexttodo/implementation-tasks.md)。所有数值为 2026-09-25 本机实测，不代表远端部署结果。
