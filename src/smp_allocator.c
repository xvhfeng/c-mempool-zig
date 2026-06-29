#include "heap.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <unistd.h>
#endif

typedef struct SmpThread {
    HeapMutex mutex;
    size_t next_addrs[32];
    size_t frees[32];
    bool initialized;
} SmpThread;

struct HeapSmpAllocator {
    _Atomic unsigned cpu_count;
    SmpThread threads[HEAP_SMP_MAX_THREADS];
};

static HeapSmpAllocator smp_global;
static _Thread_local unsigned smp_thread_index;
static const size_t smp_slab_len = 64 * 1024;
static const size_t smp_min_class = sizeof(size_t) == 8 ? 3 : 2;
static const size_t smp_size_class_count = 16 - (sizeof(size_t) == 8 ? 3 : 2);
static const size_t smp_max_alloc_search = 1;

static void *smp_alloc_fn(void *ctx, size_t len, HeapAlignment alignment, uintptr_t ra);
static bool smp_resize_fn(void *ctx, void *ptr, size_t old_len, HeapAlignment alignment, size_t new_len, uintptr_t ra);
static void *smp_remap_fn(void *ctx, void *ptr, size_t old_len, HeapAlignment alignment, size_t new_len, uintptr_t ra);
static void smp_free_fn(void *ctx, void *ptr, size_t len, HeapAlignment alignment, uintptr_t ra);

static const HeapVTable smp_vtable = { smp_alloc_fn, smp_resize_fn, smp_remap_fn, smp_free_fn };

static unsigned smp_cpu_count(void) {
    unsigned n = atomic_load(&smp_global.cpu_count);
    if (n) return n;
#if defined(_WIN32)
    n = 8;
#else
    long c = sysconf(_SC_NPROCESSORS_ONLN);
    n = c > 0 ? (unsigned)c : 1;
#endif
    if (n > HEAP_SMP_MAX_THREADS) n = HEAP_SMP_MAX_THREADS;
    unsigned expected = 0;
    atomic_compare_exchange_strong(&smp_global.cpu_count, &expected, n);
    return atomic_load(&smp_global.cpu_count);
}

static void smp_thread_init(SmpThread *t) {
    if (!t->initialized) {
        heap_mutex_init(&t->mutex);
        t->initialized = true;
    }
}

static SmpThread *smp_lock_thread(void) {
    unsigned count = smp_cpu_count();
    unsigned index = smp_thread_index % count;
    for (;;) {
        SmpThread *t = &smp_global.threads[index];
        smp_thread_init(t);
        heap_mutex_lock(&t->mutex);
        smp_thread_index = index;
        return t;
    }
}

static size_t smp_size_class(size_t len, HeapAlignment alignment) {
    size_t v = len ? len : 1;
    size_t log = heap_log2_floor(heap_ceil_pow2(v));
    size_t alog = heap_log2_floor(alignment.bytes ? alignment.bytes : 1);
    if (log < alog) log = alog;
    if (log < smp_min_class) log = smp_min_class;
    return log - smp_min_class;
}

static size_t smp_slot_size(size_t class) {
    return (size_t)1 << (class + smp_min_class);
}

HeapAllocator heap_smp_allocator(void) {
    HeapAllocator a = { &smp_global, &smp_vtable };
    return a;
}

void heap_smp_reset_for_tests(void) {
    memset(&smp_global, 0, sizeof(smp_global));
    smp_thread_index = 0;
}

static void *smp_alloc_fn(void *ctx, size_t len, HeapAlignment alignment, uintptr_t ra) {
    (void)ctx; (void)ra;
    size_t class = smp_size_class(len, alignment);
    if (class >= smp_size_class_count) return heap_page_map(len, alignment);
    size_t slot_size = smp_slot_size(class);
    SmpThread *t = smp_lock_thread();
    size_t top = t->frees[class];
    if (top) {
        size_t *node = (size_t *)(uintptr_t)top;
        t->frees[class] = *node;
        heap_mutex_unlock(&t->mutex);
        return (void *)(uintptr_t)top;
    }
    size_t next = t->next_addrs[class];
    if (next % smp_slab_len != 0) {
        t->next_addrs[class] = next + slot_size;
        heap_mutex_unlock(&t->mutex);
        return (void *)(uintptr_t)next;
    }
    (void)smp_max_alloc_search;
    void *slab = heap_page_map(smp_slab_len, heap_align_from(smp_slab_len));
    if (!slab) {
        heap_mutex_unlock(&t->mutex);
        return NULL;
    }
    t->next_addrs[class] = (size_t)(uintptr_t)slab + slot_size;
    heap_mutex_unlock(&t->mutex);
    return slab;
}

static bool smp_resize_fn(void *ctx, void *ptr, size_t old_len, HeapAlignment alignment, size_t new_len, uintptr_t ra) {
    (void)ctx; (void)ptr; (void)ra;
    size_t old_class = smp_size_class(old_len, alignment);
    size_t new_class = smp_size_class(new_len, alignment);
    if (old_class >= smp_size_class_count) {
        if (new_class < smp_size_class_count) return false;
        return heap_page_realloc(ptr, old_len, new_len, false) != NULL;
    }
    return old_class == new_class;
}

static void *smp_remap_fn(void *ctx, void *ptr, size_t old_len, HeapAlignment alignment, size_t new_len, uintptr_t ra) {
    (void)ctx; (void)ra;
    size_t old_class = smp_size_class(old_len, alignment);
    size_t new_class = smp_size_class(new_len, alignment);
    if (old_class >= smp_size_class_count) {
        if (new_class < smp_size_class_count) return NULL;
        return heap_page_realloc(ptr, old_len, new_len, true);
    }
    return old_class == new_class ? ptr : NULL;
}

static void smp_free_fn(void *ctx, void *ptr, size_t len, HeapAlignment alignment, uintptr_t ra) {
    (void)ctx; (void)ra;
    size_t class = smp_size_class(len, alignment);
    if (class >= smp_size_class_count) {
        heap_page_unmap(ptr, len);
        return;
    }
    SmpThread *t = smp_lock_thread();
    size_t *node = (size_t *)ptr;
    *node = t->frees[class];
    t->frees[class] = (size_t)(uintptr_t)ptr;
    heap_mutex_unlock(&t->mutex);
}
