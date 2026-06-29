#include "heap.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>
#if defined(__linux__)
#define HEAP_HAS_MREMAP 1
#endif
#endif

static void *page_alloc_fn(void *ctx, size_t n, HeapAlignment alignment, uintptr_t ra);
static bool page_resize_fn(void *ctx, void *ptr, size_t old_len, HeapAlignment alignment, size_t new_len, uintptr_t ra);
static void *page_remap_fn(void *ctx, void *ptr, size_t old_len, HeapAlignment alignment, size_t new_len, uintptr_t ra);
static void page_free_fn(void *ctx, void *ptr, size_t len, HeapAlignment alignment, uintptr_t ra);

const HeapVTable heap_page_allocator_vtable = {
    page_alloc_fn,
    page_resize_fn,
    page_remap_fn,
    page_free_fn,
};

HeapAllocator heap_page_allocator(void) {
    HeapAllocator a = { NULL, &heap_page_allocator_vtable };
    return a;
}

void *heap_page_map(size_t n, HeapAlignment alignment) {
    size_t page = heap_page_size();
    size_t align = alignment.bytes < page ? page : alignment.bytes;
    if (!heap_is_power_of_two(align)) return NULL;
    if (n > SIZE_MAX - align - page) return NULL;
    size_t aligned_len = heap_align_forward(n, page);

#if defined(_WIN32)
    size_t over = aligned_len + align;
    void *base = VirtualAlloc(NULL, over, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!base) return NULL;
    uintptr_t b = (uintptr_t)base;
    uintptr_t p = heap_align_forward((size_t)b, align);
    if (p != b) VirtualFree(base, 0, MEM_RELEASE);
    if (p != b) {
        base = VirtualAlloc((void *)p, aligned_len, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!base) return NULL;
    }
    return base;
#else
    size_t over = heap_align_forward(aligned_len + align, page);
    void *base = mmap(NULL, over, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (base == MAP_FAILED) return NULL;
    uintptr_t b = (uintptr_t)base;
    uintptr_t p = heap_align_forward((size_t)b, align);
    size_t prefix = (size_t)(p - b);
    if (prefix) munmap(base, prefix);
    size_t suffix_start = prefix + aligned_len;
    if (suffix_start < over) munmap((unsigned char *)base + suffix_start, over - suffix_start);
    return (void *)p;
#endif
}

void heap_page_unmap(void *ptr, size_t len) {
    if (!ptr) return;
    size_t page = heap_page_size();
    size_t aligned_len = heap_align_forward(len, page);
#if defined(_WIN32)
    (void)aligned_len;
    VirtualFree(ptr, 0, MEM_RELEASE);
#else
    munmap(ptr, aligned_len);
#endif
}

void *heap_page_realloc(void *ptr, size_t old_len, size_t new_len, bool may_move) {
    (void)may_move;
    size_t page = heap_page_size();
    size_t old_aligned = heap_align_forward(old_len, page);
    size_t new_aligned = heap_align_forward(new_len, page);
    if (old_aligned == new_aligned) return ptr;

#if defined(_WIN32)
    if (new_aligned < old_aligned) {
        void *tail = (unsigned char *)ptr + new_aligned;
        VirtualAlloc(tail, old_aligned - new_aligned, MEM_RESET, PAGE_NOACCESS);
        return ptr;
    }
    return NULL;
#else
#if HEAP_HAS_MREMAP
    int flags = may_move ? MREMAP_MAYMOVE : 0;
    void *p = mremap(ptr, old_aligned, new_aligned, flags);
    if (p != MAP_FAILED) return p;
#endif
    if (new_aligned < old_aligned) {
        munmap((unsigned char *)ptr + new_aligned, old_aligned - new_aligned);
        return ptr;
    }
    return NULL;
#endif
}

static void *page_alloc_fn(void *ctx, size_t n, HeapAlignment alignment, uintptr_t ra) {
    (void)ctx; (void)ra;
    assert(n > 0);
    return heap_page_map(n, alignment);
}

static bool page_resize_fn(void *ctx, void *ptr, size_t old_len, HeapAlignment alignment, size_t new_len, uintptr_t ra) {
    (void)ctx; (void)alignment; (void)ra;
    return heap_page_realloc(ptr, old_len, new_len, false) != NULL;
}

static void *page_remap_fn(void *ctx, void *ptr, size_t old_len, HeapAlignment alignment, size_t new_len, uintptr_t ra) {
    (void)ctx; (void)alignment; (void)ra;
    return heap_page_realloc(ptr, old_len, new_len, true);
}

static void page_free_fn(void *ctx, void *ptr, size_t len, HeapAlignment alignment, uintptr_t ra) {
    (void)ctx; (void)alignment; (void)ra;
    heap_page_unmap(ptr, len);
}
