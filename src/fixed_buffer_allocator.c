#include "heap.h"

#include <assert.h>
#include <stdint.h>

static void *fba_alloc(void *ctx, size_t n, HeapAlignment alignment, uintptr_t ra);
static bool fba_resize(void *ctx, void *ptr, size_t old_len, HeapAlignment alignment, size_t new_len, uintptr_t ra);
static void *fba_remap(void *ctx, void *ptr, size_t old_len, HeapAlignment alignment, size_t new_len, uintptr_t ra);
static void fba_free(void *ctx, void *ptr, size_t len, HeapAlignment alignment, uintptr_t ra);
static void *fba_thread_alloc(void *ctx, size_t n, HeapAlignment alignment, uintptr_t ra);
static bool no_resize(void *ctx, void *ptr, size_t old_len, HeapAlignment alignment, size_t new_len, uintptr_t ra);
static void *no_remap(void *ctx, void *ptr, size_t old_len, HeapAlignment alignment, size_t new_len, uintptr_t ra);
static void no_free(void *ctx, void *ptr, size_t len, HeapAlignment alignment, uintptr_t ra);

static const HeapVTable fba_vtable = { fba_alloc, fba_resize, fba_remap, fba_free };
static const HeapVTable fba_thread_vtable = { fba_thread_alloc, no_resize, no_remap, no_free };

void heap_fixed_buffer_init(HeapFixedBufferAllocator *self, void *buffer, size_t len) {
    atomic_store(&self->end_index, 0);
    self->buffer = (unsigned char *)buffer;
    self->len = len;
}

HeapAllocator heap_fixed_buffer_allocator(HeapFixedBufferAllocator *self) {
    HeapAllocator a = { self, &fba_vtable };
    return a;
}

HeapAllocator heap_fixed_buffer_thread_safe_allocator(HeapFixedBufferAllocator *self) {
    HeapAllocator a = { self, &fba_thread_vtable };
    return a;
}

void heap_fixed_buffer_reset(HeapFixedBufferAllocator *self) {
    atomic_store(&self->end_index, 0);
}

bool heap_fixed_buffer_owns_ptr(HeapFixedBufferAllocator *self, void *ptr) {
    uintptr_t p = (uintptr_t)ptr;
    uintptr_t b = (uintptr_t)self->buffer;
    return p >= b && p < b + self->len;
}

bool heap_fixed_buffer_owns_slice(HeapFixedBufferAllocator *self, void *ptr, size_t len) {
    uintptr_t p = (uintptr_t)ptr;
    uintptr_t b = (uintptr_t)self->buffer;
    return p >= b && p + len <= b + self->len;
}

bool heap_fixed_buffer_is_last(HeapFixedBufferAllocator *self, void *ptr, size_t len) {
    size_t end = atomic_load(&self->end_index);
    return (unsigned char *)ptr + len == self->buffer + end;
}

static void *fba_alloc(void *ctx, size_t n, HeapAlignment alignment, uintptr_t ra) {
    (void)ra;
    HeapFixedBufferAllocator *self = (HeapFixedBufferAllocator *)ctx;
    size_t end = atomic_load(&self->end_index);
    uintptr_t addr = (uintptr_t)self->buffer + end;
    uintptr_t adjusted = heap_align_forward((size_t)addr, alignment.bytes);
    size_t adjusted_index = (size_t)(adjusted - (uintptr_t)self->buffer);
    if (adjusted_index > self->len || n > self->len - adjusted_index) return NULL;
    atomic_store(&self->end_index, adjusted_index + n);
    return self->buffer + adjusted_index;
}

static bool fba_resize(void *ctx, void *ptr, size_t old_len, HeapAlignment alignment, size_t new_len, uintptr_t ra) {
    (void)alignment; (void)ra;
    HeapFixedBufferAllocator *self = (HeapFixedBufferAllocator *)ctx;
    assert(heap_fixed_buffer_owns_slice(self, ptr, old_len));
    if (!heap_fixed_buffer_is_last(self, ptr, old_len)) return new_len <= old_len;
    size_t end = atomic_load(&self->end_index);
    if (new_len <= old_len) {
        atomic_store(&self->end_index, end - (old_len - new_len));
        return true;
    }
    size_t add = new_len - old_len;
    if (add > self->len - end) return false;
    atomic_store(&self->end_index, end + add);
    return true;
}

static void *fba_remap(void *ctx, void *ptr, size_t old_len, HeapAlignment alignment, size_t new_len, uintptr_t ra) {
    return fba_resize(ctx, ptr, old_len, alignment, new_len, ra) ? ptr : NULL;
}

static void fba_free(void *ctx, void *ptr, size_t len, HeapAlignment alignment, uintptr_t ra) {
    (void)alignment; (void)ra;
    HeapFixedBufferAllocator *self = (HeapFixedBufferAllocator *)ctx;
    assert(heap_fixed_buffer_owns_slice(self, ptr, len));
    if (heap_fixed_buffer_is_last(self, ptr, len)) {
        atomic_fetch_sub(&self->end_index, len);
    }
}

static void *fba_thread_alloc(void *ctx, size_t n, HeapAlignment alignment, uintptr_t ra) {
    (void)ra;
    HeapFixedBufferAllocator *self = (HeapFixedBufferAllocator *)ctx;
    size_t end = atomic_load_explicit(&self->end_index, memory_order_seq_cst);
    for (;;) {
        uintptr_t addr = (uintptr_t)self->buffer + end;
        uintptr_t adjusted = heap_align_forward((size_t)addr, alignment.bytes);
        size_t adjusted_index = (size_t)(adjusted - (uintptr_t)self->buffer);
        if (adjusted_index > self->len || n > self->len - adjusted_index) return NULL;
        size_t new_end = adjusted_index + n;
        if (atomic_compare_exchange_weak_explicit(&self->end_index, &end, new_end, memory_order_seq_cst, memory_order_seq_cst)) {
            return self->buffer + adjusted_index;
        }
    }
}

static bool no_resize(void *ctx, void *ptr, size_t old_len, HeapAlignment alignment, size_t new_len, uintptr_t ra) {
    (void)ctx; (void)ptr; (void)old_len; (void)alignment; (void)new_len; (void)ra;
    return false;
}

static void *no_remap(void *ctx, void *ptr, size_t old_len, HeapAlignment alignment, size_t new_len, uintptr_t ra) {
    (void)ctx; (void)ptr; (void)old_len; (void)alignment; (void)new_len; (void)ra;
    return NULL;
}

static void no_free(void *ctx, void *ptr, size_t len, HeapAlignment alignment, uintptr_t ra) {
    (void)ctx; (void)ptr; (void)len; (void)alignment; (void)ra;
}
