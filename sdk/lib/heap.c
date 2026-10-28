#include <anix/libc.h>
#include <anix/stdlib.h>
#include <anix/syscall.h>
#include <stdbool.h>
#include <stdint.h>

#define HEAP_ALIGNMENT 16u
#define BLOCK_MAGIC 0x414e49584d454d31ULL

struct heap_block {
    size_t size;
    struct heap_block *previous;
    struct heap_block *next;
    uint64_t magic;
    bool free;
    uint8_t reserved[7];
} __attribute__((aligned(HEAP_ALIGNMENT)));

_Static_assert(sizeof(struct heap_block) % HEAP_ALIGNMENT == 0, "heap block header alignment");

static struct heap_block *block_head;
static struct heap_block *block_tail;
static uintptr_t heap_start;
static uintptr_t heap_end;
static bool heap_initialized;

static bool align_size(size_t size, size_t *aligned) {
    if (!aligned || size > SIZE_MAX - (HEAP_ALIGNMENT - 1u)) return false;
    *aligned = (size + HEAP_ALIGNMENT - 1u) & ~(size_t)(HEAP_ALIGNMENT - 1u);
    if (*aligned == 0) *aligned = HEAP_ALIGNMENT;
    return true;
}

static bool ensure_heap(void) {
    if (heap_initialized) return true;
    void *break_address = anix_sbrk(0);
    if (break_address == (void *)-1 || ((uintptr_t)break_address & (HEAP_ALIGNMENT - 1u))) return false;
    heap_start = heap_end = (uintptr_t)break_address;
    heap_initialized = true;
    return true;
}

static bool chain_valid(void) {
    if (!heap_initialized || heap_end < heap_start) return false;
    uintptr_t expected = heap_start;
    struct heap_block *previous = 0;
    size_t traversed = 0;
    for (struct heap_block *block = block_head; block; block = block->next) {
        if (++traversed > (heap_end - heap_start) / sizeof(struct heap_block) + 1u) return false;
        const uintptr_t address = (uintptr_t)block;
        if (address != expected || (address & (HEAP_ALIGNMENT - 1u)) ||
            address > heap_end || heap_end - address < sizeof(*block) ||
            block->magic != BLOCK_MAGIC || block->previous != previous ||
            (block->size & (HEAP_ALIGNMENT - 1u)) ||
            block->size > heap_end - address - sizeof(*block)) return false;
        expected = address + sizeof(*block) + block->size;
        previous = block;
    }
    return expected == heap_end && previous == block_tail &&
        ((block_head == 0) == (block_tail == 0));
}

static void split_block(struct heap_block *block, size_t needed) {
    const size_t remainder = block->size - needed;
    if (remainder < sizeof(*block) + HEAP_ALIGNMENT) return;
    struct heap_block *rest = (struct heap_block *)((uint8_t *)(block + 1) + needed);
    *rest = (struct heap_block){
        .size = remainder - sizeof(*rest),
        .previous = block,
        .next = block->next,
        .magic = BLOCK_MAGIC,
        .free = true,
    };
    if (rest->next) rest->next->previous = rest;
    else block_tail = rest;
    block->next = rest;
    block->size = needed;
}

static void merge_next(struct heap_block *block) {
    struct heap_block *next = block->next;
    if (!next || !next->free) return;
    block->size += sizeof(*next) + next->size;
    block->next = next->next;
    if (block->next) block->next->previous = block;
    else block_tail = block;
    next->magic = 0;
}

static bool extend_break(size_t amount, uintptr_t *old_break) {
    if (amount == 0 || amount > (size_t)INT64_MAX || anix_sbrk(0) != (void *)heap_end) return false;
    void *prior = anix_sbrk((long)amount);
    if (prior == (void *)-1 || (uintptr_t)prior != heap_end) return false;
    if (old_break) *old_break = heap_end;
    heap_end += amount;
    return true;
}

static void trim_free_tail(void) {
    struct heap_block *tail = block_tail;
    if (!tail || !tail->free || !chain_valid()) return;
    const size_t amount = sizeof(*tail) + tail->size;
    struct heap_block *previous = tail->previous;
    if (amount > (size_t)INT64_MAX || anix_sbrk(0) != (void *)heap_end) return;
    if (anix_sbrk(-(long)amount) != (void *)heap_end) return;
    heap_end -= amount;
    if (previous) previous->next = 0;
    else block_head = 0;
    block_tail = previous;
}

static struct heap_block *find_payload(const void *pointer) {
    if (!pointer || !chain_valid()) return 0;
    for (struct heap_block *block = block_head; block; block = block->next)
        if ((const void *)(block + 1) == pointer) return block;
    return 0;
}

void *anix_malloc(size_t size) {
    if (!align_size(size, &size) || !ensure_heap() || !chain_valid()) return 0;
    for (struct heap_block *block = block_head; block; block = block->next) {
        if (!block->free || block->size < size) continue;
        split_block(block, size);
        block->free = false;
        return block + 1;
    }
    if (size > SIZE_MAX - sizeof(struct heap_block)) return 0;
    const size_t amount = sizeof(struct heap_block) + size;
    uintptr_t address;
    if (!extend_break(amount, &address)) return 0;
    struct heap_block *block = (struct heap_block *)address;
    *block = (struct heap_block){
        .size = size,
        .previous = block_tail,
        .next = 0,
        .magic = BLOCK_MAGIC,
        .free = false,
    };
    if (block_tail) block_tail->next = block;
    else block_head = block;
    block_tail = block;
    return block + 1;
}

void anix_free(void *pointer) {
    if (!pointer) return;
    struct heap_block *block = find_payload(pointer);
    if (!block || block->free) return;
    block->free = true;
    if (block->next && block->next->free) merge_next(block);
    if (block->previous && block->previous->free) {
        block = block->previous;
        merge_next(block);
    }
    trim_free_tail();
}

void *anix_calloc(size_t count, size_t size) {
    if (size && count > SIZE_MAX / size) return 0;
    const size_t total = count * size;
    uint8_t *memory = anix_malloc(total);
    if (!memory) return 0;
    for (size_t i = 0; i < total; ++i) memory[i] = 0;
    return memory;
}

void *anix_realloc(void *pointer, size_t size) {
    if (!pointer) return anix_malloc(size);
    if (size == 0) { anix_free(pointer); return 0; }
    size_t needed;
    struct heap_block *block = find_payload(pointer);
    if (!block || block->free || !align_size(size, &needed)) return 0;
    if (needed <= block->size) {
        split_block(block, needed);
        if (block->next && block->next->next && block->next->next->free) merge_next(block->next);
        return pointer;
    }
    if (block->next && block->next->free && block->size <= SIZE_MAX - sizeof(*block) - block->next->size &&
        block->size + sizeof(*block) + block->next->size >= needed) {
        merge_next(block);
        split_block(block, needed);
        return pointer;
    }
    if (block == block_tail && needed - block->size <= SIZE_MAX - sizeof(*block)) {
        const size_t growth = needed - block->size;
        if (extend_break(growth, 0)) {
            block->size = needed;
            return pointer;
        }
    }
    void *replacement = anix_malloc(size);
    if (!replacement) return 0;
    (void)anix_memcpy(replacement, pointer, block->size);
    anix_free(pointer);
    return replacement;
}
