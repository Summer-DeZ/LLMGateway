# LLMGateway 执行规则

1. `.hpp` 只放 `src/public/<模块>/`，`.cpp` 只放 `src/private/<模块>/`；第三方 vendor 头放 `src/public/lib/`。
2. 单元测试放 `test/unit/`（gtest），只测纯逻辑组件（解析器、令牌桶、LRU、协议转换等），不在单测里起模型。
3. 唯一允许的测试辅助服务是 `test/fault_upstream/`（故障注入上游），只用于制造真实模型难以稳定复现的故障（429、流中断开、挂起）。功能正确性以真实模型为准。
4. 真实验收的脚本、日志、截图放 `temp/<任务编号>/`，不放进源码树。
5. 不修改、停止或重启正在被别人使用的模型服务；需要停 Qwen3.6 做降级演示时，先确认没有其他使用者。
6. 真实密钥与数据库密码只从环境变量或 `deploy/.env` 读取（配置里写 `env:NAME`），不得写进配置文件、文档、日志和验收材料。
7. 状态取值：未开始、实施中、构建通过待真实验收、真实验收通过、受环境阻塞。构建通过不等于完成。
8. 本规格锁定的是对外行为、线程归属、对象所有权和状态机语义；类名、局部算法、容器选型可自行决定，偏离锁定部分时在任务记录里写明原因。

## 构建与测试

```bash
cmake --preset dev && cmake --build --preset dev -j
ctest --preset dev --output-on-failure
./build/dev/llm-gateway -c home/gateway.json
```

规格在 `nexttodo/`，按任务编号 G00–G15 推进，先读对应章节再动手。
