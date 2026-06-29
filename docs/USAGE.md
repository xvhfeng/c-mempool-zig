# c_str 使用说明书

## 构建

```sh
cd c_str
make test
```

## 公共模型

所有 allocator 使用同一接口：

```c
typedef struct HeapAllocator {
    void *ctx;
    const HeapVTable *vtable;
} HeapAllocator;
```

通用函数：

```c
void *heap_alloc(HeapAllocator a, size_t n, HeapAlignment align);
bool heap_resize(HeapAllocator a, void *p, size_t old_n, HeapAlignment align, size_t new_n);
void *heap_remap(HeapAllocator a, void *p, size_t old_n, HeapAlignment align, size_t new_n);
void heap_free(HeapAllocator a, void *p, size_t n, HeapAlignment align);
void *heap_realloc(HeapAllocator a, void *p, size_t old_n, HeapAlignment align, size_t new_n);
```

规则：

- `old_n` 必须是原分配长度。
- `align.bytes` 必须是 2 的幂。
- `heap_remap` 允许移动；`heap_resize` 不允许移动。
- `heap_realloc` 先 remap，失败则 alloc-copy-free。

## PageAllocator

接口：

```c
HeapAllocator heap_page_allocator(void);
void *heap_page_map(size_t n, HeapAlignment alignment);
void heap_page_unmap(void *ptr, size_t len);
void *heap_page_realloc(void *ptr, size_t old_len, size_t new_len, bool may_move);
```

用途：直接使用系统页分配。POSIX 使用 `mmap/munmap/mremap`，Windows 使用 `VirtualAlloc/VirtualFree`。

示例：

```c
HeapAllocator a = heap_page_allocator();
void *p = heap_alloc(a, 4096, heap_align_from(4096));
heap_free(a, p, 4096, heap_align_from(4096));
```

## FixedBufferAllocator

接口：

```c
void heap_fixed_buffer_init(HeapFixedBufferAllocator *self, void *buffer, size_t len);
HeapAllocator heap_fixed_buffer_allocator(HeapFixedBufferAllocator *self);
HeapAllocator heap_fixed_buffer_thread_safe_allocator(HeapFixedBufferAllocator *self);
void heap_fixed_buffer_reset(HeapFixedBufferAllocator *self);
bool heap_fixed_buffer_owns_ptr(HeapFixedBufferAllocator *self, void *ptr);
bool heap_fixed_buffer_owns_slice(HeapFixedBufferAllocator *self, void *ptr, size_t len);
bool heap_fixed_buffer_is_last(HeapFixedBufferAllocator *self, void *ptr, size_t len);
```

行为：

- 从固定 buffer bump 分配。
- 普通接口支持 alloc/resize/remap/free。
- free 只回退最近一次分配。
- thread-safe 接口 lock-free，只支持 alloc。

示例：

```c
unsigned char buf[1024];
HeapFixedBufferAllocator fba;
heap_fixed_buffer_init(&fba, buf, sizeof(buf));
HeapAllocator a = heap_fixed_buffer_allocator(&fba);
void *p = heap_alloc(a, 64, heap_align_from(8));
heap_free(a, p, 64, heap_align_from(8));
```

## ArenaAllocator

接口：

```c
void heap_arena_init(HeapArenaAllocator *self, HeapAllocator child);
void heap_arena_deinit(HeapArenaAllocator *self);
HeapAllocator heap_arena_allocator(HeapArenaAllocator *self);
size_t heap_arena_query_capacity(HeapArenaAllocator *self);
bool heap_arena_reset(HeapArenaAllocator *self, HeapArenaResetMode mode);
```

Reset 模式：

```c
HEAP_ARENA_FREE_ALL
HEAP_ARENA_RETAIN_CAPACITY
HEAP_ARENA_RETAIN_WITH_LIMIT
```

行为：

- 适合批量分配和批量释放。
- 单个 free 只回退最近一次分配。
- reset 可释放全部、保留容量、按上限保留。

示例：

```c
HeapArenaAllocator arena;
heap_arena_init(&arena, heap_page_allocator());
HeapAllocator a = heap_arena_allocator(&arena);
void *p = heap_alloc(a, 128, heap_align_from(16));
heap_arena_reset(&arena, (HeapArenaResetMode){ HEAP_ARENA_FREE_ALL, 0 });
heap_arena_deinit(&arena);
```

## MemoryPool

接口：

```c
bool heap_memory_pool_init(HeapMemoryPool *pool, HeapAllocator allocator,
    size_t item_size, size_t item_alignment, bool growable, size_t capacity);
void heap_memory_pool_deinit(HeapMemoryPool *pool);
bool heap_memory_pool_add_capacity(HeapMemoryPool *pool, size_t count);
bool heap_memory_pool_reset(HeapMemoryPool *pool, HeapArenaResetMode mode);
void *heap_memory_pool_create(HeapMemoryPool *pool);
void heap_memory_pool_destroy(HeapMemoryPool *pool, void *ptr);
```

行为：

- 固定尺寸对象池。
- destroy 后对象进入 free list。
- `growable=false` 时容量耗尽返回 NULL。
- reset 批量销毁全部对象。

示例：

```c
HeapMemoryPool pool;
heap_memory_pool_init(&pool, heap_page_allocator(), sizeof(int), _Alignof(int), true, 32);
int *x = heap_memory_pool_create(&pool);
heap_memory_pool_destroy(&pool, x);
heap_memory_pool_deinit(&pool);
```

## ThreadSafeAllocator

接口：

```c
void heap_thread_safe_allocator_init(HeapThreadSafeAllocator *self, HeapAllocator child);
void heap_thread_safe_allocator_deinit(HeapThreadSafeAllocator *self);
HeapAllocator heap_thread_safe_allocator(HeapThreadSafeAllocator *self);
```

行为：用 mutex 包装任意非线程安全 allocator。

## SbrkAllocator

接口：

```c
typedef size_t (*HeapSbrkFn)(size_t n);
bool heap_sbrk_allocator_init(HeapSbrkAllocator *self, HeapSbrkFn sbrk);
void heap_sbrk_allocator_deinit(HeapSbrkAllocator *self);
HeapAllocator heap_sbrk_allocator(HeapSbrkAllocator *self);
```

行为：

- 小对象按 2 的幂 size class。
- free list 指针写在 slot 尾部。
- 大对象按 64KiB bigpage class。
- resize/remap 只允许保持同一 class。

## WasmAllocator

接口：

```c
bool heap_wasm_allocator_init(HeapWasmAllocator *self, HeapSbrkFn memory_grow_sbrk);
void heap_wasm_allocator_deinit(HeapWasmAllocator *self);
HeapAllocator heap_wasm_allocator(HeapWasmAllocator *self);
```

行为：复用 SbrkAllocator size-class 逻辑；`memory_grow_sbrk` 负责对接 wasm memory grow。

## SmpAllocator

接口：

```c
HeapAllocator heap_smp_allocator(void);
void heap_smp_reset_for_tests(void);
```

行为：

- 全局 singleton。
- 小对象使用 per-thread freelist + slab。
- 大对象直接转 PageAllocator。
- resize/remap 不允许跨小/大类别。

## DebugAllocator

接口：

```c
HeapDebugConfig heap_debug_default_config(void);
bool heap_debug_allocator_init(HeapDebugAllocator **out, HeapAllocator backing, HeapDebugConfig config);
HeapAllocator heap_debug_allocator(HeapDebugAllocator *self);
size_t heap_debug_detect_leaks(HeapDebugAllocator *self);
void heap_debug_flush_retained_metadata(HeapDebugAllocator *self);
HeapCheck heap_debug_allocator_deinit(HeapDebugAllocator *self);
void heap_debug_allocator_deinit_without_leak_check(HeapDebugAllocator *self);
size_t heap_debug_total_requested_bytes(HeapDebugAllocator *self);
void heap_debug_set_requested_memory_limit(HeapDebugAllocator *self, size_t limit);
```

配置：

```c
typedef struct HeapDebugConfig {
    size_t stack_trace_frames;
    bool enable_memory_limit;
    bool safety;
    bool thread_safe;
    bool never_unmap;
    bool retain_metadata;
    bool verbose_log;
    bool backing_allocator_zeroes;
    bool resize_stack_traces;
    uintptr_t canary;
    size_t page_size;
} HeapDebugConfig;
```

行为：

- 小对象：page bucket、used bits、requested size、alignment、alloc/free trace。
- 大对象：独立 backing allocation + metadata table。
- double free：输出 alloc、first free、second free trace。
- leak：`deinit` 或 `detectLeaks` 输出泄漏地址和 alloc trace。
- memory limit：按用户请求字节数计数。
- retain metadata：保留已释放大对象 metadata。
- never unmap：free 时不归还 backing memory，调试悬垂指针。

示例：

```c
HeapDebugConfig cfg = heap_debug_default_config();
cfg.enable_memory_limit = true;

HeapDebugAllocator *dbg;
heap_debug_allocator_init(&dbg, heap_page_allocator(), cfg);
heap_debug_set_requested_memory_limit(dbg, 1024);

HeapAllocator a = heap_debug_allocator(dbg);
void *p = heap_alloc(a, 64, heap_align_from(8));
heap_free(a, p, 64, heap_align_from(8));

HeapCheck check = heap_debug_allocator_deinit(dbg);
```

## 产物

- `include/heap.h`：全部公开接口。
- `src/*.c`：allocator 实现。
- `tests/heap_tests.c`：功能测试。
- `Makefile`：构建与测试。
