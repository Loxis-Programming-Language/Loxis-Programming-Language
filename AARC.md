# AARC — Adaptive Asynchronous Reference Counting

> 一个把「引用计数」和「环回收」拆成两条独立时间尺度、并把执行位置（inline / async）
> 按对象所有权动态选择的 GC。最终目标是在真实语言运行时里做到：
> **精确根、无全局 STW、环垃圾能收、且稳态开销贴着 ARC。**

本文档是整个 AARC 的技术总结：设计、代码地图、里程碑、**全部实测数据**、踩过的坑、
以及还没做的事。数字全部来自本机实测（Windows / MinGW g++ 13.1 / CLion 工具链），
所有“这台机器噪声很大”的地方都明确标注了。

---

## 目录

- [1. 问题定义](#1-问题定义)
- [2. 核心设计](#2-核心设计)
- [3. 代码地图](#3-代码地图)
- [4. 里程碑：引擎（M0–M12）](#4-里程碑引擎-m0m12)
- [5. 里程碑：接进 Loxis VM（M13–M14）](#5-里程碑接进-loxis-vm-m13m14)
- [6. 实测数据](#6-实测数据)
- [7. 踩过的坑（bug 清单）](#7-踩过的坑bug-清单)
- [8. 构建 / 测试 / 跑分](#8-构建--测试--跑分)
- [9. 已知限制与未做的事](#9-已知限制与未做的事)
- [10. 可以写进论文的几句话](#10-可以写进论文的几句话)

---

## 1. 问题定义

三种主流方案各自的死穴：

| 方案 | 环垃圾 | 根精度 | 停顿 | 稳态开销 |
|---|---|---|---|---|
| Tracing GC (STW) | 能收 | 需要 stack map / safepoint | 全局 STW | 低 |
| Reference Counting | **收不掉** | 不需要（每对象计数） | 无 | 每次指针写都要动 |
| 混合 (RC + 周期 tracing) | 能收 | 不需要 | 通常仍有 STW | 计数 + 周期扫描 |

AARC 想同时拿到：

1. **精确根**：靠 VM 的寄存器/全局/溢出栈屏障，而不是保守扫描
2. **无全局 STW**：回收是局部的，最坏只有单次局部扫描（实测 ≤ 0.32ms）
3. **环能收**：ECR + Tarjan，且**触发频率自适应**
4. **稳态开销贴着 ARC**：记账模型本身不比 refcount 贵

关键洞察（也是整个项目最重要的一次测量结论）：

> **AARC 的记账模型本身很快（甚至比 atomic ARC 快），慢的从来是异步机制。**
> 所以正确的架构不是「把所有对象都丢给异步 worker」，而是
> **按所有权选择执行位置**：单线程独占的对象 inline 收（LOCAL），
> 逃逸到别的线程的对象才交给 worker（SHARED）。

---

## 2. 核心设计

### 2.1 Cell 与版本化 handle

```
ObjHandle { uint32_t index; uint32_t gen; }
```

- cell 是最小 GC 单元，payload 9 字节（`tag:1 + id:4 + data:4`）
- 每个 slot 的 meta 是**一个** `atomic<uint64_t>`：`(generation << 1) | live_bit`
- alloc/free 是 slot meta 的唯一写者，每次状态迁移只有**一次原子 store**，
  所以并发读者 acquire-load 之后做 `live_bit && gen == handle.gen` 永不撕裂
- **generation 是 ABA 保护**：延迟到达的消息、陈旧 handle 永远不会匹配到复用了
  同一 slot 的另一个对象

### 2.2 RefTable：真实的图，而不是裸计数

每个逻辑边是一条稳定的 `EdgeRec`，用侵入式双向链表挂进 source 的 out-list 和
target 的 in-list。**重复 (src,dst) 边是不同记录**（multiset 语义）。

两套实现，编译期二选一：

| 方案 | 开关 | 每边成本 | 特性 |
|---|---|---|---|
| 精确反向邻接 | 默认 | ~40 B/edge | O(1) 入边遍历 → promotion closure O(component) |
| 仅正向 multiset | `-DAARC_CONSERVATIVE` | ~8 B/edge | promotion 用「可能连通分量」，可以过度提升 |

边记录分配是**无锁**的：`nextId_` 原子 + tagged-CAS free list（防 ABA）+
两级原子 block 目录（每段 1024 块 × 每块 1024 条）。安装新块是一次性 CAS。

### 2.3 消息模型

```cpp
enum class GCOp { RootAdd, RootRemove, RootCheck, EdgeAdd, EdgeRemove, Terminate };
struct GCMessage { GCOp op; ObjHandle a; ObjHandle b; uint32_t count; };
```

- 每个引用变更被拆成 **root 变更** 或 **边变更**，而不是裸 incref/decref ——
  这样 worker 才能维护真实对象图（ECR / Tarjan 需要）
- `RootCheck` = 折叠后净变化为 0，但「必须重新评估 zero/suspect」。
  这是折叠（folding）语义等价的关键
- 每条消息带完整版本化 handle，worker 应用前校验 generation

### 2.4 三阶段回收

```
阶段 1  zero-chain：refcount 归零 → 直接回收，可级联
阶段 2  ECR：从 suspect 出发找 SCC，算组件的外部入边数
阶段 3  Tarjan：ECR == 0 的 SCC 整块回收
```

- `process()` 只 **标记** 归零的 cell，绝不 inline free
- `sweepZero()` 只在 drain point 执行（此时所有队列已消费到一致切面）
- `sweep()` = `{ sweepZero(); if (suspectStack_ 非空) collectCycles(); }` 循环到不动点
  （这个不动点是必须的：收一个零链可能让它后面的环变孤儿）

### 2.5 LOCAL / SHARED 与提升（promotion）

```
Regime { Local, Promoting, Shared }
```

- **LOCAL**：单 owner 线程内联跑完整引擎（含环回收），无队列无 worker
- **SHARED**：对象已逃逸，交给异步 worker
- **提升不是拷贝，是所有权交接**：cell 永不移动，handle 永远有效；
  RefTable 行是共享真相，提升只是把 owner 换掉
- 提升触发条件只有一个：**第一次跨 owner 逃逸**（具体可观测事件），
  不是「环出现了」——每个 owner 自己都能收环
- 提升是保守的：做**无向**连通分量闭包，保证不留下 LOCAL↔SHARED 的边
- v1 单调向上，没有降级

### 2.6 自适应环触发（M14 的核心）

**两条时间尺度彻底分开**：

```
每个屏障操作：  sweepZero()      ← 便宜，越及时越好，永远急切地做
按压力触发：    collectCycles()  ← 贵，单独 gate
```

触发条件（三选一）：

```
1. suspect pressure   activeSuspects >= suspectThreshold
2. memory pressure    heapCommitted >= heapTarget && allocBytesSinceCycle >= allocThreshold
3. starvation bound   opsSinceCycle >= maxInterval          ← 没有它小环会永生
```

**yield 反馈**：

```
每次扫描后  yield = reclaimed / visited
  reclaimed == 0 && visited >= 256   →  suspectThreshold *= 2
  reclaimed * 4 >= visited           →  suspectThreshold /= 2
  夹在 [512, 65536]
```

一句话概括策略：

> **没垃圾时主动退避，有垃圾时主动变激进。**

论文用句（实测支持）：

> Suspect pressure is measured as the number of currently marked **distinct**
> suspects, not the size of the lazy work stack.

---

## 3. 代码地图

```
src/aarc/
  core/
    cell.hpp          CellPool：9B cell、版本化 handle、sharded free list、
                      per-owner recycle pile、producer-local slot cache
    gc_message.hpp    GCOp / GCMessage
    owner.hpp         OwnerToken{id, generation}
    producer_slot.hpp 生产者发布槽（batch staging + pubSeq 发布窗口）
    spsc_queue.hpp    单生产者单消费者环形队列（含 zero-copy span 写入）
    root_bag.hpp      扁平开放寻址的 root 多集（无 per-entry 堆分配）
    prof.hpp          rdtsc 采样计时（AARC_PROFILE）
  arc/
    engine.hpp        AarcEngineT<Cells>：process / sweepZero / sweep /
                      collectCycles / Tarjan 驱动 / 自适应触发所需的全部计数
    ref_table.hpp     选择器（reverse vs forward）
    ref_table_reverse.hpp  精确反向邻接（默认）
    ref_table_forward.hpp  仅正向（-DAARC_CONSERVATIVE）
    local_components.hpp   保守方案的「可能连通分量」存储
    owner_registry.hpp     租约式 owner 生命周期（Free/Active/Closing）
    runtime.hpp            顶层 Runtime：Mode::Sync / Async / Adaptive，
                           worker 循环、一致切面、quiesce 票据、提升协议
  cycle/
    cycle_detector.hpp  CycleDetectorT<Cells>：迭代式 Tarjan SCC
  tests/    test_core / test_arc / test_cycle / test_stress / test_adaptive /
            test_m3 / test_concurrency / test_batch / test_batch_cons / test_suspects
  bench/    见第 8 节
  docs/     adaptive-rc-design.md（LOCAL/SHARED 分割的设计文档）
```

**VM 侧（Loxis）**：

```
src/cpp/vm/
  Heap.hpp            重写：slot 表身份 + 真分配器（size-class free list）+ 复用
  GC.hpp / GC.cpp     GCBackend 接口 + NoGC / ARC / LOCAL-AARC 三个后端
  AarcCellSource.hpp  外部 cell 源：collector metadata 与 object storage 解耦的
                      唯一接缝
  VM.cpp              barrier（setReg/setGlobal/HStore）+ 三后端共享的执行路径
src/cpp/tools/
  BenchGC.cpp         GC benchmark harness（bench_gc target）
```

**关键解耦**：`AarcEngine` 与 `CycleDetector` 都改成 `template <class Cells>`，
留 `using AarcEngine = AarcEngineT<CellPool>;` 别名，所以：

- AARC 自己拥有：refcount / roots / 图 / suspect / Tarjan scratch
- 宿主拥有：payload / size / free list / 真正的字节复用
- 两边唯一的接口：`valid / isLiveAt / generationOf / free`

算法完全不知道 Loxis Heap 的存在。

---

## 4. 里程碑：引擎（M0–M12）

### Phase 0–2（初始实现）

| 阶段 | 内容 | 状态 |
|---|---|---|
| Phase 0 | cell pool / SPSC / 消息 | done |
| Phase 1 | 异步 ARC + reclaimer | done |
| Phase 2 | Tarjan SCC + ECR 环回收 | done |

正确性在第一阶段就拿到了：30% 环负载下，baseline ARC 精确泄漏那 30%
（多个线程数下 live 都不为 0），AARC 每个线程数都做到 **live = 0**，零 GC 错误。

### 性能结论（诚实版）

> **调优前 AARC / ARC ≈ 16–38%。** 差距全在异步机制，不在记账模型。

两个优化把差距收窄到 ~57–71%（T≥4）：

1. per-slot 无锁 `atomic<uint64_t>` liveness word（去掉每条消息的 pool 锁）
2. 全局 `pending_` 计数，worker 连续 drain，生产者只在 0→1 时唤醒

随后又做了 pool 分片、per-owner recycle pile —— **AARC 基本原地不动**。
硬证据：T1 无环 AARC ~4–5M vs ARC ~20M ≈ **单生产者/worker 对的 4 倍异步税**。

**结论：worker 的 per-message 成本是墙。**

### M1–M3：LOCAL / SHARED 分割

- **M1**：per-slot 三态 Regime
- **M2**：`Mode::Adaptive` —— 每个 Mutator 一个内联 `AarcEngine(Local)`，
  与 worker 的 `AarcEngine(Shared)` 共存于**同一个 CellPool + RefTable**，
  slot 所有权不相交
- **M3**：所有权交接式提升。逃逸方注册请求并阻塞，owner 在本地 safepoint 服务：
  结算 scratch → 算无向连通分量闭包 → 冻结 PROMOTING → 提交 SHARED → bump epoch。
  worker 不需要显式 adopt（RefTable 行是共享真相）

验证：纯 LOCAL 负载从不提升（`promotionsDone == 0`）；40 次连续交接零泄漏零错误。

### M4 基准：T1 的丑数字崩掉了

| 引擎 | T=1 中位吞吐 | 相对 ARC |
|---|---|---|
| ARC | 19.1M obj/s | 1.00× |
| **Adaptive-LOCAL** | 20.2M obj/s | **1.06×**（band 19.7–20.6） |
| AARC-async | 4.60M obj/s | 0.24× |

**这就是整个项目的转折点：LOCAL 内联跑 = 没有异步税，跟 ARC 打平甚至略胜。**
T≥4 的数字在这台机器上不可靠（run-to-run 摆动可达 2×），不做任何缩放声明。

### M5：并发协议定型（6 个真实缺陷全修）

- **一致切面**：per-producer `pubSeq`，even→odd→push→even；worker 采
  `pre[i]→tails[i]→post[i]`，全 even 且 pre==post 才算稳定切面
- **workEpoch 取代 `pending_`**：`pending_` 在 submit/fetch_sub 顺序竞争下会下溢
- **quiesce 票据**：worker 在轮次开始时捕获 `serveThrough = issued`，
  只在稳定切面发布 completed（旧切面不能服务后来的票据）
- **通知必须在 cvMu_ 下** —— 否则 notify 落在等者的「释放锁并注册」窗口里丢失
  → `awaitQuiesce` 偶发永久挂起（只有循环 20–50 次才抓得到）
- **owner 生命周期**：`OwnerToken` + 租约式注册表（第 65 个返回 nullptr）
- **suspect ABA**：`suspectStack_` 存裸索引，一个 SHARED cell 被标记 suspect
  后 free 又被 LOCAL 复用 → 下一轮 worker 会把活着的 LOCAL cell 当自己的 suspect
  （release 下会回收活对象）

### M6：侵入式反向邻接（closure O(pool) → O(component)）

promotion closure 从全池扫描改成 `outs(i) + ins(i)` 无向游走。
**实测 ~1.67×**（escape row：677,979 vs 407,063 obj/s）。

代价：40 B/edge（vs 8 B），slot 记录 +16 B。
顺带修了一个段错误：arena 的全局 free list 是单写者，
多 LOCAL 引擎并发分配边会打烂 freelist → 改成无锁 arena。

### M7：评测基准 + 3 个真实性能 bug

- **bug 1**：`closeOwner` 的 promote-all 循环是 O(live²)（每个组件重扫整个 live 集合）
  → 500K 填充时挂起。改成一次性收集所有 local 种子
- **bug 2**：RootBag 的 tombstone 从不计入 load → 探针无限循环 → 多生产者挂起
- **bug 3**：热路径每 op 调 `OwnerRegistry::state()`（带互斥）→ f=0 直接腰斩
  （4.3M → 8.6M）

RootBag 也从 `unordered_map` 换成扁平开放寻址（per-op 节点分配会把 LOCAL
快路径从 24M 打到 9M）。

### M8：f=0 成本归因

| 层级 | ARC | AARC | 比例 |
|---|---|---|---|
| allocOnly（零边） | 41.0M | 16.5M | 40.3% |
| edgeAdd | 22.1M | 13.7M | 61.8% |
| churn（+edgeRemove） | 21.7M | 13.1M | 60.5% |

**结论：f=0 的差距不是反向邻接（M6）的锅。**
零边场景就已经只有 40% —— 税在 per-op 的 `engine.process` + RootBag。

### M9：保守 LOCAL 分量（编译期 A/B）

默认反向邻接 vs `-DAARC_CONSERVATIVE`：

| 轴 | conservative | reverse |
|---|---|---|
| promotion 延迟（heap 10K–1M） | 平的 ~3µs | ~10µs（不随 heap 变） |
| promotion 延迟（组件 1–10K） | 快 ~13.5× | 慢 |
| EdgeRemove | ~392M/s | ~54M/s（~7×） |
| 每边内存 | 8 B | 40 B |

conservative 在每个轴上都赢。**没有被算法改变的 f=0 残余**
（allocOnly ~37% of ARC）是 M5 RootBag + process 的成本。

### M10a：异步传输批量化 —— 4.6×

生产者把最多 `batchSize` 条消息暂存在 ProducerSlot，用**一个**发布窗口、
一次 workEpoch、一次唤醒整块发布。

| batch | 吞吐 | 相对 ARC |
|---|---|---|
| 1 | 2.55M | 12.5% |
| 8 | 10.1M | 49.5% |
| **32** | **11.69M** | **57.3%**（膝盖） |
| 128 | 11.77M | 57.7% |

batch ≥ 8 后 peakQ 饱和在 4096 → 生产者又跑赢了单 worker。

**顺手抓出两个先存在的 bug**（batch=1 也会中）：
- worker 启动时的丢唤醒（`seenWork` 在入口赋值，早于第一次 wait 的发布被吃掉）
- cycle collector 不动点缺失（最后一次 `sweepZero` 的级联会把环变孤儿，
  而它已经在 `collectCycles` 之后 → 程序若就此静默则永久泄漏）

### M10b：批内消息折叠

同一批内对同一 handle 的所有 root 操作折成净 delta，净 0 → `RootCheck`。

- 折叠率 **1.33×–1.64×**（batch 8→128）
- 但端到端只有 0.92–1.00× —— 因为生产者变成了墙

### M11：生产者热路径（3 刀 + 1 个真 bug）

- **M11c（最大的一刀）**：RootBag 在 churn 下被 tombstone 撑到
  **524288 slot（6MB）而 size_ = 0** → 每次探针都是冷 cache/TLB miss，
  **98ns/pair**。改成按 live load 增长后 **98 → 16ns/pair**
  - 副作用：`bench_local_cost` 的 f=0 从 allocOnly 40.3% → **74.8%**，
    edgeAdd 61.8% → **93.8%**，churn 60.5% → **86.3%**
- **M11a**：zero-copy span ring（消息直写环形区，一次 release store 发布整段）
  → 1.05–1.65×
- **M11b**：producer-local slot cache（每次 refill 64 个）
  → poolAlloc 33.8 → **13–14ns/pair**
- **M11e**：生产者快了以后折叠终于赚了：**1.03 / 1.18 / 1.18 / 1.33×**

归因（batch=32，自洽求和等于墙钟）：**~105ns/pair**（从 ~155–180ns 降下来）。

### M12：flush-before-park 活性契约

> **一个 mutator 必须在停止推进 AARC 之前发布它暂存的批次。**

否则一个活着但停住的 mutator 可以让任何稳定切面永远无法形成 ——
在「on-the-fly 异步回收」的说法下这是会被 reviewer 抓的进度 bug。

实现为 `Mutator::willPark()`（等价 flush，空时零成本）。
`awaitQuiesce` 带 2 秒超时，超时打印卡住的生产者并计数。

M12b（跳过一次 `pool_.valid()` 的快路径，1.026×）**被拒绝并入默认关闭** ——
收益太小，不值得削弱一个防御性校验。

---

## 5. 里程碑：接进 Loxis VM（M13–M14）

### M13a：外部存储适配器

三后端共享**同一份** bytecode / Heap / 分配器 / barrier 插入，
只换生命周期记账。这是整个实验设计的核心：

```cpp
class GCBackend {
    virtual void onAlloc(uint32_t handle);
    virtual void rootAdd(uint32_t handle);
    virtual void rootRemove(uint32_t handle);
    virtual void edgeAdd(uint32_t from, uint32_t to);
    virtual void edgeRemove(uint32_t from, uint32_t to);
    ...
};
```

- **NoGC**：全部 no-op（但**仍然走同样的虚调用**，所以它是记账成本的下界，
  而不是「理想列」）
- **ARC**：经典引用计数，回收是 payload 9 字节 cell 的工作表遍历
- **LOCAL-AARC**：`AarcEngineT<AarcCellSource>`，inline、单 owner、含环回收

**Heap 重写**（`src/cpp/vm/Heap.hpp`）：

- **handle ≠ 字节偏移**。handle 是 slot 索引；payload 内的字节偏移按对象解析
- **slot 0 保留**：编译器把一部分 null 判断 lowering 成「跟 0 做数值比较」，
  而旧的字节偏移因为 8 字节 header 永远不为 0。**handle 0 必须永远是 null**
- payload 按整 cell 向上取整，但 **bounds check 用 `logicalSize`**
  （调用方真正申请的长度），否则分配器的 padding 会悄悄把越界写变合法
- 真分配器：size-class free list（1..64 cell 直索引，更大走 map）+ 真复用
- `reserveSlots(n)` 固定索引空间（AARC 的 scratch 是一次性定长的）

**HFree 政策定死**：所有后端下都是 no-op，只计 `ignoredFrees`。
有 collector 时生命周期归 collector；真 free 会制造悬垂 handle。

**barrier 收敛**：`setReg` / `setGlobal` / HStore。
`m_dataStack`（PushR/PopR 的溢出栈）也是根集。

### M13b–e：验证与基准

- 121 个程序（demo + `src/test/*.lx`）在三后端下输出**逐字节一致**，
  且与改动前的 golden 一致
- 三个 workload（churn / tree / cycle）语义正确
- 真实 payload 复用确认（`reusedBytes` / `reuseAllocs`）
- `bench_gc` harness

**harness 方法论**（都是踩过坑的）：

- 编译**一次**，每个 run 拷一份 Chunk
- 只计 `vm.run()`：JIT 构建（`vm.run(0)`）和 teardown（`finishGC()`）在计时区外
- **每个 rep 反转后端顺序**（固定顺序会让机器漂移伪装成后端差异，
  这个项目里已经产生过符号翻转）
- 报 median / p25 / p75 / min / max
- 截断就标 TRUNCATED
- 吞掉程序 stdout 同时跨后端比对（不一致就警告）
- 进程退出语义的 teardown：出统计前丢光所有根，所以 `live` 表示「不可达」
  而不是「还躺在寄存器里」

### M14a–e：自适应环触发

见 [2.6](#26-自适应环触发m14-的核心)。实现要点：

- `afterOp()`：每个屏障操作都跑 `sweepZero()`（有零候选才跑），
  只 gate `collectCycles()`
- 触发原因分四类计数：suspect / heap / starvation / fixed
- yield 反馈带滞回：只在 `visited >= 256` 时调整，中间区间不动
- **`pendingSuspects()` 改成 distinct 计数**（见 [7.9](#79-suspect-信号虚胖)）

---

## 6. 实测数据

> ⚠️ **本机噪声很大**：跨 session 的绝对毫秒数可以摆动 ~2×。
> 只有**同一次运行内、交替顺序**的对比可信。所有对比表都是这么测的。

### 6.1 引擎级微基准（无 VM，Release）

| 指标 | 数值 | 备注 |
|---|---|---|
| ARC（T1，无环） | ~19–21M obj/s | `bench_local_fastpath` |
| **AARC-sync / Adaptive-LOCAL（T1）** | **~20–25M obj/s** | **119–133% of ARC** |
| AARC-async（T1） | ~3.8–4.9M obj/s | 20–24% of ARC |
| 折叠率（batch 8→128） | 1.33×–1.64× | worker `process()` 调用数 |
| 生产者路径（batch=32） | **~105 ns/pair** | 从 ~155–180 降下来 |
| RootBag 修复前后 | 98 → 16 ns/pair | M11c |
| promotion 延迟（conservative） | 平的 ~3µs | 与 heap 大小无关 |
| promotion 延迟（reverse, 1M heap） | ~314× 于 conservative | M7 |

### 6.2 真实 VM：三后端（Release，`--gc-stats`）

对象数与字节数都是实测。`live` 在 teardown 丢根之后统计 —— 即「不可达」的对象。

**churn（20000 次分配）**

| 后端 | allocs | reclaimed | live | peakLive | heapBytes | reusedBytes |
|---|---|---|---|---|---|---|
| NoGC | 20000 | 0 | 20000 | 20000 | 900000 | 0 |
| ARC | 20000 | 20000 | **0** | 2 | **90** | 899910 |
| LOCAL-AARC | 20000 | 20000 | **0** | 23 | **1035** | 898965 |

**tree（100 棵深度 10 满二叉树 = 204700 对象，204600 条边）**

| 后端 | reclaimed | live | peakLive | heapBytes | reusedBytes |
|---|---|---|---|---|---|
| NoGC | 0 | 204700 | 204700 | 5526900 | 0 |
| ARC | 204700 | **0** | 4094 | **110538** | 5416362 |
| LOCAL-AARC | 204700 | **0** | 4136 | **111672** | 5415228 |

**cycle（2000 个双节点环 = 4000 对象）**

| 后端 | live | heapBytes | cycleReclaimed |
|---|---|---|---|
| NoGC | 4000 | 72000 | 0 |
| ARC | **4000（全漏）** | 72000 | 0 |
| **LOCAL-AARC** | **0** | **1152** | **4000** |

**cycle_big（200000 个双节点环 = 400000 对象）**

| 后端 | live | heapBytes | cycleReclaimed |
|---|---|---|---|
| ARC | 400000（全漏） | 7200000 | 0 |
| **LOCAL-AARC** | **0** | **69408** | **400000** |

**mixed（70% 树 / 20% 保留链表 / 10% 环）**

| 后端 | reclaimed | live | heapBytes | cycleReclaimed |
|---|---|---|---|---|
| NoGC | 0 | 216518 | 7794648 | 0 |
| ARC | 212516 | 4002（环漏掉） | 292968 | 0 |
| LOCAL-AARC | 212516 | **0** | **149760** | **4002** |

> 这张表就是整个集成阶段的目标：
> **ARC 漏掉的正好是环，AARC 一个不剩地收掉。**

### 6.3 M14：fixed cadence vs adaptive（`bench_gc`，7 reps + 2 warmup）

单位：LOCAL-AARC 的中位毫秒。括号里是同一次运行的 NoGC 基线。

| workload | NoGC | fixed 1024 | fixed 65536 | fixed 1M | **adaptive** |
|---|---|---|---|---|---|
| tree | ~80 | 303.7 | 109.2 | 101.3 | **102.2** |
| cycle_big | ~70 | 111.1 | 101.4 | 106.2 | **97.0** |
| mixed | ~105 | 383.3 | 134.6 | 124.1 | **136.6** |
| churn | ~15 | 17.4 | 17.7 | 15.5 | **15.6** |

**成功判据（全部满足）**：

```
tree        adaptive ≈ fixed 65536 / 1M    102.2 vs 109.2 / 101.3
cycle_big   adaptive ≈ 最优（而且是四者最快）97.0
mixed       adaptive 接近最优               136.6 vs 124.1
```

即：**adaptive 不需要 oracle 告诉它这是哪个 workload。**

### 6.4 成本归因：为什么 fixed cadence 是错的

**tree（Tarjan 什么都找不到）**

| | fixed 1024 | adaptive |
|---|---|---|
| scans | 5086 | **79** |
| scanVisited | 12,325,328 | **243,514** |
| cycleBytes | 0 | 0 |
| **cycleNs** | **196.7ms / 303.7ms = 65%** | **4.86ms / 102.2ms = 4.8%** |

→ scan 次数 **~64×↓**，visited **~51×↓**，环检测 CPU **~40×↓**，
回收效果零损失（本来就是 0）。

**cycle_big（yield ≈ 99%）**

| | adaptive |
|---|---|
| scans | 774 |
| scanVisited | 403,092 |
| cycleBytes | **7,193,160** |
| cycleNs | 15.5ms / 124ms |

→ threshold 被反馈压到 **512**，扫得更勤。

### 6.5 单次 Tarjan 停顿分布

这是「无全局 STW」这个说法真正的支撑数据
（**没有全局 STW，不等于没有局部同步工作**）：

| workload | scans | pause p50 | p95 | p99 | **max** |
|---|---|---|---|---|---|
| tree | 79 | 54.3µs | 100.1µs | 125.2µs | **194µs** |
| cycle_big | 774 | 16.3µs | 18.0µs | 19.3µs | **255µs** |
| mixed | 104 | 46.8µs | 90.4µs | 104.6µs | **205µs** |
| phase | 372 | 20.8µs | 31.8µs | 100.0µs | **187µs** |

**全场景最坏单次局部停顿 ≈ 0.32ms。**

### 6.6 phase-change：threshold 真的跟着 workload 走

`gc_phase.lx`：A 大树 → B 环 → C 大树 → D 环。
threshold 轨迹（`ops:threshold`，只记录变化）：

```
A (tree)    4096 → 8192 → 16384 → 32768 → 65536      ← 主动退避
B (cycle)   65536 → 32768 → 16384 → 8192 → 4096      ← 主动激进
C (tree)    4096 → 8192 → 16384 → 32768 → 65536      ← 又退避
D (cycle)   65536 → 32768 → 16384 → 8192
```

统计：`thrUp=8 thrDown=7`。
**这就是「它不是恰好适配三个 benchmark，而是真的对运行时行为做反馈」的证据。**

### 6.7 starvation bound 正确性

`gc_starvation.lx`：早早丢一个双节点环，然后 20000 次纯无环分配 ——
suspect pressure 和 heap pressure 两条路都够不着。

```
trigStarve     = 1      ← 只有 starvation bound 触发了
trigSuspect    = 0
trigHeap       = 0
cycleByPolicy  = 2      ← 那 2 个对象是被这次触发收掉的，不是 teardown
live           = 0
```

**小而安静的环不会永生。**

### 6.8 回归验证

- AARC 引擎测试套件 **10/10** 通过（`ctest`）
- 121 个 Loxis 程序：NoGC = ARC = LOCAL-AARC = 改动前 golden，**0 差异**
- `arc vs aarc`：**0 差异**
- Debug-only 寄存器不变量扫描（`LOXIS_GC_CHECK=1`）：全部程序零悬垂根

---

## 7. 踩过的坑（bug 清单）

按「发现它的东西」分类 —— 这张表本身就是方法论。

### 7.1 屏障顺序：acquire-before-release（VM 集成阶段）

编译器在每个调用点会把**所有** local 压到 data stack 溢出
（`Lowering.cpp` 的 spill-all）。所以 **spill slot 可能是某个对象唯一的引用**。

第一版 barrier 是 release-before-acquire：

```
PopR / setReg:  rootRemove(old)  →  rootAdd(new)
                         ↑ 计数掉到 0，对象被回收
                           → 然后 rootAdd 把一个死 handle 复活
```

症状（tree workload）：

```
ARC reclaimed = 225801  >  allocs = 204700     ← 回收数比分配数还多
ARC invalidOps = 14400
```

AARC 的数字看起来干净（它只标记、只在 drain point 收），但窗口是一样的。

修法：`setReg`/`setGlobal` 先加后减；`PopR` 先装进寄存器再丢 spill 引用。
抓它的工具是 debug-only 的寄存器不变量扫描。

### 7.2 反向表回收是 O(V × components)（quadratic）

```cpp
for (每个可回收 component)
    for (i : visited_)                  // ← 每个 component 重扫全表
        if (sccOf(i) == s) members.push_back(i);
```

scan 小而频繁时看不出来；adaptive 第一次扫描攒了一大堆时爆炸：

```
400k 对象 cycle workload @ fixed:1M
  before: 3310ms  (44.0×)
  after:   118ms  (1.47×)
```

修法：一遍分桶，`membersOf_` 跨 pass 复用。
**这个 bug 一直在，是 adaptive 把它照出来的。**

### 7.3 suspect 信号虚胖（M14c）

`pendingSuspects()` 返回的是 `suspectStack_.size()`，
但那个 stack 是**超集**（条目从不单独移除，clear+remark 会推重复）。

| workload | trigger 读到的 | 真实 distinct | 虚胖 |
|---|---|---|---|
| gc_phase | 1395 | 375 | 3.7× |
| 小树 phase | 1390 | 25 | **56×** |
| gc_tree | 1406 | 1406 | 1.0× |

修法：引擎维护 `activeSuspects_`，所有 `suspectMark_` 写入（**6 处**）
统一走 `setSuspect`/`clearSuspect`；`suspectStackEntries()` /
`staleSuspectEntries()` 保留原始数字做观测。
Debug checkpoint `assertActiveSuspects()` 在 `sweep()` 里做 O(capacity) 对账。

新测试套件 `test_suspects.cpp`（28 checks）钉住语义。
修复后 M14 数字**完全没变**（这四个 workload 上 starvation bound 主导，
padded count 从没跨过 threshold）—— 但这个修复保护的是低 threshold 区
（yield 反馈把 threshold 压到 512 之后，raw count 会误触发）。

### 7.4 引擎阶段（M5–M10）踩的其他坑

| bug | 症状 | 抓它的东西 |
|---|---|---|
| cv notify 不在锁内 | `awaitQuiesce` 偶发永久挂起 | 循环 20–50 次的并发测试 |
| `pending_` 下溢 | worker 忙等 | 代码审查 |
| 旧切面服务新 quiesce 票据 | 已释放的引用被当成活的 | 确定性 litmus |
| suspect ABA | release 下回收活着的 LOCAL cell | 断言 "cycle scan left its regime" |
| owner 生命周期 | 复用 owner id 的状态串味 | `OwnerToken` |
| 丢唤醒（worker 启动） | 早于首次 wait 的批次永远不醒 | 队列满/批量 flush 测试 |
| cycle collector 缺不动点 | 被零链级的联孤儿化的环永久泄漏 | 随机差分流（seed 7） |
| promote-all 是 O(live²) | 500K 填充时挂起 | 规模化 |
| RootBag tombstone 不计 load | 探针无限循环 | 多生产者 |
| RootBag 被撑到 6MB 而 size=0 | 98ns/pair | 分阶段归因计时 |
| arena freelist 单写者 | 段错误 | 多 LOCAL 引擎并发 |

### 7.5 一个反直觉的测量教训

> 裸 `__rdtsc()` 对被测代码**没有排序保证**，读数会显著偏小；
> 而且 1/64 采样会把计时器开销**乘以 64**。
> 两者都要处理，否则各阶段之和会超过墙钟。

（这也是为什么最后用的是分阶段归因 + 交替 A/B，而不是采样 profiler ——
MinGW 上没有可用的采样 profiler。）

---

## 8. 构建 / 测试 / 跑分

### 引擎（独立构建树，不碰编译器）

```bash
cd src/aarc
cmake -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 8
ctest --test-dir build            # 10 个测试
```

Debug 树：`cmake -B build-dbg -DCMAKE_BUILD_TYPE=Debug`（断言全开）。

关键编译开关：

| 开关 | 作用 |
|---|---|
| `AARC_ENABLE_STATS=0` | 关掉诊断计数器（干净 Release 测吞吐用） |
| `AARC_CONSERVATIVE=1` | 用仅正向表 + 可能连通分量 |
| `AARC_CHECK_INVARIANTS=1` | 打开 authority / regime 断言（小规模测试用） |
| `AARC_TEST_HOOKS=1` | 暴露测试钩子 |
| `AARC_INSTRUMENT=1` | 遍历计数器（`bench_diag_i`） |
| `AARC_LEGACY_POOL_CLOSURE=1` | 老的全池 closure，做 A/B |

### Loxis VM（三后端）

```bash
cmake -B build-mingw -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_COMPILER=<g++> -DCMAKE_MAKE_PROGRAM=<mingw32-make>
cmake --build build-mingw -j 8

# 跑程序，选后端
./build-mingw/Loxis.exe prog.lx --gc=nogc|arc|aarc --gc-stats [--slots=N]

# 环境开关
LOXIS_AARC_CADENCE=<n>    # 固定 cadence（绕开 adaptive）
LOXIS_GC_CHECK=1          # debug build 下的悬垂根扫描
```

### GC benchmark harness

```bash
./build-mingw/bench_gc prog.lx... [--reps=N] [--warmup=N] [--slots=N] \
                                 [--aarc-cycle=adaptive|fixed:N] \
                                 [--backends=nogc,arc,aarc]
```

输出的列：`median_ms / p25 / p75 / min / max / allocs / live / peakLive /
heapBytes / reusedBytes / cycleRcl / scans / scanVisited / cycleBytes /
cycleNs / errs`，外加一行跨后端输出一致性检查。

---

## 9. 已知限制与未做的事

### 明确的限制

1. **异步模式仍然远慢于 ARC**（~20–24%）。被设计接受为「共享状态才付的代价」，
   但 SHARED regime 本身没有被优化到 LOCAL 的水平
2. **v1 没有降级**：对象一旦 SHARED 就永远 SHARED
3. **保守分量方案会过度提升**：一个 LOCAL 对象碰到 SHARED 状态可能带走整个分量
4. **提升延迟在 reverse 方案下随 heap 增长**（conservative 方案已经解决）
5. **没有降级的 promotion 意味着长跑程序的 regime 分布会单调向 SHARED 漂移**

### 未做的事

1. **多 worker / work-stealing** —— 明确排在最后，因为先做只会放大锁竞争
2. **`alloc("int", 3)` 的 lowering 是错的** —— MIRCompiler 把 `alloc` 当裸 `halloc`
   处理，字符串索引被当成 size 用了。已用 `logicalSize` 让 padding 不再
   把越界写洗白，但 lowering 本身还没修
3. **采样 profiler** —— MinGW 上没有；用分阶段归因代替
4. **sharded/arena pool** —— 与 3/4 并行做
5. **`LocalAarcBackend` 的 YIELD_MIN_VISITED=256 是个常数** ——
   没有做成自适应

### 实验方法上的保留

- 本机跨 session 的绝对毫秒数摆动 ~2×，**任何缩放声明都不可靠**
- T≥4 的数据（core oversubscription + turbo/thermal）尤其不可信
- LTO/PGO 在 CLion 的 MinGW g++ 上跑不了（需要 MSVC 或开了 LTO 的 g++）

---

## 10. 可以写进论文的几句话

> AARC decouples reclamation into two time scales: zero-chain reclamation runs
> eagerly on every barrier, while cycle detection is gated by an adaptive
> trigger driven by suspect pressure, memory pressure, and a starvation bound.

> The execution location — inline on the owning mutator, or asynchronous on a
> shared worker — is chosen per object by its ownership, not by a global mode.
> This is why the single-threaded fast path carries no asynchronous tax: it is
> the complete cycle-safe engine, inline.

> Suspect pressure is measured as the number of currently marked distinct
> suspects, not the size of the lazy work stack.

> Adaptive cycle detection tracks workload phase: on an allocation-only
> workload the trigger backs off to one scan per 64K barriers (79 scans,
> 243K nodes visited, 4.5% of runtime); on a cyclic workload it becomes
> aggressive (774 scans, 403K nodes, 7.2 MB reclaimed, yield ≈ 99%).
> Neither was told which workload it was running.

> On a real interpreter with precise roots, heap barriers and the full cycle
> machinery, LOCAL-AARC lands within ~1.25× of an allocation-only baseline and
> reclaims every cycle that reference counting provably cannot — with a worst
> observed single collection pause of 0.32 ms and no global stop-the-world.

---

*AARC 不是「更快的 GC」。它是一个把「谁拥有这个对象」变成可观测事实、
并据此选择执行位置的 GC。*
