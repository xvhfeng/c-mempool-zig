# Zig Heap 内存管理 C 重写交付方案

## 交付目标

- 目标：用 C 重写当前 9 个 Zig allocator 源文件。
- 对外接口：统一 `Allocator` vtable，提供 `alloc/resize/remap/free`。
- C 标准：C11。
- 平台层：POSIX `mmap/munmap/mremap`，Windows `VirtualAlloc/VirtualFree`，Wasm `memory.grow`。
- 线程：C11 atomics + pthread mutex / Windows SRWLOCK。

## C 公共接口

```c
typedef struct HeapAlignment { size_t bytes; } HeapAlignment;

typedef struct HeapAllocator HeapAllocator;
typedef void *(*heap_alloc_fn)(void *ctx, size_t n, HeapAlignment align, uintptr_t ra);
typedef bool  (*heap_resize_fn)(void *ctx, void *ptr, size_t old_n, HeapAlignment align, size_t new_n, uintptr_t ra);
typedef void *(*heap_remap_fn)(void *ctx, void *ptr, size_t old_n, HeapAlignment align, size_t new_n, uintptr_t ra);
typedef void  (*heap_free_fn)(void *ctx, void *ptr, size_t n, HeapAlignment align, uintptr_t ra);

typedef struct HeapVTable {
    heap_alloc_fn alloc;
    heap_resize_fn resize;
    heap_remap_fn remap;
    heap_free_fn free;
} HeapVTable;

struct HeapAllocator {
    void *ctx;
    const HeapVTable *vt;
};
```

## 文件映射

- `ThreadSafeAllocator.zig` -> `thread_safe_allocator.c/.h`
- `arena_allocator.zig` -> `arena_allocator.c/.h`
- `PageAllocator.zig` -> `page_allocator.c/.h`
- `sbrk_allocator.zig` -> `sbrk_allocator.c/.h`
- `FixedBufferAllocator.zig` -> `fixed_buffer_allocator.c/.h`
- `memory_pool.zig` -> `memory_pool.c/.h`
- `debug_allocator.zig` -> `debug_allocator.c/.h`
- `WasmAllocator.zig` -> `wasm_allocator.c/.h`
- `SmpAllocator.zig` -> `smp_allocator.c/.h`
- 公共工具 -> `heap_common.h`, `heap_platform.c/.h`, `heap_atomic.h`

## ThreadSafeAllocator.zig

- L1：说明用途：包装非线程安全 allocator。
- L3：保存下游 allocator。
- L4：保存互斥锁，默认初始化。
- L6-L16：生成 allocator vtable，ctx 指向自身。
- L18-L24：`alloc` 加锁，转调 child `rawAlloc`。
- L26-L33：`resize` 加锁，转调 child `rawResize`。
- L35-L42：`remap` 加锁，转调 child `rawRemap`。
- L44-L51：`free` 加锁，转调 child `rawFree`。
- L53-L55：导入依赖和类型别名。
- C 注意点：锁必须包住完整 child 调用；禁止递归进入同一 allocator；vtable 静态常量。

## arena_allocator.zig

- L1-L5：导入 std、assert、mem、Allocator、Alignment。
- L7-L10：语义：批量释放；单个 free 只回退最近一次分配。
- L11-L14：`ArenaAllocator` 保存 child allocator 和 state。
- L15-L27：`State` 仅含 buffer 链表和当前 end index；`promote` 用 child allocator 恢复完整 allocator。
- L29-L39：返回 allocator vtable。
- L41-L45：`BufNode` 内嵌链表节点；`data` 是整块分配长度；定义节点对齐。
- L47-L49：`init` 初始化空 state。
- L51-L63：`deinit` 遍历所有节点，按原始 buffer 长度释放。
- L65-L77：`ResetMode`：全释放、保留容量、按上限保留。
- L78-L90：`queryCapacity` 统计所有 arena 可用容量，不含节点头。
- L91-L101：`reset` 契约：失败仍可用；`free_all` 恒成功。
- L102-L124：计算 reset 后目标容量。
- L125-L130：容量为 0：释放所有节点，清空 state。
- L131-L144：保留容量：释放除最后一个节点外的所有节点。
- L145-L148：先将 `end_index` 归零，保证失败后 arena 已重置。
- L149-L157：如果剩余节点大小匹配直接复用；否则尝试原地 resize。
- L158-L167：resize 失败则重新 alloc 新节点、释放旧节点、挂回链表。
- L172-L183：`createNode` 按 `prev_len + minimum + 50%` 增长，prepend 新节点，end_index 归零。
- L185-L216：`alloc` 从当前节点按 alignment 调整地址；空间不够先 resize 当前节点，失败再建新节点。
- L218-L241：`resize` 仅最近分配可增长；非最近分配只允许逻辑缩小。
- L243-L251：`remap` 等价于成功 resize 后返回原指针。
- L253-L266：`free` 仅最近分配回退 `end_index`，其他 free 无操作。
- L269-L306：测试 reset 预热、随机分配、保留 buffer。
- C 结构体：

```c
typedef struct ArenaBufNode { size_t data; struct ArenaBufNode *next; } ArenaBufNode;
typedef struct ArenaState { ArenaBufNode *first; size_t end_index; } ArenaState;
typedef struct ArenaAllocator { HeapAllocator child; ArenaState state; } ArenaAllocator;
```

- C 注意点：`BufNode` 和可用内存同块分配；指针偏移必须按 `align_forward`；reset 失败不能留下悬空链表。

## PageAllocator.zig

- L1-L16：导入平台依赖；Windows placeholder 常量。
- L17-L22：导出 vtable。
- L24-L28：`map` 入口；防止 `n + page_size` 溢出；取 alignment bytes。
- L29-L43：Windows 快路径：直接 `NtAllocateVirtualMemory`，满足 alignment 即返回；否则释放。
- L44-L56：Windows 过量 reserve placeholder，计算对齐地址和 prefix。
- L58-L70：释放 prefix/suffix placeholder，保留中间对齐区。
- L72-L85：提交中间 placeholder；失败则释放。
- L88-L94：POSIX 计算页对齐长度、过量映射长度和 mmap hint。
- L95-L103：POSIX `mmap` 匿名私有内存，并找对齐指针。
- L104-L110：释放对齐前/后的多余页。
- L111-L113：更新全局 mmap hint，返回结果。
- L116-L121：allocator `alloc` 断言 n>0 并调用 map。
- L123-L128：`resize` 调 `realloc(... may_move=false)`。
- L130-L135：`remap` 调 `realloc(... may_move=true)`。
- L137-L142：`free` 调 `unmap`。
- L144-L153：`unmap`：Windows release 整区；POSIX 按页对齐长度 `munmap`。
- L155-L159：`realloc` 规范化旧内存和新页对齐大小。
- L160-L178：Windows shrink 用 `MEM_RESET` 释放驻留；grow 超旧页对齐区失败。
- L180-L188：POSIX 若页对齐长度不变直接成功；支持 `mremap` 则使用。
- L190-L195：无 `mremap` 时 shrink 释放尾部页。
- L197-L198：不能 grow 返回失败。
- C 注意点：alignment 可能大于 page size；Windows 需要 placeholder 路径；POSIX `mremap` 可选；free 长度必须是用户旧长度。

## sbrk_allocator.zig

- L1-L8：导入依赖。
- L9-L17：模板 allocator，由外部 `sbrk(n)` 提供扩展堆能力。
- L18-L24：错误类型、最大值、bigpage 常量和数量。
- L26-L34：小对象 size class 和大对象 bigpage class。
- L35-L39：全局数组：每类 bump next、free list、大对象 free list。
- L41-L42：全局互斥锁。
- L43-L80：`alloc` 加锁；小对象优先 free list，再 bump 分配；页边界时申请 bigpage；大对象按 bigpage 分配。
- L82-L110：`resize` 不迁移，只允许保持同一小 size class 或同一大 bigpage class。
- L112-L120：`remap` 成功 resize 返回原指针。
- L122-L150：`free` 将小对象或大对象挂回对应 free list，next 指针写在槽尾。
- L152-L154：按字节数计算需要 bigpage 数。
- L156-L168：`allocBigPages` 复用 big free list，否则调用 sbrk。
- L172-L178：编译测试。
- C 注意点：free list 指针存储在 slot 尾部；`len + sizeof(size_t)` 使用饱和/溢出检查；全局锁必须覆盖所有数组。

## FixedBufferAllocator.zig

- L1-L6：导入依赖，`@This`。
- L8-L9：状态：end_index 和外部 buffer。
- L11-L16：初始化。
- L18-L29：普通 allocator vtable；和线程安全版本不能混用。
- L31-L44：lock-free thread-safe allocator，仅支持 alloc；resize/remap/free 禁用。
- L46-L52：判断 ptr/slice 是否属于 buffer。
- L54-L60：判断是否最近分配；alignment padding 导致可能假阴性。
- L62-L72：`alloc` 按 alignment 调整 end_index，越界失败。
- L74-L102：`resize`：非最近分配只允许缩小；最近分配可缩小/原地扩展。
- L104-L112：`remap` 成功 resize 返回原指针。
- L114-L128：`free`：只有最近分配回退 end_index。
- L130-L143：线程安全 alloc：CAS 更新 end_index。
- L145-L147：reset 清零 end_index。
- L149-L157：内部 contains 判断。
- L159-L230：测试基本分配、reset、realloc 复用、线程安全版本。
- C 结构体：

```c
typedef struct FixedBufferAllocator {
    _Atomic size_t end_index;
    unsigned char *buffer;
    size_t len;
} FixedBufferAllocator;
```

- C 注意点：普通接口不用 atomic 也可读写同字段；线程安全接口只支持 bump alloc；free 不清零内存。

## memory_pool.zig

- L1-L5：导入 allocator、alignment、MemoryPool。
- L6-L9：`Managed` 兼容别名。
- L11-L19：`Aligned` 按指定 alignment 创建池。
- L21-L24：`AlignedManaged` 兼容别名。
- L26-L33：Options：alignment、growable。
- L35-L47：`Extra` 主模板；自然 alignment 时归一化为 null。
- L48-L63：Pool 状态：arena_state、free_list；item_size 是 max(Node, Item)；item_alignment 是 max(item, node)。
- L64-L68：empty 常量。
- L70-L78：`initCapacity` 预热 num 个 item。
- L80-L84：deinit 释放 arena。
- L86-L91：转 managed。
- L93-L102：addCapacity 循环 allocNew 并压入 free list。
- L104-L126：reset 通过 arena reset 清空所有对象和 free list。
- L128-L140：create 优先 pop free list；growable 时新分配；非 growable 耗尽返回 OOM。
- L142-L147：destroy 将 item 置 undefined 并挂回 free list。
- L149-L154：allocNew 从 arena 分配 item_size。
- L158-L234：Managed 包装器持有 allocator，转调 unmanaged。
- L236-L380：测试唯一性、复用、预热失败、非 growable、自然/手动高对齐。
- C 结构体：

```c
typedef struct MemoryPool {
    ArenaState arena;
    void *free_list;
    size_t item_size;
    size_t item_alignment;
    bool growable;
} MemoryPool;
```

- C 注意点：C 无 comptime 泛型，使用 runtime `item_size/item_alignment`；destroy 只接受同池指针；reset 会使已发出指针全部失效。

## WasmAllocator.zig

- L1-L8：导入 wasm、math、allocator。
- L9-L16：编译期限制：仅 wasm32/wasm64 且 single-threaded。
- L18-L25：vtable 和 Error。
- L27-L40：bigpage、size class、big class 常量。
- L42-L46：全局 next/free/big_free 数组。
- L48-L82：alloc：小对象按 size class 使用 free list 或 bump；大对象用 bigpage；底层 `memory.grow`。
- L84-L110：resize：只允许同一小 slot 或同一大 bigpage class。
- L112-L120：remap 成功 resize 返回原地址。
- L122-L148：free：写槽尾 next 指针并入 free list。
- L150-L152：计算 bigpage 数。
- L154-L169：allocBigPages：复用大 free list，否则 `@wasmMemoryGrow`。
- L171-L326：测试小对象顺序/逆序 free、大对象、OOM、realloc、shrink、大小类边界、标准 allocator。
- C 注意点：该文件 C 重写需要 wasm32 target；用 `__builtin_wasm_memory_grow` 或宿主封装；单线程版无锁。

## SmpAllocator.zig

- L1-L29：说明：ReleaseFast + 多线程；singleton；每线程 freelist；大对象直接 PageAllocator。
- L30-L39：导入依赖。
- L40-L47：全局 allocator、threadlocal index。
- L49-L55：最多 128 线程槽；slab 至少 64KiB；size class 参数。
- L57-L72：Thread 元数据：cache line 对齐、mutex、每类 next/free。
- L73-L92：`Thread.lock`：先锁当前 thread_index，失败按 CPU count 轮转直到拿到锁。
- L94-L96：unlock。
- L99-L104：懒加载 CPU count，最多 128，CAS 发布。
- L106-L111：vtable。
- L113-L115：编译期要求非 single_threaded。
- L117-L124：alloc：大对象交给 PageAllocator。
- L126-L148：小对象：slot_size；先用 free list，再用当前 slab bump。
- L150-L157：搜索次数满后 mmap 新 slab，slab 对齐，设置 next。
- L159-L172：当前 thread 槽无资源时释放锁，尝试其他 thread 槽。
- L175-L185：resize：小对象必须同 class；大对象不能变小类，转 PageAllocator。
- L187-L197：remap：小对象同 class 返回原指针；大对象转 PageAllocator may_move。
- L199-L215：free：大对象 unmap；小对象写入 thread freelist。
- L217-L223：sizeClassIndex 和 slotSize。
- C 注意点：singleton 全局；`thread_local` 保存索引；free 可能进入任意线程槽，不保证回原线程；slab 对齐是算法前提。

## debug_allocator.zig

- L1-L23：功能列表：栈追踪、double free、leak、地址不复用、最小后端分配、释放驻留内存、跨平台、编译期配置；代价慢且浪费。
- L25-L82：设计：小对象按 page bucket；slot 使用位图；bucket 链表用于 leak；大对象直接 backing allocator，元数据在 hash map；resize 不跨大小类。
- L83-L110：导入依赖；默认 page size 和默认栈帧数。
- L112-L170：Config：stack frames、memory limit、safety、thread_safe、MutexType、never_unmap、retain_metadata、verbose_log、backing zero、resize stack trace、canary、page_size。
- L172-L186：DebugAllocator 类型；字段：backing allocator、small buckets、大对象表、内存限制字段、mutex。
- L187-L197：编译期计算每个小类 slot count；page_size 必须 2 的幂。
- L199-L230：page alignment、SlotIndex、mutex init、DummyMutex、stack trace 存储大小、小类数量和最大小对象。
- L232-L236：bucket 指针比较函数。
- L238-L269：LargeAlloc 元数据：bytes、requested_size、stack traces、freed、alignment；支持 dump/get/capture trace。
- L270：LargeAllocTable 使用 unmanaged hash map。
- L272-L335：BucketHeader 布局：allocated/freed count、prev/next、canary；从 page 求 bucket；usedBits、requestedSizes、aligns、stackTracePtr、captureStackTrace。
- L337-L347：allocator vtable。
- L349-L364：读取 bucket stack trace。
- L366-L390：计算 bucket 内 metadata 起始位置和总大小。
- L392-L414：编译期二分计算某 size class 一个 page 可容纳 slot 数。
- L416-L422：used bits 数量和字节数。
- L424-L457：遍历 bucket 位图，输出 leak 日志。
- L459-L489：detectLeaks 遍历所有 small bucket 和 large table，返回 leak 数。
- L491-L502：retain_metadata 下释放被 never_unmap 故意保留的大对象。
- L504-L514：flushRetainedMetadata 清理已 free 的大对象 metadata。
- L516-L529：deinit / deinitWithoutLeakChecks。
- L531-L534：收集 stack trace，尾部清零。
- L536-L554：报告 double free：alloc、first free、second free 三段 trace。
- L556-L676：resizeLarge：查表、检测 invalid/double free/size mismatch、拒绝 large->small、内存限额、调用 rawResize/rawRemap、更新 metadata 和 hash key。
- L678-L739：freeLarge：查表、检测 double free/size mismatch、按 never_unmap 决定是否释放、更新内存限额、日志、删除或标记 metadata。
- L741-L846：alloc：锁；内存限额；大对象分配并入 hash；小对象从当前 bucket 分配 slot；没有 bucket 时申请 page 并初始化 metadata。
- L848-L865：resize：按旧 size class 分派 large/small。
- L867-L884：remap：large 可移动，small 同类返回原指针。
- L886-L1005：free small：定位 bucket/slot；canary 校验；double free 检测；size/alignment mismatch 日志；更新限额；记录 free trace；清 used bit；bucket 全空则摘链并可释放 page。
- L1007-L1100：resizeSmall：safety 校验 used bit、size、alignment；拒绝跨 size class；更新限额、清 undefined 尾部、日志、requested size、resize trace。
- L1104-L1107：TraceKind 枚举。
- L1109-L1479：测试：小/大分配、OOM、realloc、shrink、大对象增长、跨小大类、alignment、mutex override、非 page backing、内存上限、requested size 统计、retain metadata/never unmap。
- C 结构体：

```c
typedef struct DebugConfig {
    size_t stack_trace_frames;
    bool enable_memory_limit, safety, thread_safe;
    bool never_unmap, retain_metadata, verbose_log;
    bool backing_allocator_zeroes, resize_stack_traces;
    uintptr_t canary;
    size_t page_size;
} DebugConfig;

typedef struct DebugBucket {
    uint32_t allocated_count;
    uint32_t freed_count;
    struct DebugBucket *prev, *next;
    uintptr_t canary;
} DebugBucket;

typedef struct DebugLargeAlloc {
    unsigned char *ptr;
    size_t len;
    size_t requested_size;
    uintptr_t stack_addresses[2][HEAP_DEBUG_MAX_FRAMES];
    bool freed;
    HeapAlignment alignment;
} DebugLargeAlloc;

typedef struct DebugAllocator {
    HeapAllocator backing;
    DebugBucket **buckets;
    HeapHashMap large_allocations;
    size_t total_requested_bytes;
    size_t requested_memory_limit;
    HeapMutex mutex;
    DebugConfig config;
} DebugAllocator;
```

- C 注意点：Zig comptime 配置改为 runtime config；`small_bucket_count/slot_counts` 初始化时计算；hash map 需要开放寻址或链式表；stack trace 用 `backtrace()`/`RtlCaptureStackBackTrace`，不可用则置 0；bucket metadata 必须从 page 尾部反推。

## C 重写核心流程

### alloc

```c
void *heap_alloc(HeapAllocator *a, size_t n, HeapAlignment align) {
    return a->vt->alloc(a->ctx, n, align, 0);
}
```

### realloc

```c
void *heap_realloc(HeapAllocator *a, void *p, size_t old_n, HeapAlignment align, size_t new_n) {
    void *q = a->vt->remap(a->ctx, p, old_n, align, new_n, 0);
    if (q) return q;
    q = a->vt->alloc(a->ctx, new_n, align, 0);
    if (!q) return NULL;
    memcpy(q, p, old_n < new_n ? old_n : new_n);
    a->vt->free(a->ctx, p, old_n, align, 0);
    return q;
}
```

### size class

```c
static inline size_t ceil_pow2(size_t x);
static inline size_t log2_floor(size_t x);
static inline size_t align_forward(size_t x, size_t a) { return (x + a - 1) & ~(a - 1); }
```

## C 目录结构

```text
include/
  heap_allocator.h
  heap_common.h
  page_allocator.h
  arena_allocator.h
  fixed_buffer_allocator.h
  memory_pool.h
  thread_safe_allocator.h
  debug_allocator.h
  smp_allocator.h
  sbrk_allocator.h
  wasm_allocator.h
src/
  heap_common.c
  heap_platform.c
  page_allocator.c
  arena_allocator.c
  fixed_buffer_allocator.c
  memory_pool.c
  thread_safe_allocator.c
  debug_allocator.c
  smp_allocator.c
  sbrk_allocator.c
  wasm_allocator.c
tests/
  test_arena.c
  test_fixed_buffer.c
  test_memory_pool.c
  test_page.c
  test_debug.c
  test_smp.c
  test_sbrk.c
  test_wasm.c
```

## 实现顺序

1. `heap_allocator.h`：vtable、alignment、公共 realloc。
2. `heap_common.c`：pow2、log2、align、overflow 检查。
3. `page_allocator.c`：平台页分配。
4. `fixed_buffer_allocator.c`：最小 allocator 和测试。
5. `arena_allocator.c`：依赖 fixed/page 均可测。
6. `memory_pool.c`：基于 arena。
7. `thread_safe_allocator.c`：包装器。
8. `sbrk_allocator.c` / `wasm_allocator.c`：同一 size-class 逻辑拆公共 helper。
9. `smp_allocator.c`：多线程 slab。
10. `debug_allocator.c`：最后实现，依赖 hash map、stack trace、平台层。

## 测试清单

- fixed：越界 OOM、reset 覆盖、最近分配 resize/free、非最近分配 shrink、CAS alloc。
- arena：批量释放、retain capacity、retain limit、alignment、最近分配 shrink/grow。
- page：alignment 大于页、shrink、grow fail、remap may_move、Windows placeholder。
- pool：对象唯一性、destroy 复用、预热、非 growable、alignment。
- sbrk/wasm：小对象顺序/逆序 free、大对象 free list、同 class resize、OOM。
- smp：多线程并发 alloc/free、slot class 不变 resize、大对象 page allocator。
- debug：leak、double free、invalid free、size mismatch、alignment mismatch、memory limit、retain metadata、never unmap。

## 关键风险

- Zig `Allocator` 的 `resize/remap/free` 传入旧 slice；C 必须显式传 `old_n`。
- Zig comptime 泛型在 C 中必须转为 runtime 配置或宏生成。
- DebugAllocator bucket 从 page 反推 metadata，page alignment 不满足会误判。
- SmpAllocator 和 SbrkAllocator free list 指针写入用户内存，释放后读写是设计行为。
- Arena/FixedBuffer 的 free 不是通用 free，只回退栈顶分配。
- Wasm allocator 仅单线程；多线程 Wasm 不能直接复用。
