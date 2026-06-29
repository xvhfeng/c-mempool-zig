#include "heap.h"

static void *tsa_alloc(void *ctx, size_t n, HeapAlignment alignment, uintptr_t ra);
static bool tsa_resize(void *ctx, void *ptr, size_t old_len, HeapAlignment alignment, size_t new_len, uintptr_t ra);
static void *tsa_remap(void *ctx, void *ptr, size_t old_len, HeapAlignment alignment, size_t new_len, uintptr_t ra);
static void tsa_free(void *ctx, void *ptr, size_t len, HeapAlignment alignment, uintptr_t ra);

static const HeapVTable tsa_vtable = { tsa_alloc, tsa_resize, tsa_remap, tsa_free };

void heap_thread_safe_allocator_init(HeapThreadSafeAllocator *self, HeapAllocator child) {
    self->child = child;
    heap_mutex_init(&self->mutex);
}

void heap_thread_safe_allocator_deinit(HeapThreadSafeAllocator *self) {
    heap_mutex_deinit(&self->mutex);
}

HeapAllocator heap_thread_safe_allocator(HeapThreadSafeAllocator *self) {
    HeapAllocator a = { self, &tsa_vtable };
    return a;
}

static void *tsa_alloc(void *ctx, size_t n, HeapAlignment alignment, uintptr_t ra) {
    HeapThreadSafeAllocator *self = (HeapThreadSafeAllocator *)ctx;
    heap_mutex_lock(&self->mutex);
    void *p = self->child.vtable->alloc(self->child.ctx, n, alignment, ra);
    heap_mutex_unlock(&self->mutex);
    return p;
}

static bool tsa_resize(void *ctx, void *ptr, size_t old_len, HeapAlignment alignment, size_t new_len, uintptr_t ra) {
    HeapThreadSafeAllocator *self = (HeapThreadSafeAllocator *)ctx;
    heap_mutex_lock(&self->mutex);
    bool ok = self->child.vtable->resize(self->child.ctx, ptr, old_len, alignment, new_len, ra);
    heap_mutex_unlock(&self->mutex);
    return ok;
}

static void *tsa_remap(void *ctx, void *ptr, size_t old_len, HeapAlignment alignment, size_t new_len, uintptr_t ra) {
    HeapThreadSafeAllocator *self = (HeapThreadSafeAllocator *)ctx;
    heap_mutex_lock(&self->mutex);
    void *p = self->child.vtable->remap(self->child.ctx, ptr, old_len, alignment, new_len, ra);
    heap_mutex_unlock(&self->mutex);
    return p;
}

static void tsa_free(void *ctx, void *ptr, size_t len, HeapAlignment alignment, uintptr_t ra) {
    HeapThreadSafeAllocator *self = (HeapThreadSafeAllocator *)ctx;
    heap_mutex_lock(&self->mutex);
    self->child.vtable->free(self->child.ctx, ptr, len, alignment, ra);
    heap_mutex_unlock(&self->mutex);
}
