# net_loop PMD 实现报告

## 1. 概述

net_loop 是一个基于 `rte_ring` 的真回环虚拟以太网驱动（PMD）。
每个设备的 TX burst 将 mbuf 指针入队到 `rte_ring`，RX burst 从同一个 ring 出队。
零拷贝，仅传递指针。

```
tx_burst() → rte_ring_enqueue_burst(ring) → rte_ring_dequeue_burst(ring) → rx_burst()
```

## 2. 文件清单

| 文件 | 行数 | 用途 |
|------|------|------|
| `drivers/net/loop/rte_eth_loop.c` | ~290 | 驱动主文件 |
| `drivers/net/loop/meson.build` | 4 | 构建配置 |
| `app/test/test_pmd_loop.c` | ~160 | 单元测试 |

修改的文件：
- `drivers/net/meson.build` — 添加 `'loop'` 到 drivers 列表
- `app/test/meson.build` — 添加测试文件依赖

## 3. 数据结构

### 3.1 loop_queue — 队列结构

```c
struct loop_queue {
    struct rte_ring *rng;   /* 共享的回环 ring */
    uint16_t port_id;       /* 所属端口 ID */
    uint64_t rx_pkts;       /* RX 包计数 */
    uint64_t tx_pkts;       /* TX 包计数 */
};
```

### 3.2 pmd_internals — 设备私有数据

```c
struct pmd_internals {
    uint16_t port_id;
    struct loop_queue rx_queues[1];  /* 固定 1 个 RX 队列 */
    struct loop_queue tx_queues[1];  /* 固定 1 个 TX 队列 */
    struct rte_ether_addr eth_addr;  /* MAC 地址 */
};
```

核心设计：`tx_queues[0].rng` 和 `rx_queues[0].rng` 指向**同一个** `rte_ring` 实例。
TX 入队，RX 出队，形成回环。

## 4. 回调函数实现

### 4.1 eth_dev_ops 最小回调集（13 个）

| 回调 | 行为 |
|------|------|
| `dev_configure` | return 0 |
| `dev_start` | link UP, queue state STARTED |
| `dev_stop` | link DOWN, queue state STOPPED |
| `dev_close` | free ring, mac_addrs = NULL |
| `dev_infos_get` | max_mac_addrs=1, max_rx_pktlen=UINT32_MAX, scatter/multi_segs offload |
| `rx_queue_setup` | 绑定 queue, 设置 port_id |
| `tx_queue_setup` | 绑定 queue |
| `stats_get` | 汇总 rx_pkts/tx_pkts (含 qstats) |
| `stats_reset` | 清零计数 |
| `link_update` | return 0 |
| `promiscuous_enable` | return 0 |
| `promiscuous_disable` | return 0 |
| `mac_addr_set` | 复制 MAC 地址到 internals->eth_addr |

### 4.2 数据路径

**TX burst** (~10 行): 调用 `rte_ring_enqueue_burst()` 将 mbuf 指针入队，更新 tx_pkts 计数。

**RX burst** (~15 行): 调用 `rte_ring_dequeue_burst()` 从同一 ring 出队，设置 mbuf->port，更新 rx_pkts 计数。

### 4.3 设备创建流程

```
rte_pmd_loop_probe()
  → 解析 "size=<int>" 参数
  → eth_dev_loop_create()
      → rte_ring_create("LOOP_%s", size, RING_F_SP_ENQ|RING_F_SC_DEQ)
      → rte_eth_vdev_allocate(vdev, sizeof(*internals))
      → 初始化 internals: 同一个 ring 赋给 tx_queues[0] 和 rx_queues[0]
      → 随机生成 MAC 地址
      → 设置 data: nb_rx_queues=1, nb_tx_queues=1, dev_link, mac_addrs
      → 注册 ops 和 burst 函数
      → rte_eth_dev_probing_finish()
```

## 5. 驱动注册

```c
static struct rte_vdev_driver pmd_loop_drv = {
    .probe = rte_pmd_loop_probe,
    .remove = rte_pmd_loop_remove,
};

RTE_PMD_REGISTER_VDEV(net_loop, pmd_loop_drv);
RTE_PMD_REGISTER_PARAM_STRING(net_loop, "size=<int>");
```

用法：`--vdev=net_loop0` 或 `--vdev=net_loop0,size=2048`

## 6. 单元测试

文件：`app/test/test_pmd_loop.c`
注册为 `REGISTER_FAST_TEST(loop_pmd_autotest, true, true, test_loop_pmd)`

### 测试用例

| 测试 | 验证内容 |
|------|----------|
| `test_loop_configure` | vdev 创建 → configure → queue setup → start → link get (验证 link UP, 10G) |
| `test_loop_tx_rx` | 分配 128 个 mbuf → TX burst → RX burst → 验证收到的指针与发送的一致 (零拷贝) |
| `test_loop_stats` | stats reset → 验证全 0 → TX/RX 各 1 包 → 验证 ipackets=1, opackets=1 → reset → 验证归零 |

运行方式：
```bash
./build/app/dpdk-test --no-huge --no-pci -l 0 loop_pmd_autotest
```

测试结果：
```
 + Test Suite Summary : Loop PMD Unit Test Suite
 + Tests Total :        3
 + Tests Skipped :      0
 + Tests Executed :     3
 + Tests Passed :       3
 + Tests Failed :       0
```

## 7. testpmd 验证

```bash
sudo ./build/app/dpdk-testpmd -l 0-1 -n 1 --no-pci --vdev=net_loop0 -- -i
```

结果：
- 设备初始化成功，Port 0: 6A:43:7D:F1:42:CF
- Link status: up, 10 Gbps, full-duplex
- Max RX/TX queues: 1
- `start tx_first 1` 后持续转发
- RX-packets == TX-packets (19,780,772,320)，零丢包，零错误

## 8. 与参考实现的对比

| 特性 | net_loop | net_null | net_ring |
|------|----------|----------|----------|
| 数据路径 | rte_ring 回环 | 分配/释放空包 | rte_ring 转发 |
| 零拷贝 | 是 | 否 | 是 |
| 多队列 | 否 (固定 1) | 是 (RTE_MAX_QUEUES_PER_PORT) | 是 (可配置) |
| RSS 支持 | 否 | 是 | 否 |
| 代码行数 | ~290 | ~780 | ~830 |
| 回调数量 | 13 | 17 | 17 |
| 参数 | size | size, copy, no-rx | nodeaction |

## 9. 关键设计决策

1. **固定 1 个队列**: 简化实现，避免 RTE_MAX_QUEUES_PER_PORT 的内存开销
2. **SP/SC ring**: 使用 `RING_F_SP_ENQ | RING_F_SC_DEQ` 标志，单生产者单消费者模式下无需原子操作，性能最优
3. **无 header 文件**: 不对外暴露 API（不需要 `rte_eth_from_rings` 这样的公共接口）
4. **无 version.map**: 作为纯 vdev 驱动，没有需要导出的符号
5. **mac_addr_set 实际复制**: 不同于 null PMD 的空实现，这里实际保存 MAC 地址
