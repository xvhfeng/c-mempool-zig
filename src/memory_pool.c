#include "heap.h"

#include <stddef.h>
#include <string.h>

typedef struct PoolNode {
    struct PoolNode *next;
} PoolNode;

bool heap_memory_pool_init(HeapMemoryPool *pool, HeapAllocator allocator, size_t item_size, size_t item_alignment, bool growable, size_t capacity) {
    size_t node_size = sizeof(PoolNode);
    size_t node_align = _Alignof(PoolNode);
    pool->item_size = item_size > node_size ? item_size : node_size;
    pool->item_alignment = item_alignment > node_align ? item_alignment : node_align;
    pool->growable = growable;
    pool->free_list = NULL;
    heap_arena_init(&pool->arena, allocator);
    if (capacity && !heap_memory_pool_add_capacity(pool, capacity)) {
        heap_memory_pool_deinit(pool);
        return false;
    }
    return true;
}

void heap_memory_pool_deinit(HeapMemoryPool *pool) {
    heap_arena_deinit(&pool->arena);
    pool->free_list = NULL;
}

static void *pool_alloc_new(HeapMemoryPool *pool) {
    return heap_alloc(heap_arena_allocator(&pool->arena), pool->item_size, heap_align_from(pool->item_alignment));
}

bool heap_memory_pool_add_capacity(HeapMemoryPool *pool, size_t count) {
    for (size_t i = 0; i < count; i++) {
        PoolNode *node = (PoolNode *)pool_alloc_new(pool);
        if (!node) return false;
        node->next = (PoolNode *)pool->free_list;
        pool->free_list = node;
    }
    return true;
}

bool heap_memory_pool_reset(HeapMemoryPool *pool, HeapArenaResetMode mode) {
    bool ok = heap_arena_reset(&pool->arena, mode);
    pool->free_list = NULL;
    return ok;
}

void *heap_memory_pool_create(HeapMemoryPool *pool) {
    PoolNode *node = (PoolNode *)pool->free_list;
    if (node) {
        pool->free_list = node->next;
        memset(node, 0, pool->item_size);
        return node;
    }
    if (!pool->growable) return NULL;
    void *ptr = pool_alloc_new(pool);
    if (ptr) memset(ptr, 0, pool->item_size);
    return ptr;
}

void heap_memory_pool_destroy(HeapMemoryPool *pool, void *ptr) {
    if (!ptr) return;
    PoolNode *node = (PoolNode *)ptr;
    node->next = (PoolNode *)pool->free_list;
    pool->free_list = node;
}
