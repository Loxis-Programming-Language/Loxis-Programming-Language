你正在一个现有的 C++ 项目中工作。你的任务是为 Loxis 编程语言实现一个名为 **AARC (Asynchronous ARC)** 的实验性混合垃圾回收器。

AARC 的总体设计是：

* 主线程使用低开销的异步引用计数（Async ARC）
* 引用计数更新通过线程本地 SPSC 队列提交给后台 GC worker
* 后台维护引用计数、可疑对象集合与循环引用信息
* 使用后台 cycle detection / God Object 机制回收循环引用
* 后续加入 optimistic heap snapshot，使大部分 cycle scanning 无需暂停 mutator
* 当 snapshot 已过期或存在竞争时，仅针对候选 cycle 使用短暂 STW revalidation
* 整个系统必须首先保证 correctness，再考虑性能

目标语言标准为 **C++17**。

除项目当前已有依赖外，不要引入新的第三方依赖。优先使用标准库。

---

# 0. 开始工作前

不要立即生成大量代码。

首先：

1. 阅读当前 repository 的目录结构。
2. 找到：

   * heap / allocator
   * object representation
   * VM / bytecode dispatch loop
   * thread/runtime abstraction
   * GC 或 memory management 相关现有代码
   * build system
   * test framework
3. 搜索是否已经存在：

   * incref / decref
   * object ID / handle
   * object traversal / outgoing references
   * safepoint
   * allocator freelist
   * worker thread
   * GC roots
4. 确认下面的设计与现有 Loxis runtime 是否冲突。

如果规格中的文件名与现有代码结构明显不一致：

**遵循现有项目结构，而不是为了匹配规格强行重构项目。**

如果仓库中已经存在等价组件：

**扩展/复用现有组件，不要重新实现第二套。**

然后给出一个非常简短的 implementation plan，并立即开始实现。

不要只给建议。

你的任务是实际修改代码。

---

# 1. Correctness 原则

以下原则优先级高于具体 API 名称。

## 1.1 不允许错误回收

任何情况下：

> false negative cycle detection 可以暂时接受；
> false positive reclamation 不可接受。

如果无法证明对象或 cycle 已不可达，不得释放。

## 1.2 引用更新必须有明确顺序

所有 mutator 产生的引用关系变化必须能与 ARC 更新建立确定的 happens-before / sequencing 关系。

不能因为异步处理导致：

* DECREF 先于对应 INCREF 被观察
* 已重新获得引用的对象被后台释放
* snapshot scanner 使用逻辑上不可能存在的 object graph

## 1.3 对象生命周期和 object ID 必须防止 ABA

如果 object ID/cell 可以被重用，则后台队列里滞后的：

* INCREF
* DECREF
* cycle candidate
* snapshot edge

不得错误作用于同一 cell 后来创建的新对象。

如果现有 runtime 没有 generation/version 机制，请设计一个最小可靠方案，例如：

* generation counter
* versioned object handle
* deferred cell reuse

选择与当前代码最兼容的方案。

不要忽略这个问题。

---

# 2. Object / Heap 基础约束

Loxis 当前概念模型中，一个 heap cell 目标尺寸约为 **9 bytes**。

逻辑布局类似：

```cpp
tag      : 1 byte
id       : 4 bytes
data     : 4 bytes
```

但是：

不要假设普通 C++ struct 自然布局一定为 9 bytes。

如果实际 runtime 要求严格 9-byte storage：

* 使用明确的 packed/storage representation
* 或 byte array + accessor
* 并添加 static_assert

不要为了方便直接创建一个由于 alignment 变成 12/16 bytes 的结构然后仍声称它是 9-byte cell。

如果当前 Loxis 已经有 Object/Cell representation，则优先适配现有结构。

---

# 3. 目标代码结构

理想结构如下，但可以根据现有 repository 适配：

```text
src/aarc/
├── core/
│   ├── heap.hpp
│   ├── heap.cpp
│   ├── object.hpp
│   ├── spsc_queue.hpp
│   └── gc_ops.hpp
│
├── arc/
│   ├── ref_table.hpp
│   ├── ref_table.cpp
│   ├── arc_api.hpp
│   ├── arc_api.cpp
│   ├── ecr.hpp
│   ├── ecr.cpp
│   ├── background_worker.hpp
│   └── background_worker.cpp
│
├── cycle/
│   ├── suspect_set.hpp
│   ├── suspect_set.cpp
│   ├── cycle_detector.hpp
│   ├── cycle_detector.cpp
│   ├── ecr_calculator.hpp
│   ├── ecr_calculator.cpp
│   ├── cycle_manager.hpp
│   ├── cycle_manager.cpp
│   ├── cycle_reclaimer.hpp
│   └── cycle_reclaimer.cpp
│
├── scheduler/
│   ├── task_queue.hpp
│   ├── scheduler.hpp
│   ├── scheduler.cpp
│   ├── adaptive_scheduler.hpp
│   ├── adaptive_scheduler.cpp
│   ├── safepoint.hpp
│   └── safepoint.cpp
│
├── snapshot/
│   ├── snapshot.hpp
│   ├── snapshot.cpp
│   ├── snapshot_scanner.hpp
│   ├── snapshot_scanner.cpp
│   ├── snapshot_validator.hpp
│   └── snapshot_validator.cpp
│
└── tests/
    ├── test_heap.cpp
    ├── test_spsc.cpp
    ├── test_ref_table.cpp
    ├── test_cycle_detector.cpp
    ├── test_snapshot.cpp
    ├── benchmark_no_cycle.cpp
    ├── benchmark_with_cycle.cpp
    └── benchmark_concurrent.cpp
```

再次强调：

如果 repository 已经存在更合理的位置，使用现有布局。

---

# 4. Phase 0 — 基础设施

先完成 Phase 0，然后立即 build + test。

不要一次性跳到 Phase 4。

## 0.1 Fixed-size heap allocator

实现固定 cell allocator。

需要支持：

```cpp
allocate()
deallocate(ptr)
iterate_cells(callback)
```

要求：

* cell allocation/deallocation 正确
* freelist 不损坏
* 不 double free
* iteration 不访问已释放/无效 memory
* 如需要并发访问，明确 synchronization policy
* allocator metadata 不强行塞进 9-byte user payload，除非设计明确允许

如果项目已有 allocator，适配现有 allocator，而不是复制一套。

## 0.2 Object representation

提供安全的：

```cpp
get_tag()
set_tag()
get_id()
```

以及 GC 所需的最低限度 metadata。

如果需要 generation/version，请在这里或对应 side table 中实现。

## 0.3 SPSC Queue

实现固定容量 lock-free SPSC ring buffer。

接口类似：

```cpp
bool push(const GCMessage&);
bool pop(GCMessage&);
bool empty() const;
size_t size() const;
```

要求：

* 真正只支持一个 producer + 一个 consumer
* 使用 std::atomic
* 正确使用 acquire/release ordering
* 不使用 mutex 模拟“lock-free”
* 明确处理 queue full
* 不静默丢 GC event

如果每个 mutator thread 一个 queue，则 producer 是该 mutator，consumer 是 GC worker。

不要对一个 SPSC queue 使用多个 producer。

## 0.4 GC Messages

不要只定义裸 enum。

优先定义类似：

```cpp
enum class GCOp {
    IncRef,
    DecRef,
    ...
};

struct GCMessage {
    GCOp op;
    ObjectHandle object;
    // 必要时包含 source / target / generation / edge info
};
```

消息必须包含后台正确处理更新所需的信息。

---

# 5. Phase 1 — Async ARC

Phase 0 测试通过后再继续。

## 1.1 RefTable

实现集中式 reference count table。

概念接口：

```cpp
increment(handle)
decrement(handle)
get_count(handle)
```

要求：

* object identity 安全
* 不出现 unsigned underflow
* DECREF 到 0 时不能在存在 pending reference update 的情况下直接错误释放
* 明确 ref count 更新由哪个线程拥有

如果只有 background worker 修改 ref table，可以优先使用 single-writer 设计，而不是无意义地给所有字段加 atomic。

只有确实存在多线程直接访问时再使用 atomic。

## 1.2 Mutator ARC API

实现：

```cpp
aarc_incref(...)
aarc_decref(...)
```

fast path 应尽量只：

* 构造 GC message
* 写入 thread-local queue

不要在正常 mutator path 中进行 expensive graph scanning。

处理 queue full：

不允许丢消息。

可以：

* flush
* assist GC
* slow path
* 临时同步消费

选择一个 correctness-safe 策略。

## 1.3 Background Worker

worker 应：

* 扫描/消费各 mutator SPSC queue
* 按正确顺序更新 ref table
* 产生 reclaim candidate
* 处理 shutdown
* 不 busy-spin 消耗一个完整 CPU core，除非 benchmark mode 明确要求

接口可类似：

```cpp
worker_loop()
process_queue(thread_id)
```

## 1.4 Reclaimer

当对象确认 refcount 为 0 且安全释放时：

1. 枚举该对象 outgoing references
2. 对 outgoing targets 产生对应 DECREF / reference removal
3. 清理 GC metadata
4. 最后归还 allocator cell

不要简单：

```cpp
if (refcount == 0)
    free(cell);
```

而忽略对象内部引用。

## 1.5 ECR / external incoming references

这里不要根据“incref/decref”凭空猜测一条引用是否来自 cycle 外部。

ECR 必须基于实际 object graph edge information。

必须区分：

* root → object
* object A → object B
* candidate cycle 内部 edge
* candidate cycle 外部 edge

如果 runtime 当前没有 write barrier / edge update API，请新增最低限度接口，例如：

```cpp
aarc_add_edge(source, target)
aarc_remove_edge(source, target)

aarc_add_root(target)
aarc_remove_root(target)
```

或者与现有 object field assignment path 集成。

ECR 的定义应该是：

> 对一个 cycle/candidate component，从该 component 外部进入 component 的有效引用数量。

不要把普通总 refcount 直接当作 ECR。

---

# 6. Phase 2 — Cycle Detection

Phase 1 正确工作后再实现。

## 2.1 Suspect Set

当某个对象可能成为 cyclic garbage 时加入 suspect set。

不要简单规定：

```text
external_incoming == 0 => 一定是垃圾
```

它只是 candidate。

接口：

```cpp
mark_suspect(handle)
unmark_suspect(handle)
is_suspect(handle)
get_suspects()
```

避免重复插入。

## 2.2 Graph traversal / cycle detector

实现基于 object graph 的 DFS。

可以使用 WHITE / GRAY / BLACK。

但注意：

检测到 DFS back-edge 不代表“DFS 当前路径上的所有东西都属于同一个可回收环”。

必须正确求出 cycle / strongly connected component。

如果实现简单 DFS 容易产生错误成员集合，优先使用：

* Tarjan SCC
* Kosaraju
* 或其它明确正确的 SCC 算法

对于 GC correctness，我更偏好 SCC，而不是仅仅找“回到起点”。

接口可以保持：

```cpp
detect_cycle(start)
get_cycle_members()
```

但内部应保证得到的是正确 component。

## 2.3 ECR Calculation

对 SCC/component 计算：

```text
incoming edges from outside component
```

包括：

* root references
* other live objects
* VM/runtime handles

只有 external incoming == 0 的 SCC 才可能回收。

## 2.4 Cycle Manager

维护 cycle metadata，例如：

```cpp
CycleId
members
external_ref_count
version
state
```

支持概念上的：

```cpp
merge_cycle(...)
split_cycle(...)
get_cycle_ref_count(...)
```

但是不要为了 API 清单强行设计昂贵的 eager merge/split。

如果重新扫描 SCC 比维护动态 split 更简单可靠，可以采用 lazy invalidation + rescan。

正确性优先。

## 2.5 Cycle reclamation

在回收一个 component 前必须再次确认：

* 所有成员 identity/generation 仍一致
* 没有新的 external incoming references
* object graph 没有使 candidate 失效
* 没有 snapshot/version race

然后才可以统一回收。

回收过程中注意 outgoing edges 指向 component 外部对象时需要更新这些对象的引用计数。

---

# 7. Phase 3 — Scheduler

在 GC correctness 已有测试覆盖后实现。

## 3.1 Task Queue

后台任务至少可以表示：

* ARC queue processing
* suspect scanning
* snapshot scanning
* reclamation
* validation

不要过度抽象。

## 3.2 Collection Task Scheduler

支持：

* task priority
* incremental work budget
* pause/resume（如果确实需要）
* bounded work slices

目标是避免一次 cycle scan 长时间占用 worker。

优先使用“work budget”而不是复杂的实时调度框架。

## 3.3 Adaptive Scheduler

根据：

* pending ARC queue length
* allocation rate
* suspect count
* worker utilization

调整：

* GC worker 数
* scan interval
* incremental scan budget

第一版保持策略简单、可测试。

不要做不可解释的复杂 heuristic。

## 3.4 Safepoint

找到 VM bytecode dispatch loop 或等价 mutator polling point。

提供：

```cpp
request_safepoint()
wait_for_safepoint()
resume_from_safepoint()
```

要求：

* 不 deadlock
* 不在持有会被 GC 请求的 runtime locks 时永久等待
* 正确记录 participating mutator threads
* shutdown/thread creation 与 safepoint 协议兼容

Safepoint 主要用于：

* rare GC mode transition
* snapshot revalidation fallback
* runtime teardown

正常 ARC path 不应频繁 STW。

---

# 8. Phase 4 — Optimistic Snapshot

这是 AARC 的关键部分。

目标：

cycle scanning 大部分时间在 stable snapshot 上进行，而不是阻塞 mutator。

## 4.1 Snapshot representation

snapshot 不需要复制全部 object payload。

只复制 GC 所需的 graph information：

* object identity
* generation
* outgoing edges
* root information
* 必要 GC metadata
* snapshot version

优先 shallow graph snapshot。

## 4.2 Graph versioning

每次会影响 GC reachability 的 mutation，例如：

```text
root add/remove
object edge add/remove
object destruction/reuse
```

必须更新相应 version。

不要只维护一个随便 increment 的全局数字而没有明确 synchronization semantics。

可以先实现全局 monotonically increasing epoch/version。

如果 contention 后续成为问题，再优化。

## 4.3 Snapshot Scanner

在 snapshot 上运行 Phase 2 的 SCC/cycle detection。

scanner 不得 dereference 已经可能被 allocator 重用的 raw pointer。

snapshot 中使用稳定 identity / handle。

## 4.4 Validation

candidate reclaim 前：

如果当前 graph version 与 snapshot version 一致：

可以进入最终安全检查和 reclaim。

如果不一致：

**不得直接 reclaim。**

可以：

1. 放弃 candidate，下轮重扫；
2. 或进入 targeted STW revalidation。

第一版可以优先选择“版本变化则放弃”，因为最安全。

然后再实现 STW fallback。

## 4.5 STW fallback

当 candidate 频繁 invalidated 或需要立即回收时：

1. request safepoint
2. 等待所有 mutator 到达 safepoint
3. 只重新验证 candidate SCC/cycle
4. 如果仍无 external incoming refs，则回收
5. resume mutators

不要默认 full-heap GC。

---

# 9. Tests

每完成一个 Phase，都必须：

1. build
2. run existing tests
3. run new tests
4. 修复失败
5. 确认无 warning/UBSan/ASan 问题（如果项目已有 sanitizer 配置）

不要在测试失败时继续堆下一阶段代码。

至少增加：

## Heap

* allocate/free
* reuse
* full capacity
* iteration
* double-free protection/assertion
* generation reuse safety

## SPSC

* FIFO ordering
* wrap-around
* queue full
* producer/consumer concurrent stress
* TSAN-friendly behavior

注意：

SPSC 测试只能有一个 producer + 一个 consumer。

不要写“多 producer 测 SPSC”。

## Refcount

* incref/decref
* zero transition
* nested object release
* delayed queue processing
* object reuse / stale message protection

## Cycle detection

至少：

```text
A -> B -> A
```

```text
A -> B -> C -> A
```

```text
root -> A -> B -> A
```

后者不得回收。

还要测试：

```text
A -> B -> A
C -> B
```

只要 C 仍 live，该 component 不得错误回收。

以及多个 SCC 相连：

```text
A <-> B -> C <-> D
```

确认 SCC membership 正确。

## Snapshot

测试：

1. 创建 cycle
2. take snapshot
3. scanner 找到 candidate
4. mutator 修改 edge/root
5. snapshot validation 必须失败
6. candidate 不得被回收

再测试无 mutation 时可以成功回收。

## Concurrency stress

并发进行：

* object allocation
* edge mutation
* root mutation
* incref/decref
* cycle creation/destruction

检查：

* crash
* UAF
* double free
* refcount underflow
* leaked reclaimable cycles
* stale handle corruption

---

# 10. Benchmarks

增加 benchmark，但不要伪造结论。

目标 workload：

### No-cycle

约 1,000,000 个 object allocation/reference operations。

比较：

* synchronous/basic ARC baseline
* AARC

记录：

* operations/sec
* allocation throughput
* mutator overhead
* GC worker CPU use

目标参考：

AARC mutator throughput 尽量达到 baseline ARC 的 90% 以上。

但如果达不到：

记录真实结果，不要为了通过测试修改 benchmark threshold 或伪造数字。

### Cycle-heavy

约 10% 对象参与 cycle。

验证：

* cyclic garbage 最终被释放
* steady-state heap 不无限增长
* pause distribution

### Concurrent

例如 8 mutator threads（如果 runtime 支持）。

记录：

* p50 / p95 / p99 pause
* max pause
* throughput
* GC CPU usage

“低于 1ms”是优化目标，不是 correctness test。

不要声称达到 Go/Java 80% 性能，除非 repository 里真的存在可复现的同机 baseline benchmark。

---

# 11. 实现方式

按照以下节奏执行：

```text
Phase 0
→ build/test/fix
→ Phase 1
→ build/test/fix
→ Phase 2
→ build/test/fix
→ Phase 3
→ build/test/fix
→ Phase 4
→ build/test/fix
→ benchmark
```

不要每完成一个小文件就停下来询问我。

只要没有真正阻塞实现的信息缺失，就继续工作。

遇到局部设计不明确时：

1. 查看现有代码；
2. 选择最小、最保守、最符合现有 architecture 的实现；
3. 在代码中留下简短注释；
4. 继续。

不要因为细节不确定而停下来让我选择 A/B/C。

---

# 12. 不要做的事情

不要：

* 重写整个 VM
* 为 AARC 引入大型框架
* 无必要修改 public language semantics
* 引入 shared_ptr 作为 GC 实现
* 使用 mutex 把所有 runtime 操作全局串行化
* 把 SPSC 偷换成 mutex queue
* 忽略 object reuse / ABA
* 在 snapshot version 已变化时继续 reclaim
* 仅凭 DFS back edge 判断整个 subtree 都是 cycle
* 将 total refcount 等同于 external cycle refcount
* 在 queue full 时丢弃 INCREF/DECREF
* 为了让测试 PASS 而削弱测试
* 注释掉失败测试
* 用 sleep 修 concurrency bug
* 捕获异常后静默忽略
* 留下大量 TODO 代替实际实现
* 在没有测量的情况下宣称“lock-free / wait-free / <1ms”

---

# 13. Coding style

遵循 repository 已有 style。

要求：

* C++17
* RAII
* const correctness
* 明确 ownership
* 尽量避免裸 owning pointer
* atomic memory ordering 写清楚设计原因
* concurrency-sensitive code 添加简洁注释
* public API 尽量小
* 不做无关格式化
* 不做无关重构

对于 memory ordering：

不要无脑全部使用 `memory_order_seq_cst`。

但如果当前阶段无法证明更弱 ordering 正确，可以先使用更保守 ordering，之后再优化。

Correctness > micro-optimization。

---

# 14. 最终交付

完成实现后，不要只说“done”。

给我一个简洁报告，包括：

```text
AARC implementation status

Phase 0: ...
Phase 1: ...
Phase 2: ...
Phase 3: ...
Phase 4: ...

Files added:
...

Files modified:
...

Important design decisions:
...

Correctness protections:
- ABA:
- queue overflow:
- cycle validation:
- snapshot invalidation:
- safepoint:

Tests:
<actual commands>
<actual results>

Benchmarks:
<actual measured results>

Known limitations:
...

Recommended next optimization:
...
```

所有测试和 benchmark 数字必须来自你实际运行得到的结果。

不要虚构执行结果。

现在开始：

先检查 repository 和现有 Loxis runtime，然后按照 Phase 0 → Phase 4 顺序直接实现 AARC。
