#include "heap.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void section(const char *name) {
    printf("\n========== %s ==========\n", name);
}

static void step(const char *allocator, const char *op, size_t bytes, size_t alignment, const void *ptr) {
    printf("[%s] %s: bytes=%zu alignment=%zu ptr=%p\n", allocator, op, bytes, alignment, ptr);
}

static void note(const char *allocator, const char *message) {
    printf("[%s] %s\n", allocator, message);
}

static void test_fixed_buffer(void) {
    section("FixedBufferAllocator");
    unsigned char mem[128];
    HeapFixedBufferAllocator fba;
    heap_fixed_buffer_init(&fba, mem, sizeof(mem));
    printf("[FixedBufferAllocator] init: buffer=%p size=%zu\n", (void *)mem, sizeof(mem));
    HeapAllocator a = heap_fixed_buffer_allocator(&fba);
    void *p = heap_alloc(a, 7, HEAP_ALIGN_OF(uint64_t));
    step("FixedBufferAllocator", "alloc", 7, _Alignof(uint64_t), p);
    assert(p);
    assert(((uintptr_t)p % _Alignof(uint64_t)) == 0);
    void *q = heap_alloc(a, 8, HEAP_ALIGN_OF(uint64_t));
    step("FixedBufferAllocator", "alloc", 8, _Alignof(uint64_t), q);
    assert(q && q != p);
    note("FixedBufferAllocator", "resize latest allocation q from 8 bytes to 16 bytes");
    assert(heap_resize(a, q, 8, HEAP_ALIGN_OF(uint64_t), 16));
    step("FixedBufferAllocator", "free", 16, _Alignof(uint64_t), q);
    heap_free(a, q, 16, HEAP_ALIGN_OF(uint64_t));
    note("FixedBufferAllocator", "try resize non-latest allocation p from 8 bytes to 200 bytes; expected failure");
    assert(!heap_resize(a, p, 8, HEAP_ALIGN_OF(uint64_t), 200));
    note("FixedBufferAllocator", "reset: p is not individually freed; reset releases the bump state");
    heap_fixed_buffer_reset(&fba);
    void *r = heap_alloc(a, 128, heap_align_from(1));
    step("FixedBufferAllocator", "alloc after reset", 128, 1, r);
    assert(r == mem);

    note("FixedBufferAllocator", "reset before lock-free thread-safe allocator test");
    heap_fixed_buffer_reset(&fba);
    HeapAllocator ta = heap_fixed_buffer_thread_safe_allocator(&fba);
    void *tp = heap_alloc(ta, 8, HEAP_ALIGN_OF(uint64_t));
    step("FixedBufferThreadSafeAllocator", "atomic alloc", 8, _Alignof(uint64_t), tp);
    assert(tp);
    note("FixedBufferThreadSafeAllocator", "resize is disabled in this interface; expected failure");
    assert(!heap_resize(ta, mem, 8, HEAP_ALIGN_OF(uint64_t), 4));
}

static void test_arena(void) {
    section("ArenaAllocator");
    HeapArenaAllocator arena;
    heap_arena_init(&arena, heap_page_allocator());
    note("ArenaAllocator", "init with PageAllocator child");
    HeapAllocator a = heap_arena_allocator(&arena);
    void *p = heap_alloc(a, 32, heap_align_from(16));
    step("ArenaAllocator", "alloc", 32, 16, p);
    void *q = heap_alloc(a, 64, heap_align_from(8));
    step("ArenaAllocator", "alloc", 64, 8, q);
    assert(p && q);
    note("ArenaAllocator", "resize latest allocation q from 64 bytes to 70 bytes");
    assert(heap_resize(a, q, 64, heap_align_from(8), 70));
    step("ArenaAllocator", "free latest allocation", 70, 8, q);
    heap_free(a, q, 70, heap_align_from(8));
    size_t cap = heap_arena_query_capacity(&arena);
    printf("[ArenaAllocator] query capacity: capacity=%zu bytes; p=%p remains allocated until arena reset/deinit\n", cap, p);
    assert(cap > 0);
    note("ArenaAllocator", "reset retain_capacity: unreleased allocation p is batch-released by reset");
    assert(heap_arena_reset(&arena, (HeapArenaResetMode){ HEAP_ARENA_RETAIN_CAPACITY, 0 }));
    assert(heap_arena_query_capacity(&arena) >= cap / 2);
    void *r = heap_alloc(a, 16, heap_align_from(1));
    step("ArenaAllocator", "alloc after reset", 16, 1, r);
    assert(r);
    note("ArenaAllocator", "deinit: releases all retained arena buffers and any allocations not individually freed");
    heap_arena_deinit(&arena);
}

static void test_memory_pool(void) {
    section("MemoryPool");
    HeapMemoryPool pool;
    note("MemoryPool", "init growable pool with PageAllocator child; item=uint64_t; capacity=2");
    assert(heap_memory_pool_init(&pool, heap_page_allocator(), sizeof(uint64_t), _Alignof(uint64_t), true, 2));
    uint64_t *a = (uint64_t *)heap_memory_pool_create(&pool);
    step("MemoryPool", "create item", sizeof(uint64_t), _Alignof(uint64_t), a);
    uint64_t *b = (uint64_t *)heap_memory_pool_create(&pool);
    step("MemoryPool", "create item", sizeof(uint64_t), _Alignof(uint64_t), b);
    assert(a && b && a != b);
    step("MemoryPool", "destroy item into free list", sizeof(uint64_t), _Alignof(uint64_t), b);
    heap_memory_pool_destroy(&pool, b);
    uint64_t *c = (uint64_t *)heap_memory_pool_create(&pool);
    step("MemoryPool", "create item reusing free list", sizeof(uint64_t), _Alignof(uint64_t), c);
    assert(c == b);
    note("MemoryPool", "deinit: a and c are not individually freed; arena backing memory is released here");
    heap_memory_pool_deinit(&pool);

    note("MemoryPool", "init non-growable pool with capacity=1");
    assert(heap_memory_pool_init(&pool, heap_page_allocator(), sizeof(uint32_t), _Alignof(uint32_t), false, 1));
    void *one = heap_memory_pool_create(&pool);
    step("MemoryPool", "create non-growable item", sizeof(uint32_t), _Alignof(uint32_t), one);
    assert(one);
    note("MemoryPool", "second create exceeds capacity; expected NULL");
    assert(!heap_memory_pool_create(&pool));
    note("MemoryPool", "deinit: releases non-growable pool backing memory");
    heap_memory_pool_deinit(&pool);
}

static void test_page(void) {
    section("PageAllocator");
    HeapAllocator a = heap_page_allocator();
    void *p = heap_alloc(a, 1000, heap_align_from(4096));
    step("PageAllocator", "map/alloc", 1000, 4096, p);
    assert(p && ((uintptr_t)p % 4096) == 0);
    note("PageAllocator", "resize page allocation from 1000 bytes to 500 bytes; expected in-place success");
    assert(heap_resize(a, p, 1000, heap_align_from(4096), 500));
    step("PageAllocator", "unmap/free", 500, 4096, p);
    heap_free(a, p, 500, heap_align_from(4096));
}

static unsigned char sbrk_heap[1024 * 1024];
static size_t sbrk_pos;

static size_t fake_sbrk(size_t n) {
    size_t aligned = heap_align_forward(sbrk_pos, heap_page_size());
    if (aligned + n > sizeof(sbrk_heap)) return 0;
    sbrk_pos = aligned + n;
    printf("[fake_sbrk] grow: request=%zu aligned_offset=%zu result=%p new_pos=%zu\n", n, aligned, (void *)(sbrk_heap + aligned), sbrk_pos);
    return (size_t)(uintptr_t)(sbrk_heap + aligned);
}

static void test_sbrk_wasm(void) {
    section("SbrkAllocator");
    sbrk_pos = 0;
    HeapSbrkAllocator sbrk_alloc;
    assert(heap_sbrk_allocator_init(&sbrk_alloc, fake_sbrk));
    note("SbrkAllocator", "init with fake_sbrk backing buffer");
    HeapAllocator a = heap_sbrk_allocator(&sbrk_alloc);
    void *p = heap_alloc(a, 7, HEAP_ALIGN_OF(uint64_t));
    step("SbrkAllocator", "small alloc", 7, _Alignof(uint64_t), p);
    void *q = heap_alloc(a, 70000, heap_align_from(8));
    step("SbrkAllocator", "large alloc", 70000, 8, q);
    assert(p && q);
    note("SbrkAllocator", "resize small allocation from 7 bytes to 8 bytes inside same size class");
    assert(heap_resize(a, p, 7, HEAP_ALIGN_OF(uint64_t), 8));
    step("SbrkAllocator", "free small allocation", 8, _Alignof(uint64_t), p);
    heap_free(a, p, 8, HEAP_ALIGN_OF(uint64_t));
    step("SbrkAllocator", "free large allocation", 70000, 8, q);
    heap_free(a, q, 70000, heap_align_from(8));
    heap_sbrk_allocator_deinit(&sbrk_alloc);
    note("SbrkAllocator", "deinit: metadata arrays released; fake_sbrk backing buffer is static test memory");

    section("WasmAllocator");
    sbrk_pos = 0;
    HeapWasmAllocator wasm;
    assert(heap_wasm_allocator_init(&wasm, fake_sbrk));
    note("WasmAllocator", "init with fake wasm memory-grow sbrk callback");
    HeapAllocator wa = heap_wasm_allocator(&wasm);
    p = heap_alloc(wa, 16, heap_align_from(8));
    step("WasmAllocator", "alloc", 16, 8, p);
    assert(p);
    step("WasmAllocator", "free", 16, 8, p);
    heap_free(wa, p, 16, heap_align_from(8));
    heap_wasm_allocator_deinit(&wasm);
    note("WasmAllocator", "deinit complete");
}

static void test_smp(void) {
    section("SmpAllocator");
    HeapAllocator a = heap_smp_allocator();
    void *p = heap_alloc(a, 31, heap_align_from(8));
    step("SmpAllocator", "small slab alloc", 31, 8, p);
    void *q = heap_alloc(a, 100000, heap_align_from(4096));
    step("SmpAllocator", "large page alloc", 100000, 4096, q);
    assert(p && q);
    note("SmpAllocator", "resize small allocation from 31 bytes to 32 bytes inside same class");
    assert(heap_resize(a, p, 31, heap_align_from(8), 32));
    note("SmpAllocator", "resize small allocation to large class; expected failure");
    assert(!heap_resize(a, p, 32, heap_align_from(8), 100000));
    step("SmpAllocator", "free small slab allocation", 32, 8, p);
    heap_free(a, p, 32, heap_align_from(8));
    step("SmpAllocator", "free large page allocation", 100000, 4096, q);
    heap_free(a, q, 100000, heap_align_from(4096));
}

static void test_thread_safe(void) {
    section("ThreadSafeAllocator");
    unsigned char mem[64];
    HeapFixedBufferAllocator fba;
    heap_fixed_buffer_init(&fba, mem, sizeof(mem));
    printf("[ThreadSafeAllocator] child FixedBufferAllocator buffer=%p size=%zu\n", (void *)mem, sizeof(mem));
    HeapThreadSafeAllocator tsa;
    heap_thread_safe_allocator_init(&tsa, heap_fixed_buffer_allocator(&fba));
    note("ThreadSafeAllocator", "init mutex wrapper around FixedBufferAllocator");
    HeapAllocator a = heap_thread_safe_allocator(&tsa);
    void *p = heap_alloc(a, 16, heap_align_from(8));
    step("ThreadSafeAllocator", "locked alloc via child", 16, 8, p);
    assert(p);
    step("ThreadSafeAllocator", "locked free via child", 16, 8, p);
    heap_free(a, p, 16, heap_align_from(8));
    heap_thread_safe_allocator_deinit(&tsa);
    note("ThreadSafeAllocator", "deinit mutex wrapper");
}

static void test_debug(void) {
    section("DebugAllocator");
    HeapDebugAllocator *dbg = NULL;
    HeapDebugConfig cfg = heap_debug_default_config();
    cfg.thread_safe = false;
    assert(heap_debug_allocator_init(&dbg, heap_page_allocator(), cfg));
    note("DebugAllocator", "init with PageAllocator backing; safety enabled; leak check enabled");
    HeapAllocator a = heap_debug_allocator(dbg);
    void *p = heap_alloc(a, 20, heap_align_from(4));
    step("DebugAllocator", "small alloc with metadata", 20, 4, p);
    assert(p);
    note("DebugAllocator", "resize small allocation from 20 bytes to 21 bytes inside same class");
    assert(heap_resize(a, p, 20, heap_align_from(4), 21));
    note("DebugAllocator", "resize small allocation to large class; expected failure and original allocation remains active");
    assert(!heap_resize(a, p, 21, heap_align_from(4), 1000000));
    step("DebugAllocator", "free small allocation with metadata validation", 21, 4, p);
    heap_free(a, p, 21, heap_align_from(4));
    note("DebugAllocator", "deinit performs final leak detection; expected ok");
    assert(heap_debug_allocator_deinit(dbg).value == HEAP_CHECK_OK);

    cfg.enable_memory_limit = true;
    assert(heap_debug_allocator_init(&dbg, heap_page_allocator(), cfg));
    heap_debug_set_requested_memory_limit(dbg, 32);
    note("DebugAllocator", "init memory-limit mode; requested_memory_limit=32 bytes");
    a = heap_debug_allocator(dbg);
    p = heap_alloc(a, 16, heap_align_from(8));
    step("DebugAllocator", "limited alloc", 16, 8, p);
    assert(p);
    printf("[DebugAllocator] total_requested_bytes=%zu after 16-byte allocation\n", heap_debug_total_requested_bytes(dbg));
    assert(heap_debug_total_requested_bytes(dbg) == 16);
    note("DebugAllocator", "try alloc 64 bytes beyond memory limit; expected NULL and no allocation to free");
    assert(!heap_alloc(a, 64, heap_align_from(8)));
    step("DebugAllocator", "free limited allocation", 16, 8, p);
    heap_free(a, p, 16, heap_align_from(8));
    printf("[DebugAllocator] total_requested_bytes=%zu after free\n", heap_debug_total_requested_bytes(dbg));
    assert(heap_debug_total_requested_bytes(dbg) == 0);
    note("DebugAllocator", "deinit memory-limit instance; expected ok");
    assert(heap_debug_allocator_deinit(dbg).value == HEAP_CHECK_OK);

    cfg.enable_memory_limit = false;
    cfg.retain_metadata = true;
    cfg.never_unmap = true;
    assert(heap_debug_allocator_init(&dbg, heap_page_allocator(), cfg));
    note("DebugAllocator", "init retain_metadata + never_unmap mode");
    a = heap_debug_allocator(dbg);
    p = heap_alloc(a, 200000, heap_align_from(16));
    step("DebugAllocator", "large alloc retained metadata", 200000, 16, p);
    assert(p);
    note("DebugAllocator", "free large allocation: memory is retained because never_unmap=true; metadata marks it freed");
    heap_free(a, p, 200000, heap_align_from(16));
    note("DebugAllocator", "flush retained metadata: now releases large allocation retained by never_unmap");
    heap_debug_flush_retained_metadata(dbg);
    note("DebugAllocator", "deinit retain_metadata instance; expected ok");
    assert(heap_debug_allocator_deinit(dbg).value == HEAP_CHECK_OK);
}

int main(void) {
    puts("heap c_str detailed allocator test start");
    test_page();
    test_fixed_buffer();
    test_arena();
    test_memory_pool();
    test_thread_safe();
    test_sbrk_wasm();
    test_smp();
    test_debug();
    puts("\nheap c_str detailed allocator tests passed");
    return 0;
}
