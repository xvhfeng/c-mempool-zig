#include "heap.h"

bool heap_wasm_allocator_init(HeapWasmAllocator *self, HeapSbrkFn memory_grow_sbrk) {
    return heap_sbrk_allocator_init(&self->inner, memory_grow_sbrk);
}

void heap_wasm_allocator_deinit(HeapWasmAllocator *self) {
    heap_sbrk_allocator_deinit(&self->inner);
}

HeapAllocator heap_wasm_allocator(HeapWasmAllocator *self) {
    return heap_sbrk_allocator(&self->inner);
}
