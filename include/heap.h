#ifndef HEAP_H
#define HEAP_H

#include <stdalign.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef HEAP_DEBUG_MAX_FRAMES
#define HEAP_DEBUG_MAX_FRAMES 16
#endif

#ifndef HEAP_SMP_MAX_THREADS
#define HEAP_SMP_MAX_THREADS 128
#endif

typedef struct HeapAlignment {
    size_t bytes;
} HeapAlignment;

typedef struct HeapAllocator HeapAllocator;

typedef void *(*HeapAllocFn)(void *ctx, size_t n, HeapAlignment alignment, uintptr_t return_address);
typedef bool (*HeapResizeFn)(void *ctx, void *ptr, size_t old_len, HeapAlignment alignment, size_t new_len, uintptr_t return_address);
typedef void *(*HeapRemapFn)(void *ctx, void *ptr, size_t old_len, HeapAlignment alignment, size_t new_len, uintptr_t return_address);
typedef void (*HeapFreeFn)(void *ctx, void *ptr, size_t len, HeapAlignment alignment, uintptr_t return_address);

typedef struct HeapVTable {
    HeapAllocFn alloc;
    HeapResizeFn resize;
    HeapRemapFn remap;
    HeapFreeFn free;
} HeapVTable;

struct HeapAllocator {
    void *ctx;
    const HeapVTable *vtable;
};

typedef struct HeapMutex {
#if defined(_WIN32)
    void *handle;
#else
    void *handle;
#endif
} HeapMutex;

typedef struct HeapCheck {
    enum { HEAP_CHECK_OK = 0, HEAP_CHECK_LEAK = 1 } value;
} HeapCheck;

static inline HeapAlignment heap_align_from(size_t bytes) {
    HeapAlignment a;
    a.bytes = bytes ? bytes : 1;
    return a;
}

#define HEAP_ALIGN_OF(T) heap_align_from(_Alignof(T))

void heap_mutex_init(HeapMutex *m);
void heap_mutex_deinit(HeapMutex *m);
void heap_mutex_lock(HeapMutex *m);
void heap_mutex_unlock(HeapMutex *m);

bool heap_is_power_of_two(size_t x);
size_t heap_align_forward(size_t value, size_t alignment);
void *heap_align_ptr(void *ptr, size_t alignment);
bool heap_add_overflow_size(size_t a, size_t b, size_t *out);
size_t heap_ceil_pow2(size_t x);
size_t heap_log2_floor(size_t x);
size_t heap_page_size(void);

void *heap_alloc(HeapAllocator allocator, size_t n, HeapAlignment alignment);
bool heap_resize(HeapAllocator allocator, void *ptr, size_t old_len, HeapAlignment alignment, size_t new_len);
void *heap_remap(HeapAllocator allocator, void *ptr, size_t old_len, HeapAlignment alignment, size_t new_len);
void heap_free(HeapAllocator allocator, void *ptr, size_t len, HeapAlignment alignment);
void *heap_realloc(HeapAllocator allocator, void *ptr, size_t old_len, HeapAlignment alignment, size_t new_len);

extern const HeapVTable heap_page_allocator_vtable;
HeapAllocator heap_page_allocator(void);
void *heap_page_map(size_t n, HeapAlignment alignment);
void heap_page_unmap(void *ptr, size_t len);
void *heap_page_realloc(void *ptr, size_t old_len, size_t new_len, bool may_move);

typedef struct HeapThreadSafeAllocator {
    HeapAllocator child;
    HeapMutex mutex;
} HeapThreadSafeAllocator;

void heap_thread_safe_allocator_init(HeapThreadSafeAllocator *self, HeapAllocator child);
void heap_thread_safe_allocator_deinit(HeapThreadSafeAllocator *self);
HeapAllocator heap_thread_safe_allocator(HeapThreadSafeAllocator *self);

typedef struct HeapFixedBufferAllocator {
    _Atomic size_t end_index;
    unsigned char *buffer;
    size_t len;
} HeapFixedBufferAllocator;

void heap_fixed_buffer_init(HeapFixedBufferAllocator *self, void *buffer, size_t len);
HeapAllocator heap_fixed_buffer_allocator(HeapFixedBufferAllocator *self);
HeapAllocator heap_fixed_buffer_thread_safe_allocator(HeapFixedBufferAllocator *self);
void heap_fixed_buffer_reset(HeapFixedBufferAllocator *self);
bool heap_fixed_buffer_owns_ptr(HeapFixedBufferAllocator *self, void *ptr);
bool heap_fixed_buffer_owns_slice(HeapFixedBufferAllocator *self, void *ptr, size_t len);
bool heap_fixed_buffer_is_last(HeapFixedBufferAllocator *self, void *ptr, size_t len);

typedef struct HeapArenaBufNode {
    size_t data;
    struct HeapArenaBufNode *next;
} HeapArenaBufNode;

typedef struct HeapArenaState {
    HeapArenaBufNode *first;
    size_t end_index;
} HeapArenaState;

typedef enum HeapArenaResetKind {
    HEAP_ARENA_FREE_ALL,
    HEAP_ARENA_RETAIN_CAPACITY,
    HEAP_ARENA_RETAIN_WITH_LIMIT,
} HeapArenaResetKind;

typedef struct HeapArenaResetMode {
    HeapArenaResetKind kind;
    size_t limit;
} HeapArenaResetMode;

typedef struct HeapArenaAllocator {
    HeapAllocator child;
    HeapArenaState state;
} HeapArenaAllocator;

void heap_arena_init(HeapArenaAllocator *self, HeapAllocator child);
void heap_arena_deinit(HeapArenaAllocator *self);
HeapAllocator heap_arena_allocator(HeapArenaAllocator *self);
size_t heap_arena_query_capacity(HeapArenaAllocator *self);
bool heap_arena_reset(HeapArenaAllocator *self, HeapArenaResetMode mode);

typedef struct HeapMemoryPool {
    HeapArenaAllocator arena;
    void *free_list;
    size_t item_size;
    size_t item_alignment;
    bool growable;
} HeapMemoryPool;

bool heap_memory_pool_init(HeapMemoryPool *pool, HeapAllocator allocator, size_t item_size, size_t item_alignment, bool growable, size_t capacity);
void heap_memory_pool_deinit(HeapMemoryPool *pool);
bool heap_memory_pool_add_capacity(HeapMemoryPool *pool, size_t count);
bool heap_memory_pool_reset(HeapMemoryPool *pool, HeapArenaResetMode mode);
void *heap_memory_pool_create(HeapMemoryPool *pool);
void heap_memory_pool_destroy(HeapMemoryPool *pool, void *ptr);

typedef size_t (*HeapSbrkFn)(size_t n);

typedef struct HeapSbrkAllocator {
    HeapSbrkFn sbrk;
    HeapMutex mutex;
    size_t *next_addrs;
    size_t *frees;
    size_t *big_frees;
    size_t size_class_count;
    size_t big_size_class_count;
    size_t min_class;
    size_t bigpage_size;
} HeapSbrkAllocator;

bool heap_sbrk_allocator_init(HeapSbrkAllocator *self, HeapSbrkFn sbrk);
void heap_sbrk_allocator_deinit(HeapSbrkAllocator *self);
HeapAllocator heap_sbrk_allocator(HeapSbrkAllocator *self);

typedef struct HeapWasmAllocator {
    HeapSbrkAllocator inner;
} HeapWasmAllocator;

bool heap_wasm_allocator_init(HeapWasmAllocator *self, HeapSbrkFn memory_grow_sbrk);
void heap_wasm_allocator_deinit(HeapWasmAllocator *self);
HeapAllocator heap_wasm_allocator(HeapWasmAllocator *self);

typedef struct HeapSmpAllocator HeapSmpAllocator;
HeapAllocator heap_smp_allocator(void);
void heap_smp_reset_for_tests(void);

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

typedef struct HeapDebugAllocator HeapDebugAllocator;

HeapDebugConfig heap_debug_default_config(void);
bool heap_debug_allocator_init(HeapDebugAllocator **out, HeapAllocator backing, HeapDebugConfig config);
HeapAllocator heap_debug_allocator(HeapDebugAllocator *self);
size_t heap_debug_detect_leaks(HeapDebugAllocator *self);
void heap_debug_flush_retained_metadata(HeapDebugAllocator *self);
HeapCheck heap_debug_allocator_deinit(HeapDebugAllocator *self);
void heap_debug_allocator_deinit_without_leak_check(HeapDebugAllocator *self);
size_t heap_debug_total_requested_bytes(HeapDebugAllocator *self);
void heap_debug_set_requested_memory_limit(HeapDebugAllocator *self, size_t limit);

#ifdef __cplusplus
}
#endif

#endif
