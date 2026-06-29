#include "heap.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32) && !defined(__wasm__)
#include <execinfo.h>
#endif

typedef enum TraceKind {
    TRACE_ALLOC = 0,
    TRACE_FREE = 1,
} TraceKind;

typedef struct DebugBucketHeader {
    size_t allocated_count;
    size_t freed_count;
    struct DebugBucketHeader *prev;
    struct DebugBucketHeader *next;
    uintptr_t canary;
} DebugBucketHeader;

typedef struct DebugLargeAlloc {
    unsigned char *ptr;
    size_t len;
    size_t requested_size;
    uintptr_t stack_addresses[2][HEAP_DEBUG_MAX_FRAMES];
    bool freed;
    HeapAlignment alignment;
} DebugLargeAlloc;

typedef struct DebugLargeTable {
    DebugLargeAlloc *items;
    size_t len;
    size_t cap;
} DebugLargeTable;

struct HeapDebugAllocator {
    HeapAllocator backing;
    DebugBucketHeader **buckets;
    size_t *slot_counts;
    size_t small_bucket_count;
    DebugLargeTable large;
    size_t total_requested_bytes;
    size_t requested_memory_limit;
    HeapMutex mutex;
    HeapDebugConfig config;
};

static void *debug_alloc_fn(void *ctx, size_t len, HeapAlignment alignment, uintptr_t ra);
static bool debug_resize_fn(void *ctx, void *ptr, size_t old_len, HeapAlignment alignment, size_t new_len, uintptr_t ra);
static void *debug_remap_fn(void *ctx, void *ptr, size_t old_len, HeapAlignment alignment, size_t new_len, uintptr_t ra);
static void debug_free_fn(void *ctx, void *ptr, size_t len, HeapAlignment alignment, uintptr_t ra);

static const HeapVTable debug_vtable = { debug_alloc_fn, debug_resize_fn, debug_remap_fn, debug_free_fn };

HeapDebugConfig heap_debug_default_config(void) {
    HeapDebugConfig c;
    c.stack_trace_frames = 6;
    c.enable_memory_limit = false;
    c.safety = true;
    c.thread_safe = true;
    c.never_unmap = false;
    c.retain_metadata = false;
    c.verbose_log = false;
    c.backing_allocator_zeroes = true;
    c.resize_stack_traces = false;
    c.canary = (uintptr_t)0x9232a6ff85dff10fULL;
    size_t p = heap_page_size();
    c.page_size = p > 128 * 1024 ? p : 128 * 1024;
    return c;
}

static size_t trace_frames(HeapDebugAllocator *self) {
    return self->config.stack_trace_frames > HEAP_DEBUG_MAX_FRAMES ? HEAP_DEBUG_MAX_FRAMES : self->config.stack_trace_frames;
}

static void capture_trace(HeapDebugAllocator *self, uintptr_t out[HEAP_DEBUG_MAX_FRAMES]) {
    size_t n = trace_frames(self);
    memset(out, 0, sizeof(uintptr_t) * HEAP_DEBUG_MAX_FRAMES);
#if !defined(_WIN32) && !defined(__wasm__)
    void *buf[HEAP_DEBUG_MAX_FRAMES];
    int got = backtrace(buf, (int)n);
    for (int i = 0; i < got && i < (int)HEAP_DEBUG_MAX_FRAMES; i++) out[i] = (uintptr_t)buf[i];
#else
    (void)n;
#endif
}

static void print_trace(HeapDebugAllocator *self, const uintptr_t trace[HEAP_DEBUG_MAX_FRAMES]) {
    size_t n = trace_frames(self);
    for (size_t i = 0; i < n && trace[i]; i++) fprintf(stderr, "    #%zu 0x%zx\n", i, (size_t)trace[i]);
}

static void report_double_free(HeapDebugAllocator *self, const uintptr_t alloc_trace[HEAP_DEBUG_MAX_FRAMES], const uintptr_t free_trace[HEAP_DEBUG_MAX_FRAMES]) {
    uintptr_t second[HEAP_DEBUG_MAX_FRAMES];
    capture_trace(self, second);
    fprintf(stderr, "Double free detected. Allocation:\n");
    print_trace(self, alloc_trace);
    fprintf(stderr, "First free:\n");
    print_trace(self, free_trace);
    fprintf(stderr, "Second free:\n");
    print_trace(self, second);
}

static bool large_reserve(DebugLargeTable *t, size_t extra) {
    if (t->len + extra <= t->cap) return true;
    size_t nc = t->cap ? t->cap * 2 : 16;
    while (nc < t->len + extra) nc *= 2;
    DebugLargeAlloc *items = (DebugLargeAlloc *)realloc(t->items, nc * sizeof(*items));
    if (!items) return false;
    t->items = items;
    t->cap = nc;
    return true;
}

static DebugLargeAlloc *large_find(DebugLargeTable *t, void *ptr) {
    for (size_t i = 0; i < t->len; i++) {
        if (t->items[i].ptr == (unsigned char *)ptr) return &t->items[i];
    }
    return NULL;
}

static void large_remove(DebugLargeTable *t, void *ptr) {
    for (size_t i = 0; i < t->len; i++) {
        if (t->items[i].ptr == (unsigned char *)ptr) {
            t->items[i] = t->items[t->len - 1];
            t->len--;
            return;
        }
    }
}

static size_t used_bits_count(size_t slot_count) {
    return (slot_count + sizeof(size_t) * 8 - 1) / (sizeof(size_t) * 8);
}

static size_t used_bits_size(size_t slot_count) {
    return used_bits_count(slot_count) * sizeof(size_t);
}

static size_t bucket_requested_sizes_start(HeapDebugAllocator *self, size_t slot_count) {
    (void)self;
    return heap_align_forward(sizeof(DebugBucketHeader) + used_bits_size(slot_count), _Alignof(size_t));
}

static size_t bucket_aligns_start(HeapDebugAllocator *self, size_t slot_count) {
    return bucket_requested_sizes_start(self, slot_count) + sizeof(size_t) * slot_count;
}

static size_t bucket_stack_start(HeapDebugAllocator *self, size_t slot_count) {
    size_t start = self->config.safety
        ? bucket_aligns_start(self, slot_count) + sizeof(size_t) * slot_count
        : sizeof(DebugBucketHeader) + used_bits_size(slot_count);
    return heap_align_forward(start, _Alignof(uintptr_t));
}

static size_t bucket_size(HeapDebugAllocator *self, size_t slot_count) {
    return bucket_stack_start(self, slot_count) + sizeof(uintptr_t) * trace_frames(self) * 2 * slot_count;
}

static DebugBucketHeader *bucket_from_page(HeapDebugAllocator *self, uintptr_t page_addr, size_t slot_count) {
    uintptr_t unaligned = page_addr + self->config.page_size - bucket_size(self, slot_count);
    return (DebugBucketHeader *)(unaligned & ~(uintptr_t)(_Alignof(DebugBucketHeader) - 1));
}

static size_t *bucket_used_bits(DebugBucketHeader *bucket, size_t index) {
    unsigned char *ptr = (unsigned char *)bucket;
    return &((size_t *)(void *)(ptr + sizeof(DebugBucketHeader)))[index];
}

static size_t *bucket_requested_sizes(HeapDebugAllocator *self, DebugBucketHeader *bucket, size_t slot_count) {
    return (size_t *)(void *)((unsigned char *)bucket + bucket_requested_sizes_start(self, slot_count));
}

static size_t *bucket_aligns(HeapDebugAllocator *self, DebugBucketHeader *bucket, size_t slot_count) {
    return (size_t *)(void *)((unsigned char *)bucket + bucket_aligns_start(self, slot_count));
}

static uintptr_t *bucket_trace_ptr(HeapDebugAllocator *self, DebugBucketHeader *bucket, size_t slot_count, size_t slot_index, TraceKind kind) {
    size_t n = trace_frames(self);
    unsigned char *start = (unsigned char *)bucket + bucket_stack_start(self, slot_count);
    return (uintptr_t *)(void *)(start + sizeof(uintptr_t) * n * 2 * slot_index + sizeof(uintptr_t) * n * (size_t)kind);
}

static size_t calculate_slot_count(HeapDebugAllocator *self, size_t size_class_index) {
    size_t size_class = (size_t)1 << size_class_index;
    size_t lower = 2;
    size_t upper = (self->config.page_size - bucket_size(self, lower)) / size_class;
    while (upper > lower) {
        size_t proposed = lower + (upper - lower) / 2;
        if (proposed == lower) return lower;
        size_t slots_end = proposed * size_class;
        size_t header_begin = heap_align_forward(slots_end, _Alignof(DebugBucketHeader));
        size_t end = header_begin + bucket_size(self, proposed);
        if (end > self->config.page_size) upper = proposed - 1;
        else lower = proposed;
    }
    return lower;
}

static size_t debug_size_class(HeapDebugAllocator *self, size_t len, HeapAlignment alignment) {
    (void)self;
    size_t l = heap_log2_floor(heap_ceil_pow2(len ? len : 1));
    size_t a = heap_log2_floor(heap_ceil_pow2(alignment.bytes ? alignment.bytes : 1));
    return l > a ? l : a;
}

bool heap_debug_allocator_init(HeapDebugAllocator **out, HeapAllocator backing, HeapDebugConfig config) {
    if (!config.page_size) config.page_size = heap_debug_default_config().page_size;
    if (!heap_is_power_of_two(config.page_size)) return false;
    HeapDebugAllocator *self = (HeapDebugAllocator *)calloc(1, sizeof(*self));
    if (!self) return false;
    self->backing = backing.vtable ? backing : heap_page_allocator();
    self->config = config;
    self->requested_memory_limit = SIZE_MAX;
    self->small_bucket_count = heap_log2_floor(config.page_size) - 1;
    self->buckets = (DebugBucketHeader **)calloc(self->small_bucket_count, sizeof(*self->buckets));
    self->slot_counts = (size_t *)calloc(self->small_bucket_count, sizeof(*self->slot_counts));
    if (!self->buckets || !self->slot_counts) {
        free(self->buckets);
        free(self->slot_counts);
        free(self);
        return false;
    }
    heap_mutex_init(&self->mutex);
    for (size_t i = 0; i < self->small_bucket_count; i++) self->slot_counts[i] = calculate_slot_count(self, i);
    *out = self;
    return true;
}

HeapAllocator heap_debug_allocator(HeapDebugAllocator *self) {
    HeapAllocator a = { self, &debug_vtable };
    return a;
}

size_t heap_debug_total_requested_bytes(HeapDebugAllocator *self) {
    return self->total_requested_bytes;
}

void heap_debug_set_requested_memory_limit(HeapDebugAllocator *self, size_t limit) {
    self->requested_memory_limit = limit;
}

static bool memory_limit_add(HeapDebugAllocator *self, size_t n) {
    if (!self->config.enable_memory_limit) return true;
    if (n > SIZE_MAX - self->total_requested_bytes) return false;
    size_t next = self->total_requested_bytes + n;
    if (next > self->requested_memory_limit) return false;
    self->total_requested_bytes = next;
    return true;
}

static void memory_limit_sub(HeapDebugAllocator *self, size_t n) {
    if (self->config.enable_memory_limit) self->total_requested_bytes -= n;
}

static void *debug_alloc_large(HeapDebugAllocator *self, size_t len, HeapAlignment alignment, uintptr_t ra) {
    if (!large_reserve(&self->large, 1)) return NULL;
    void *ptr = self->backing.vtable->alloc(self->backing.ctx, len, alignment, ra);
    if (!ptr) return NULL;
    DebugLargeAlloc *la = &self->large.items[self->large.len++];
    memset(la, 0, sizeof(*la));
    la->ptr = (unsigned char *)ptr;
    la->len = len;
    la->requested_size = len;
    la->alignment = alignment;
    la->freed = false;
    capture_trace(self, la->stack_addresses[TRACE_ALLOC]);
    if (self->config.verbose_log) fprintf(stderr, "large alloc %zu bytes at %p\n", len, ptr);
    return ptr;
}

static void *debug_alloc_fn(void *ctx, size_t len, HeapAlignment alignment, uintptr_t ra) {
    HeapDebugAllocator *self = (HeapDebugAllocator *)ctx;
    if (self->config.thread_safe) heap_mutex_lock(&self->mutex);
    if (!memory_limit_add(self, len)) {
        if (self->config.thread_safe) heap_mutex_unlock(&self->mutex);
        return NULL;
    }
    size_t class = debug_size_class(self, len, alignment);
    if (class >= self->small_bucket_count) {
        void *p = debug_alloc_large(self, len, alignment, ra);
        if (!p) memory_limit_sub(self, len);
        if (self->config.thread_safe) heap_mutex_unlock(&self->mutex);
        return p;
    }
    size_t slot_count = self->slot_counts[class];
    DebugBucketHeader *bucket = self->buckets[class];
    if (!bucket || bucket->allocated_count >= slot_count) {
        void *page = self->backing.vtable->alloc(self->backing.ctx, self->config.page_size, heap_align_from(self->config.page_size), ra);
        if (!page) {
            memory_limit_sub(self, len);
            if (self->config.thread_safe) heap_mutex_unlock(&self->mutex);
            return NULL;
        }
        bucket = bucket_from_page(self, (uintptr_t)page, slot_count);
        memset(bucket, 0, bucket_size(self, slot_count));
        bucket->canary = self->config.canary;
        bucket->prev = self->buckets[class];
        if (bucket->prev) bucket->prev->next = bucket;
        self->buckets[class] = bucket;
    }
    size_t slot = bucket->allocated_count++;
    size_t used_i = slot / (sizeof(size_t) * 8);
    size_t bit = slot % (sizeof(size_t) * 8);
    *bucket_used_bits(bucket, used_i) |= (size_t)1 << bit;
    if (self->config.safety) {
        bucket_requested_sizes(self, bucket, slot_count)[slot] = len;
        bucket_aligns(self, bucket, slot_count)[slot] = alignment.bytes;
    }
    capture_trace(self, bucket_trace_ptr(self, bucket, slot_count, slot, TRACE_ALLOC));
    uintptr_t page_addr = (uintptr_t)bucket & ~(uintptr_t)(self->config.page_size - 1);
    void *ptr = (void *)(page_addr + slot * ((size_t)1 << class));
    if (self->config.verbose_log) fprintf(stderr, "small alloc %zu bytes at %p\n", len, ptr);
    if (self->config.thread_safe) heap_mutex_unlock(&self->mutex);
    return ptr;
}

static bool validate_small(HeapDebugAllocator *self, void *ptr, size_t len, HeapAlignment alignment, size_t class, size_t *slot_out, DebugBucketHeader **bucket_out) {
    size_t slot_count = self->slot_counts[class];
    uintptr_t addr = (uintptr_t)ptr;
    uintptr_t page_addr = addr & ~(uintptr_t)(self->config.page_size - 1);
    DebugBucketHeader *bucket = bucket_from_page(self, page_addr, slot_count);
    if (bucket->canary != self->config.canary) {
        if (self->config.safety) fprintf(stderr, "Invalid free\n");
        return false;
    }
    size_t size_class = (size_t)1 << class;
    size_t slot = (addr - page_addr) / size_class;
    size_t used_i = slot / (sizeof(size_t) * 8);
    size_t bit = slot % (sizeof(size_t) * 8);
    bool is_used = ((*bucket_used_bits(bucket, used_i) >> bit) & 1) != 0;
    if (!is_used) {
        if (self->config.safety) {
            report_double_free(self,
                bucket_trace_ptr(self, bucket, slot_count, slot, TRACE_ALLOC),
                bucket_trace_ptr(self, bucket, slot_count, slot, TRACE_FREE));
        }
        return false;
    }
    if (self->config.safety) {
        size_t requested = bucket_requested_sizes(self, bucket, slot_count)[slot];
        size_t stored_align = bucket_aligns(self, bucket, slot_count)[slot];
        if (requested == 0) {
            fprintf(stderr, "Invalid free\n");
            return false;
        }
        if (requested != len) fprintf(stderr, "Allocation size %zu bytes does not match free size %zu\n", requested, len);
        if (stored_align != alignment.bytes) fprintf(stderr, "Allocation alignment %zu does not match free alignment %zu\n", stored_align, alignment.bytes);
    }
    *slot_out = slot;
    *bucket_out = bucket;
    return true;
}

static void debug_free_large(HeapDebugAllocator *self, void *ptr, size_t len, HeapAlignment alignment, uintptr_t ra) {
    DebugLargeAlloc *la = large_find(&self->large, ptr);
    if (!la) {
        if (self->config.safety) fprintf(stderr, "Invalid free\n");
        return;
    }
    if (self->config.retain_metadata && la->freed) {
        report_double_free(self, la->stack_addresses[TRACE_ALLOC], la->stack_addresses[TRACE_FREE]);
        return;
    }
    if (self->config.safety && la->len != len) fprintf(stderr, "Allocation size %zu bytes does not match free size %zu\n", la->len, len);
    if (!self->config.never_unmap) self->backing.vtable->free(self->backing.ctx, ptr, len, alignment, ra);
    memory_limit_sub(self, la->requested_size);
    if (self->config.retain_metadata) {
        la->freed = true;
        capture_trace(self, la->stack_addresses[TRACE_FREE]);
    } else {
        large_remove(&self->large, ptr);
    }
    if (self->config.verbose_log) fprintf(stderr, "large free %zu bytes at %p\n", len, ptr);
}

static void debug_free_fn(void *ctx, void *ptr, size_t len, HeapAlignment alignment, uintptr_t ra) {
    HeapDebugAllocator *self = (HeapDebugAllocator *)ctx;
    if (self->config.thread_safe) heap_mutex_lock(&self->mutex);
    size_t class = debug_size_class(self, len, alignment);
    if (class >= self->small_bucket_count) {
        debug_free_large(self, ptr, len, alignment, ra);
        if (self->config.thread_safe) heap_mutex_unlock(&self->mutex);
        return;
    }
    size_t slot = 0;
    DebugBucketHeader *bucket = NULL;
    if (!validate_small(self, ptr, len, alignment, class, &slot, &bucket)) {
        if (self->config.thread_safe) heap_mutex_unlock(&self->mutex);
        return;
    }
    size_t slot_count = self->slot_counts[class];
    capture_trace(self, bucket_trace_ptr(self, bucket, slot_count, slot, TRACE_FREE));
    size_t used_i = slot / (sizeof(size_t) * 8);
    size_t bit = slot % (sizeof(size_t) * 8);
    *bucket_used_bits(bucket, used_i) &= ~((size_t)1 << bit);
    if (self->config.safety) bucket_requested_sizes(self, bucket, slot_count)[slot] = 0;
    bucket->freed_count++;
    memory_limit_sub(self, len);
    if (bucket->freed_count == bucket->allocated_count) {
        if (bucket->prev) bucket->prev->next = bucket->next;
        if (bucket->next) bucket->next->prev = bucket->prev;
        else self->buckets[class] = bucket->prev;
        if (!self->config.never_unmap) {
            uintptr_t page_addr = (uintptr_t)bucket & ~(uintptr_t)(self->config.page_size - 1);
            self->backing.vtable->free(self->backing.ctx, (void *)page_addr, self->config.page_size, heap_align_from(self->config.page_size), ra);
        }
    }
    if (self->config.verbose_log) fprintf(stderr, "small free %zu bytes at %p\n", len, ptr);
    if (self->config.thread_safe) heap_mutex_unlock(&self->mutex);
}

static bool debug_resize_small(HeapDebugAllocator *self, void *ptr, size_t old_len, HeapAlignment alignment, size_t new_len, uintptr_t ra, size_t class) {
    size_t new_class = debug_size_class(self, new_len, alignment);
    size_t slot = 0;
    DebugBucketHeader *bucket = NULL;
    if (!validate_small(self, ptr, old_len, alignment, class, &slot, &bucket)) return false;
    if (new_class != class) return false;
    if (self->config.enable_memory_limit) {
        size_t prev = self->total_requested_bytes;
        size_t next = prev - old_len + new_len;
        if (new_len > old_len && next > self->requested_memory_limit) return false;
        self->total_requested_bytes = next;
    }
    size_t slot_count = self->slot_counts[class];
    if (self->config.safety) bucket_requested_sizes(self, bucket, slot_count)[slot] = new_len;
    if (self->config.resize_stack_traces) capture_trace(self, bucket_trace_ptr(self, bucket, slot_count, slot, TRACE_ALLOC));
    if (self->config.verbose_log) fprintf(stderr, "small resize %zu bytes at %p to %zu\n", old_len, ptr, new_len);
    (void)ra;
    return true;
}

static void *debug_resize_large(HeapDebugAllocator *self, void *ptr, size_t old_len, HeapAlignment alignment, size_t new_len, uintptr_t ra, bool may_move) {
    DebugLargeAlloc *la = large_find(&self->large, ptr);
    if (!la || (self->config.retain_metadata && la->freed)) return NULL;
    if (self->config.safety && la->len != old_len) fprintf(stderr, "Allocation size %zu bytes does not match resize size %zu\n", la->len, old_len);
    if (debug_size_class(self, new_len, alignment) < self->small_bucket_count) return NULL;
    size_t prev_total = self->total_requested_bytes;
    if (self->config.enable_memory_limit) {
        size_t next = prev_total - la->requested_size + new_len;
        if (new_len > la->requested_size && next > self->requested_memory_limit) return NULL;
        self->total_requested_bytes = next;
    }
    void *resized = may_move
        ? self->backing.vtable->remap(self->backing.ctx, ptr, old_len, alignment, new_len, ra)
        : (self->backing.vtable->resize(self->backing.ctx, ptr, old_len, alignment, new_len, ra) ? ptr : NULL);
    if (!resized) {
        self->total_requested_bytes = prev_total;
        return NULL;
    }
    la->ptr = (unsigned char *)resized;
    la->len = new_len;
    la->requested_size = new_len;
    if (self->config.resize_stack_traces) capture_trace(self, la->stack_addresses[TRACE_ALLOC]);
    if (self->config.verbose_log) fprintf(stderr, "large resize %zu bytes at %p to %zu at %p\n", old_len, ptr, new_len, resized);
    return resized;
}

static bool debug_resize_fn(void *ctx, void *ptr, size_t old_len, HeapAlignment alignment, size_t new_len, uintptr_t ra) {
    HeapDebugAllocator *self = (HeapDebugAllocator *)ctx;
    if (self->config.thread_safe) heap_mutex_lock(&self->mutex);
    size_t class = debug_size_class(self, old_len, alignment);
    bool ok = class >= self->small_bucket_count
        ? debug_resize_large(self, ptr, old_len, alignment, new_len, ra, false) != NULL
        : debug_resize_small(self, ptr, old_len, alignment, new_len, ra, class);
    if (self->config.thread_safe) heap_mutex_unlock(&self->mutex);
    return ok;
}

static void *debug_remap_fn(void *ctx, void *ptr, size_t old_len, HeapAlignment alignment, size_t new_len, uintptr_t ra) {
    HeapDebugAllocator *self = (HeapDebugAllocator *)ctx;
    if (self->config.thread_safe) heap_mutex_lock(&self->mutex);
    size_t class = debug_size_class(self, old_len, alignment);
    void *p = class >= self->small_bucket_count
        ? debug_resize_large(self, ptr, old_len, alignment, new_len, ra, true)
        : (debug_resize_small(self, ptr, old_len, alignment, new_len, ra, class) ? ptr : NULL);
    if (self->config.thread_safe) heap_mutex_unlock(&self->mutex);
    return p;
}

size_t heap_debug_detect_leaks(HeapDebugAllocator *self) {
    size_t leaks = 0;
    if (self->config.thread_safe) heap_mutex_lock(&self->mutex);
    for (size_t class = 0; class < self->small_bucket_count; class++) {
        size_t slot_count = self->slot_counts[class];
        for (DebugBucketHeader *b = self->buckets[class]; b; b = b->prev) {
            for (size_t word = 0; word < used_bits_count(slot_count); word++) {
                size_t bits = *bucket_used_bits(b, word);
                while (bits) {
                    size_t bit = heap_log2_floor(bits & (~bits + 1));
                    size_t slot = word * sizeof(size_t) * 8 + bit;
                    uintptr_t page_addr = (uintptr_t)b & ~(uintptr_t)(self->config.page_size - 1);
                    void *addr = (void *)(page_addr + slot * ((size_t)1 << class));
                    fprintf(stderr, "memory address %p leaked\n", addr);
                    print_trace(self, bucket_trace_ptr(self, b, slot_count, slot, TRACE_ALLOC));
                    leaks++;
                    bits &= bits - 1;
                }
            }
        }
    }
    for (size_t i = 0; i < self->large.len; i++) {
        DebugLargeAlloc *la = &self->large.items[i];
        if (self->config.retain_metadata && la->freed) continue;
        fprintf(stderr, "memory address %p leaked\n", la->ptr);
        print_trace(self, la->stack_addresses[TRACE_ALLOC]);
        leaks++;
    }
    if (self->config.thread_safe) heap_mutex_unlock(&self->mutex);
    return leaks;
}

void heap_debug_flush_retained_metadata(HeapDebugAllocator *self) {
    if (!self->config.retain_metadata) return;
    for (size_t i = 0; i < self->large.len;) {
        DebugLargeAlloc *la = &self->large.items[i];
        if (la->freed) {
            if (self->config.never_unmap) self->backing.vtable->free(self->backing.ctx, la->ptr, la->len, la->alignment, 0);
            self->large.items[i] = self->large.items[self->large.len - 1];
            self->large.len--;
        } else {
            i++;
        }
    }
}

HeapCheck heap_debug_allocator_deinit(HeapDebugAllocator *self) {
    size_t leaks = self->config.safety ? heap_debug_detect_leaks(self) : 0;
    heap_debug_allocator_deinit_without_leak_check(self);
    HeapCheck c = { leaks ? HEAP_CHECK_LEAK : HEAP_CHECK_OK };
    return c;
}

void heap_debug_allocator_deinit_without_leak_check(HeapDebugAllocator *self) {
    if (!self) return;
    if (self->config.retain_metadata) heap_debug_flush_retained_metadata(self);
    free(self->large.items);
    free(self->buckets);
    free(self->slot_counts);
    heap_mutex_deinit(&self->mutex);
    free(self);
}
