#include <anix/heap.h>
#include <anix/kva.h>
#include <anix/panic.h>
#include <anix/pmm.h>
#include <anix/spinlock.h>
#include <anix/string.h>

#define HEAP_REGION_PAGES 4u
#define HEAP_ALIGNMENT 16u
#define HEAP_TRAILER_SIZE 16u
#define HEAP_REDZONE_SIZE sizeof(uint64_t)
#define HEAP_BLOCK_MAGIC 0x414e495848454150ULL
#define HEAP_ALLOCATED_TAG 0x9e3779b97f4a7c15ULL
#define HEAP_FREE_TAG 0xd1b54a32d192ed03ULL
#define HEAP_REDZONE_MAGIC 0x484541505245445aULL
#define HEAP_FREE_PATTERN 0xdd

struct heap_block {
    size_t size;
    uint64_t magic;
    uint8_t free;
    uint8_t reserved[7];
    uint64_t alignment_padding;
    struct heap_block *previous;
    struct heap_block *next;
};
_Static_assert(sizeof(struct heap_block) % HEAP_ALIGNMENT == 0,
               "heap blocks must preserve 16-byte alignment");

static struct heap_block *heap_blocks;
static struct anix_spinlock heap_lock;
static bool heap_initialized;

static size_t align_size(size_t size) {
    if (size > SIZE_MAX - (HEAP_ALIGNMENT - 1u)) return 0;
    return (size + HEAP_ALIGNMENT - 1u) & ~(size_t)(HEAP_ALIGNMENT - 1u);
}

static uint64_t block_magic(const struct heap_block *block) {
    return HEAP_BLOCK_MAGIC ^ (uint64_t)(uintptr_t)block ^ (uint64_t)block->size ^
        (block->free ? HEAP_FREE_TAG : HEAP_ALLOCATED_TAG);
}

static uint64_t redzone_magic(const struct heap_block *block) {
    return HEAP_REDZONE_MAGIC ^ (uint64_t)(uintptr_t)block ^ (uint64_t)block->size;
}

static uint64_t *block_redzone(struct heap_block *block) {
    return (uint64_t *)((uint8_t *)(block + 1) + block->size);
}

static bool block_header_mapped(const struct heap_block *block) {
    return block && !((uintptr_t)block & (HEAP_ALIGNMENT - 1u)) &&
        kva_range_is_mapped(block, sizeof(*block));
}

static bool block_valid(const struct heap_block *block) {
    if (!block_header_mapped(block)) return false;
    if (block->free > 1u || !block->size || (block->size & (HEAP_ALIGNMENT - 1u)) ||
        block->size > KVA_PAGE_COUNT * PMM_PAGE_SIZE - sizeof(*block) - HEAP_TRAILER_SIZE)
        return false;
    const uint8_t *payload = (const uint8_t *)(block + 1);
    if (!kva_range_is_mapped(payload, block->size + HEAP_TRAILER_SIZE)) return false;
    if (block->magic != block_magic(block)) return false;
    return *block_redzone((struct heap_block *)block) == redzone_magic(block);
}

static void seal_block(struct heap_block *block) {
    block->magic = block_magic(block);
    *block_redzone(block) = redzone_magic(block);
}

static bool block_links_valid(const struct heap_block *block) {
    if (!block_valid(block)) return false;
    if (block->previous && (!block_valid(block->previous) ||
        block->previous->next != block || (uintptr_t)block->previous >= (uintptr_t)block)) return false;
    if (block->next && (!block_valid(block->next) ||
        block->next->previous != block || (uintptr_t)block->next <= (uintptr_t)block)) return false;
    return true;
}

static void insert_block(struct heap_block *block) {
    struct heap_block *cursor = heap_blocks;
    struct heap_block *previous = 0;
    while (cursor && (uintptr_t)cursor < (uintptr_t)block) {
        if (!block_links_valid(cursor)) panic("kernel heap free-list corruption");
        previous = cursor;
        cursor = cursor->next;
    }
    if ((previous && (uintptr_t)((uint8_t *)(previous + 1) + previous->size + HEAP_TRAILER_SIZE) >
                       (uintptr_t)block) ||
        (cursor && (uintptr_t)((uint8_t *)(block + 1) + block->size + HEAP_TRAILER_SIZE) >
                    (uintptr_t)cursor)) panic("overlapping kernel heap regions");
    block->previous = previous;
    block->next = cursor;
    if (previous) previous->next = block;
    else heap_blocks = block;
    if (cursor) cursor->previous = block;
}

static bool grow_heap(void) {
    const size_t bytes = HEAP_REGION_PAGES * PMM_PAGE_SIZE;
    struct heap_block *block = kva_alloc_pages(HEAP_REGION_PAGES, VMM_WRITABLE | VMM_NO_EXECUTE);
    if (!block) return false;
    block->size = (bytes - sizeof(*block) - HEAP_TRAILER_SIZE) & ~(size_t)(HEAP_ALIGNMENT - 1u);
    block->free = 1u;
    kmemset(block->reserved, 0, sizeof(block->reserved));
    block->alignment_padding = 0;
    block->previous = 0;
    block->next = 0;
    seal_block(block);
    insert_block(block);
    return true;
}

bool kheap_init(void) {
    uintptr_t lock_flags = anix_spin_lock_irqsave(&heap_lock);
    if (heap_initialized) {
        anix_spin_unlock_irqrestore(&heap_lock, lock_flags);
        return false;
    }
    heap_blocks = 0;
    if (!grow_heap()) {
        anix_spin_unlock_irqrestore(&heap_lock, lock_flags);
        return false;
    }
    heap_initialized = true;
    anix_spin_unlock_irqrestore(&heap_lock, lock_flags);
    return true;
}

static void split_block(struct heap_block *block, size_t needed) {
    if (block->size < needed + HEAP_TRAILER_SIZE + sizeof(*block) + HEAP_ALIGNMENT) return;
    struct heap_block *remainder = (struct heap_block *)
        ((uint8_t *)(block + 1) + needed + HEAP_TRAILER_SIZE);
    remainder->size = block->size - needed - HEAP_TRAILER_SIZE - sizeof(*remainder);
    remainder->free = 1u;
    kmemset(remainder->reserved, 0, sizeof(remainder->reserved));
    remainder->alignment_padding = 0;
    remainder->previous = block;
    remainder->next = block->next;
    if (remainder->next) remainder->next->previous = remainder;
    block->next = remainder;
    block->size = needed;
    seal_block(block);
    seal_block(remainder);
}

static void *allocate_locked(size_t needed) {
    for (;;) {
        for (struct heap_block *block = heap_blocks; block;) {
            if (!block_links_valid(block)) panic("kernel heap metadata or redzone corrupted");
            struct heap_block *next = block->next;
            if (block->free && block->size >= needed) {
                split_block(block, needed);
                block->free = 0u;
                seal_block(block);
                return block + 1;
            }
            block = next;
        }
        if (!grow_heap()) return 0;
    }
}

void *kmalloc(size_t size) {
    if (!heap_initialized || size == 0) return 0;
    const size_t needed = align_size(size);
    if (needed == 0) return 0;
    uintptr_t lock_flags = anix_spin_lock_irqsave(&heap_lock);
    void *result = allocate_locked(needed);
    anix_spin_unlock_irqrestore(&heap_lock, lock_flags);
    return result;
}

void *kcalloc(size_t count, size_t size) {
    if (count != 0 && size > SIZE_MAX / count) return 0;
    const size_t total = count * size;
    void *result = kmalloc(total);
    if (result) kmemset(result, 0, total);
    return result;
}

static bool merge_next(struct heap_block *block) {
    if (!block_links_valid(block)) return false;
    struct heap_block *next = block->next;
    if (!next || !next->free ||
        (uint8_t *)(block + 1) + block->size + HEAP_TRAILER_SIZE != (uint8_t *)next) return true;
    if (!block_links_valid(next) || block->size > SIZE_MAX - HEAP_TRAILER_SIZE - sizeof(*next) - next->size)
        return false;
    block->size += HEAP_TRAILER_SIZE + sizeof(*next) + next->size;
    block->next = next->next;
    if (block->next) block->next->previous = block;
    next->magic = 0;
    seal_block(block);
    return true;
}

bool kfree(void *pointer) {
    if (!pointer) return true;
    if (!heap_initialized || (uintptr_t)pointer < KVA_BASE + sizeof(struct heap_block)) return false;
    struct heap_block *block = (struct heap_block *)pointer - 1;
    uintptr_t lock_flags = anix_spin_lock_irqsave(&heap_lock);
    if (!block_links_valid(block) || block->free) {
        anix_spin_unlock_irqrestore(&heap_lock, lock_flags);
        return false;
    }
    kmemset(pointer, HEAP_FREE_PATTERN, block->size);
    block->free = 1u;
    seal_block(block);
    if (!merge_next(block)) {
        anix_spin_unlock_irqrestore(&heap_lock, lock_flags);
        return false;
    }
    if (block->previous && block->previous->free) {
        block = block->previous;
        if (!merge_next(block)) {
            anix_spin_unlock_irqrestore(&heap_lock, lock_flags);
            return false;
        }
    }
    anix_spin_unlock_irqrestore(&heap_lock, lock_flags);
    return true;
}

void *krealloc(void *pointer, size_t size) {
    if (!pointer) return kmalloc(size);
    if (size == 0) {
        (void)kfree(pointer);
        return 0;
    }
    if (!heap_initialized || (uintptr_t)pointer < KVA_BASE + sizeof(struct heap_block)) return 0;
    const size_t needed = align_size(size);
    if (!needed) return 0;
    struct heap_block *block = (struct heap_block *)pointer - 1;
    uintptr_t lock_flags = anix_spin_lock_irqsave(&heap_lock);
    if (!block_links_valid(block) || block->free) {
        anix_spin_unlock_irqrestore(&heap_lock, lock_flags);
        return 0;
    }
    const size_t old_size = block->size;
    if (needed <= old_size) {
        anix_spin_unlock_irqrestore(&heap_lock, lock_flags);
        return pointer;
    }
    anix_spin_unlock_irqrestore(&heap_lock, lock_flags);

    void *replacement = kmalloc(size);
    if (!replacement) return 0;
    kmemcpy(replacement, pointer, old_size);
    if (!kfree(pointer)) {
        (void)kfree(replacement);
        return 0;
    }
    return replacement;
}
