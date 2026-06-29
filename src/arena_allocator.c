#include "heap.h"

#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>

static void *arena_alloc_fn(void *ctx, size_t n, HeapAlignment alignment, uintptr_t ra);
static bool arena_resize_fn(void *ctx, void *ptr, size_t old_len, HeapAlignment alignment, size_t new_len, uintptr_t ra);
static void *arena_remap_fn(void *ctx, void *ptr, size_t old_len, HeapAlignment alignment, size_t new_len, uintptr_t ra);
static void arena_free_fn(void *ctx, void *ptr, size_t len, HeapAlignment alignment, uintptr_t ra);

static const HeapVTable arena_vtable = { arena_alloc_fn, arena_resize_fn, arena_remap_fn, arena_free_fn };
static const HeapAlignment node_alignment = { _Alignof(HeapArenaBufNode) };

void heap_arena_init(HeapArenaAllocator *self, HeapAllocator child) {
    self->child = child;
    self->state.first = NULL;
    self->state.end_index = 0;
}

void heap_arena_deinit(HeapArenaAllocator *self) {
    HeapArenaBufNode *it = self->state.first;
    while (it) {
        HeapArenaBufNode *next = it->next;
        self->child.vtable->free(self->child.ctx, it, it->data, node_alignment, 0);
        it = next;
    }
    self->state.first = NULL;
    self->state.end_index = 0;
}

HeapAllocator heap_arena_allocator(HeapArenaAllocator *self) {
    HeapAllocator a = { self, &arena_vtable };
    return a;
}

size_t heap_arena_query_capacity(HeapArenaAllocator *self) {
    size_t size = 0;
    for (HeapArenaBufNode *it = self->state.first; it; it = it->next) {
        size += it->data - sizeof(HeapArenaBufNode);
    }
    return size;
}

static HeapArenaBufNode *arena_create_node(HeapArenaAllocator *self, size_t prev_len, size_t minimum_size) {
    size_t actual_min = minimum_size + sizeof(HeapArenaBufNode) + 16;
    size_t big_enough = prev_len + actual_min;
    size_t len = big_enough + big_enough / 2;
    void *ptr = self->child.vtable->alloc(self->child.ctx, len, node_alignment, 0);
    if (!ptr) return NULL;
    HeapArenaBufNode *node = (HeapArenaBufNode *)ptr;
    node->data = len;
    node->next = self->state.first;
    self->state.first = node;
    self->state.end_index = 0;
    return node;
}

bool heap_arena_reset(HeapArenaAllocator *self, HeapArenaResetMode mode) {
    size_t requested = 0;
    if (mode.kind == HEAP_ARENA_RETAIN_CAPACITY) requested = heap_arena_query_capacity(self);
    if (mode.kind == HEAP_ARENA_RETAIN_WITH_LIMIT) {
        size_t cap = heap_arena_query_capacity(self);
        requested = cap < mode.limit ? cap : mode.limit;
    }
    if (requested == 0) {
        heap_arena_deinit(self);
        return true;
    }

    size_t total_size = requested + sizeof(HeapArenaBufNode);
    HeapArenaBufNode *it = self->state.first;
    HeapArenaBufNode *last = NULL;
    while (it) {
        HeapArenaBufNode *next = it->next;
        if (!next) {
            last = it;
            break;
        }
        self->child.vtable->free(self->child.ctx, it, it->data, node_alignment, 0);
        it = next;
    }
    self->state.first = last;
    self->state.end_index = 0;
    if (!last) return true;
    last->next = NULL;
    if (last->data == total_size) return true;
    if (self->child.vtable->resize(self->child.ctx, last, last->data, node_alignment, total_size, 0)) {
        last->data = total_size;
        return true;
    }
    void *new_ptr = self->child.vtable->alloc(self->child.ctx, total_size, node_alignment, 0);
    if (!new_ptr) return false;
    self->child.vtable->free(self->child.ctx, last, last->data, node_alignment, 0);
    HeapArenaBufNode *node = (HeapArenaBufNode *)new_ptr;
    node->data = total_size;
    node->next = NULL;
    self->state.first = node;
    return true;
}

static void *arena_alloc_fn(void *ctx, size_t n, HeapAlignment alignment, uintptr_t ra) {
    (void)ra;
    HeapArenaAllocator *self = (HeapArenaAllocator *)ctx;
    size_t align = alignment.bytes;
    HeapArenaBufNode *cur = self->state.first;
    if (!cur) {
        cur = arena_create_node(self, 0, n + align);
        if (!cur) return NULL;
    }
    for (;;) {
        unsigned char *buf = (unsigned char *)cur + sizeof(HeapArenaBufNode);
        size_t buf_len = cur->data - sizeof(HeapArenaBufNode);
        uintptr_t addr = (uintptr_t)buf + self->state.end_index;
        uintptr_t adjusted = heap_align_forward((size_t)addr, align);
        size_t adjusted_index = self->state.end_index + (size_t)(adjusted - addr);
        size_t new_end = adjusted_index + n;
        if (new_end <= buf_len) {
            self->state.end_index = new_end;
            return buf + adjusted_index;
        }
        size_t bigger = sizeof(HeapArenaBufNode) + new_end;
        if (self->child.vtable->resize(self->child.ctx, cur, cur->data, node_alignment, bigger, 0)) {
            cur->data = bigger;
        } else {
            cur = arena_create_node(self, buf_len, n + align);
            if (!cur) return NULL;
        }
    }
}

static bool arena_resize_fn(void *ctx, void *ptr, size_t old_len, HeapAlignment alignment, size_t new_len, uintptr_t ra) {
    (void)alignment; (void)ra;
    HeapArenaAllocator *self = (HeapArenaAllocator *)ctx;
    HeapArenaBufNode *node = self->state.first;
    if (!node) return false;
    unsigned char *buf = (unsigned char *)node + sizeof(HeapArenaBufNode);
    size_t buf_len = node->data - sizeof(HeapArenaBufNode);
    if (buf + self->state.end_index != (unsigned char *)ptr + old_len) return new_len <= old_len;
    if (old_len >= new_len) {
        self->state.end_index -= old_len - new_len;
        return true;
    }
    size_t add = new_len - old_len;
    if (buf_len - self->state.end_index >= add) {
        self->state.end_index += add;
        return true;
    }
    return false;
}

static void *arena_remap_fn(void *ctx, void *ptr, size_t old_len, HeapAlignment alignment, size_t new_len, uintptr_t ra) {
    return arena_resize_fn(ctx, ptr, old_len, alignment, new_len, ra) ? ptr : NULL;
}

static void arena_free_fn(void *ctx, void *ptr, size_t len, HeapAlignment alignment, uintptr_t ra) {
    (void)alignment; (void)ra;
    HeapArenaAllocator *self = (HeapArenaAllocator *)ctx;
    HeapArenaBufNode *node = self->state.first;
    if (!node) return;
    unsigned char *buf = (unsigned char *)node + sizeof(HeapArenaBufNode);
    if (buf + self->state.end_index == (unsigned char *)ptr + len) {
        self->state.end_index -= len;
    }
}
