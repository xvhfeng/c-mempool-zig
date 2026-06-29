#include "heap.h"

#include <stdlib.h>
#include <string.h>

static void *sbrk_alloc_fn(void *ctx, size_t len, HeapAlignment alignment, uintptr_t ra);
static bool sbrk_resize_fn(void *ctx, void *ptr, size_t old_len, HeapAlignment alignment, size_t new_len, uintptr_t ra);
static void *sbrk_remap_fn(void *ctx, void *ptr, size_t old_len, HeapAlignment alignment, size_t new_len, uintptr_t ra);
static void sbrk_free_fn(void *ctx, void *ptr, size_t len, HeapAlignment alignment, uintptr_t ra);

static const HeapVTable sbrk_vtable = { sbrk_alloc_fn, sbrk_resize_fn, sbrk_remap_fn, sbrk_free_fn };

static size_t big_pages_needed(HeapSbrkAllocator *self, size_t byte_count) {
    return (byte_count + (self->bigpage_size + sizeof(size_t) - 1)) / self->bigpage_size;
}

bool heap_sbrk_allocator_init(HeapSbrkAllocator *self, HeapSbrkFn sbrk) {
    memset(self, 0, sizeof(*self));
    self->sbrk = sbrk;
    self->bigpage_size = 64 * 1024;
    self->min_class = heap_log2_floor(heap_ceil_pow2(1 + sizeof(size_t)));
    self->size_class_count = heap_log2_floor(self->bigpage_size) - self->min_class;
    self->big_size_class_count = sizeof(size_t) * 8 - heap_log2_floor(self->bigpage_size);
    self->next_addrs = (size_t *)calloc(self->size_class_count, sizeof(size_t));
    self->frees = (size_t *)calloc(self->size_class_count, sizeof(size_t));
    self->big_frees = (size_t *)calloc(self->big_size_class_count, sizeof(size_t));
    if (!self->next_addrs || !self->frees || !self->big_frees) {
        heap_sbrk_allocator_deinit(self);
        return false;
    }
    heap_mutex_init(&self->mutex);
    return true;
}

void heap_sbrk_allocator_deinit(HeapSbrkAllocator *self) {
    free(self->next_addrs);
    free(self->frees);
    free(self->big_frees);
    self->next_addrs = self->frees = self->big_frees = NULL;
    heap_mutex_deinit(&self->mutex);
}

HeapAllocator heap_sbrk_allocator(HeapSbrkAllocator *self) {
    HeapAllocator a = { self, &sbrk_vtable };
    return a;
}

static size_t sbrk_alloc_big_pages(HeapSbrkAllocator *self, size_t n) {
    size_t pow2_pages = heap_ceil_pow2(n);
    if (!pow2_pages) return 0;
    size_t class = heap_log2_floor(pow2_pages);
    size_t slot_size = pow2_pages * self->bigpage_size;
    if (class < self->big_size_class_count && self->big_frees[class]) {
        size_t top = self->big_frees[class];
        size_t *node = (size_t *)(uintptr_t)(top + slot_size - sizeof(size_t));
        self->big_frees[class] = *node;
        return top;
    }
    return self->sbrk(slot_size);
}

static void *sbrk_alloc_fn(void *ctx, size_t len, HeapAlignment alignment, uintptr_t ra) {
    (void)ra;
    HeapSbrkAllocator *self = (HeapSbrkAllocator *)ctx;
    heap_mutex_lock(&self->mutex);
    size_t actual;
    if (heap_add_overflow_size(len, sizeof(size_t), &actual)) {
        heap_mutex_unlock(&self->mutex);
        return NULL;
    }
    if (actual < alignment.bytes) actual = alignment.bytes;
    size_t slot_size = heap_ceil_pow2(actual);
    if (!slot_size) {
        heap_mutex_unlock(&self->mutex);
        return NULL;
    }
    size_t class = heap_log2_floor(slot_size) - self->min_class;
    size_t addr = 0;
    if (class < self->size_class_count) {
        if (self->frees[class]) {
            addr = self->frees[class];
            size_t *node = (size_t *)(uintptr_t)(addr + slot_size - sizeof(size_t));
            self->frees[class] = *node;
        } else {
            size_t next = self->next_addrs[class];
            if (next % heap_page_size() == 0) {
                addr = sbrk_alloc_big_pages(self, 1);
                if (addr) self->next_addrs[class] = addr + slot_size;
            } else {
                addr = next;
                self->next_addrs[class] = next + slot_size;
            }
        }
    } else {
        addr = sbrk_alloc_big_pages(self, big_pages_needed(self, actual));
    }
    heap_mutex_unlock(&self->mutex);
    return addr ? (void *)(uintptr_t)addr : NULL;
}

static bool sbrk_resize_fn(void *ctx, void *ptr, size_t old_len, HeapAlignment alignment, size_t new_len, uintptr_t ra) {
    (void)ptr; (void)ra;
    HeapSbrkAllocator *self = (HeapSbrkAllocator *)ctx;
    heap_mutex_lock(&self->mutex);
    size_t old_actual = old_len + sizeof(size_t);
    size_t new_actual = new_len + sizeof(size_t);
    if (old_actual < alignment.bytes) old_actual = alignment.bytes;
    if (new_actual < alignment.bytes) new_actual = alignment.bytes;
    size_t old_slot = heap_ceil_pow2(old_actual);
    size_t old_class = heap_log2_floor(old_slot) - self->min_class;
    bool ok;
    if (old_class < self->size_class_count) {
        ok = old_slot == heap_ceil_pow2(new_actual);
    } else {
        size_t old_big = heap_ceil_pow2(big_pages_needed(self, old_actual));
        size_t new_big = heap_ceil_pow2(big_pages_needed(self, new_actual));
        ok = old_big == new_big;
    }
    heap_mutex_unlock(&self->mutex);
    return ok;
}

static void *sbrk_remap_fn(void *ctx, void *ptr, size_t old_len, HeapAlignment alignment, size_t new_len, uintptr_t ra) {
    return sbrk_resize_fn(ctx, ptr, old_len, alignment, new_len, ra) ? ptr : NULL;
}

static void sbrk_free_fn(void *ctx, void *ptr, size_t len, HeapAlignment alignment, uintptr_t ra) {
    (void)ra;
    HeapSbrkAllocator *self = (HeapSbrkAllocator *)ctx;
    heap_mutex_lock(&self->mutex);
    size_t actual = len + sizeof(size_t);
    if (actual < alignment.bytes) actual = alignment.bytes;
    size_t slot_size = heap_ceil_pow2(actual);
    size_t class = heap_log2_floor(slot_size) - self->min_class;
    size_t addr = (size_t)(uintptr_t)ptr;
    if (class < self->size_class_count) {
        size_t *node = (size_t *)(uintptr_t)(addr + slot_size - sizeof(size_t));
        *node = self->frees[class];
        self->frees[class] = addr;
    } else {
        size_t pages = heap_ceil_pow2(big_pages_needed(self, actual));
        size_t big_class = heap_log2_floor(pages);
        size_t slot_bytes = pages * self->bigpage_size;
        if (big_class < self->big_size_class_count) {
            size_t *node = (size_t *)(uintptr_t)(addr + slot_bytes - sizeof(size_t));
            *node = self->big_frees[big_class];
            self->big_frees[big_class] = addr;
        }
    }
    heap_mutex_unlock(&self->mutex);
}
