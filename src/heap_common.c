#include "heap.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

void heap_mutex_init(HeapMutex *m) {
#if defined(_WIN32)
    CRITICAL_SECTION *cs = (CRITICAL_SECTION *)malloc(sizeof(*cs));
    InitializeCriticalSection(cs);
    m->handle = cs;
#else
    pthread_mutex_t *pm = (pthread_mutex_t *)malloc(sizeof(*pm));
    pthread_mutex_init(pm, NULL);
    m->handle = pm;
#endif
}

void heap_mutex_deinit(HeapMutex *m) {
    if (!m || !m->handle) return;
#if defined(_WIN32)
    DeleteCriticalSection((CRITICAL_SECTION *)m->handle);
#else
    pthread_mutex_destroy((pthread_mutex_t *)m->handle);
#endif
    free(m->handle);
    m->handle = NULL;
}

void heap_mutex_lock(HeapMutex *m) {
    if (!m || !m->handle) return;
#if defined(_WIN32)
    EnterCriticalSection((CRITICAL_SECTION *)m->handle);
#else
    pthread_mutex_lock((pthread_mutex_t *)m->handle);
#endif
}

void heap_mutex_unlock(HeapMutex *m) {
    if (!m || !m->handle) return;
#if defined(_WIN32)
    LeaveCriticalSection((CRITICAL_SECTION *)m->handle);
#else
    pthread_mutex_unlock((pthread_mutex_t *)m->handle);
#endif
}

bool heap_is_power_of_two(size_t x) {
    return x != 0 && (x & (x - 1)) == 0;
}

size_t heap_align_forward(size_t value, size_t alignment) {
    if (alignment <= 1) return value;
    assert(heap_is_power_of_two(alignment));
    return (value + alignment - 1) & ~(alignment - 1);
}

void *heap_align_ptr(void *ptr, size_t alignment) {
    uintptr_t p = (uintptr_t)ptr;
    return (void *)heap_align_forward((size_t)p, alignment);
}

bool heap_add_overflow_size(size_t a, size_t b, size_t *out) {
    if (SIZE_MAX - a < b) return true;
    *out = a + b;
    return false;
}

size_t heap_ceil_pow2(size_t x) {
    if (x <= 1) return 1;
    if (x > ((size_t)1 << (sizeof(size_t) * 8 - 1))) return 0;
    x--;
    for (size_t i = 1; i < sizeof(size_t) * 8; i <<= 1) x |= x >> i;
    return x + 1;
}

size_t heap_log2_floor(size_t x) {
    size_t r = 0;
    while (x >>= 1) r++;
    return r;
}

size_t heap_page_size(void) {
#if defined(_WIN32)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (size_t)si.dwPageSize;
#else
    long n = sysconf(_SC_PAGESIZE);
    return n > 0 ? (size_t)n : 4096;
#endif
}

void *heap_alloc(HeapAllocator allocator, size_t n, HeapAlignment alignment) {
    if (!n) n = 1;
    return allocator.vtable->alloc(allocator.ctx, n, alignment, 0);
}

bool heap_resize(HeapAllocator allocator, void *ptr, size_t old_len, HeapAlignment alignment, size_t new_len) {
    if (!ptr) return false;
    if (!new_len) new_len = 1;
    return allocator.vtable->resize(allocator.ctx, ptr, old_len, alignment, new_len, 0);
}

void *heap_remap(HeapAllocator allocator, void *ptr, size_t old_len, HeapAlignment alignment, size_t new_len) {
    if (!ptr) return NULL;
    if (!new_len) new_len = 1;
    return allocator.vtable->remap(allocator.ctx, ptr, old_len, alignment, new_len, 0);
}

void heap_free(HeapAllocator allocator, void *ptr, size_t len, HeapAlignment alignment) {
    if (!ptr) return;
    allocator.vtable->free(allocator.ctx, ptr, len ? len : 1, alignment, 0);
}

void *heap_realloc(HeapAllocator allocator, void *ptr, size_t old_len, HeapAlignment alignment, size_t new_len) {
    if (!ptr) return heap_alloc(allocator, new_len, alignment);
    if (!new_len) {
        heap_free(allocator, ptr, old_len, alignment);
        return NULL;
    }
    void *mapped = heap_remap(allocator, ptr, old_len, alignment, new_len);
    if (mapped) return mapped;
    void *next = heap_alloc(allocator, new_len, alignment);
    if (!next) return NULL;
    memcpy(next, ptr, old_len < new_len ? old_len : new_len);
    heap_free(allocator, ptr, old_len, alignment);
    return next;
}
