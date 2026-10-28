#include <anix/vmm.h>
#include <anix/pmm.h>
#include <anix/string.h>

#define ENTRIES_PER_TABLE 512u
#define INTERMEDIATE_FLAGS (VMM_PRESENT | VMM_WRITABLE | VMM_USER)
#define LEAF_ALLOWED (VMM_WRITABLE | VMM_USER | VMM_WRITE_THROUGH | VMM_CACHE_DISABLE | VMM_GLOBAL | VMM_NO_EXECUTE)
#define LARGE_2M_MASK 0x000fffffffe00000ULL
#define LARGE_1G_MASK 0x000fffffc0000000ULL

typedef uint64_t page_table[ENTRIES_PER_TABLE];

static uint16_t level_index(uint64_t address, unsigned level) {
    return (uint16_t)((address >> (12u + level * 9u)) & 0x1ffu);
}

bool vmm_is_canonical(uint64_t address) {
    uint64_t upper = address >> 48;
    uint64_t sign = (address >> 47) & 1u;
    return sign ? upper == 0xffffu : upper == 0;
}

static page_table *table_from_phys(uint64_t physical) {
    return (page_table *)pmm_phys_to_virt(physical & VMM_PHYS_MASK);
}

static uint64_t allocate_table(void) {
    uint64_t physical = pmm_alloc_phys_page();
    if (physical == PMM_INVALID_PHYS) return PMM_INVALID_PHYS;
    page_table *table = table_from_phys(physical);
    if (!table) { pmm_free_phys_page(physical); return PMM_INVALID_PHYS; }
    kmemset(table, 0, VMM_PAGE_SIZE);
    return physical;
}

bool vmm_create(struct vmm_space *space) {
    if (!space) return false;
    uint64_t root = allocate_table();
    if (root == PMM_INVALID_PHYS) return false;
    space->root_phys = root;
    space->owned = true;
    return true;
}

bool vmm_import(struct vmm_space *space, uint64_t root_phys) {
    if (!space || (root_phys & (VMM_PAGE_SIZE - 1u))) return false;
    if (!table_from_phys(root_phys)) return false;
    space->root_phys = root_phys;
    space->owned = false;
    return true;
}

bool vmm_import_active(struct vmm_space *space) {
    return vmm_import(space, vmm_active_root());
}

static bool table_empty(page_table *table) {
    for (size_t i = 0; i < ENTRIES_PER_TABLE; ++i) if ((*table)[i] & VMM_PRESENT) return false;
    return true;
}

static void destroy_level(uint64_t table_phys, unsigned level) {
    page_table *table = table_from_phys(table_phys);
    if (!table) return;
    if (level > 0) {
        for (size_t i = 0; i < ENTRIES_PER_TABLE; ++i) {
            uint64_t entry = (*table)[i];
            if ((entry & VMM_PRESENT) && !(entry & VMM_LARGE_PAGE)) destroy_level(entry & VMM_PHYS_MASK, level - 1u);
        }
    }
    pmm_free_phys_page(table_phys & VMM_PHYS_MASK);
}

void vmm_destroy(struct vmm_space *space) {
    if (!space || space->root_phys == PMM_INVALID_PHYS) return;
    if (!space->owned) { space->root_phys = PMM_INVALID_PHYS; return; }
    destroy_level(space->root_phys, 3);
    space->root_phys = PMM_INVALID_PHYS;
    space->owned = false;
}

bool vmm_map_large(struct vmm_space *space, uint64_t virtual_address, uint64_t physical_address, uint64_t size, uint64_t flags) {
    if (!space || !space->owned || (size != VMM_SIZE_2M && size != VMM_SIZE_1G)) return false;
    if ((virtual_address & (size - 1u)) || (physical_address & (size - 1u)) || !vmm_is_canonical(virtual_address)) return false;
    unsigned target_level = size == VMM_SIZE_1G ? 2u : 1u;
    uint64_t physical_mask = size == VMM_SIZE_1G ? LARGE_1G_MASK : LARGE_2M_MASK;
    page_table *table = table_from_phys(space->root_phys);
    if (!table) return false;
    page_table *parents[2] = {0};
    uint16_t parent_indices[2] = {0};
    uint64_t created_phys[2] = { PMM_INVALID_PHYS, PMM_INVALID_PHYS };
    size_t created_count = 0;
    for (unsigned level = 3; level > target_level; --level) {
        uint16_t index = level_index(virtual_address, level);
        uint64_t entry = (*table)[index];
        if (!(entry & VMM_PRESENT)) {
            uint64_t child = allocate_table();
            if (child == PMM_INVALID_PHYS) goto rollback;
            parents[created_count] = table;
            parent_indices[created_count] = index;
            created_phys[created_count++] = child;
            (*table)[index] = child | INTERMEDIATE_FLAGS;
            entry = (*table)[index];
        }
        if (entry & VMM_LARGE_PAGE) goto rollback;
        table = table_from_phys(entry);
        if (!table) goto rollback;
    }
    uint16_t index = level_index(virtual_address, target_level);
    if ((*table)[index] & VMM_PRESENT) goto rollback;
    (*table)[index] = (physical_address & physical_mask) | VMM_PRESENT | VMM_LARGE_PAGE | (flags & LEAF_ALLOWED);
    return true;
rollback:
    while (created_count > 0) {
        --created_count;
        (*parents[created_count])[parent_indices[created_count]] = 0;
        pmm_free_phys_page(created_phys[created_count]);
    }
    return false;
}

bool vmm_map_page(struct vmm_space *space, uint64_t virtual_address, uint64_t physical_address, uint64_t flags) {
    if (!space || !space->owned || space->root_phys == PMM_INVALID_PHYS || !vmm_is_canonical(virtual_address)) return false;
    if ((virtual_address & (VMM_PAGE_SIZE - 1u)) || (physical_address & (VMM_PAGE_SIZE - 1u))) return false;
    page_table *table = table_from_phys(space->root_phys);
    if (!table) return false;
    page_table *parents[3] = { 0 };
    uint16_t parent_indices[3] = { 0 };
    uint64_t created_phys[3] = { PMM_INVALID_PHYS, PMM_INVALID_PHYS, PMM_INVALID_PHYS };
    size_t created_count = 0;
    for (unsigned level = 3; level > 0; --level) {
        uint16_t index = level_index(virtual_address, level);
        uint64_t entry = (*table)[index];
        if (!(entry & VMM_PRESENT)) {
            uint64_t child = allocate_table();
            if (child == PMM_INVALID_PHYS) goto rollback;
            parents[created_count] = table;
            parent_indices[created_count] = index;
            created_phys[created_count++] = child;
            (*table)[index] = child | INTERMEDIATE_FLAGS;
            entry = (*table)[index];
        }
        if (entry & VMM_LARGE_PAGE) goto rollback;
        table = table_from_phys(entry);
        if (!table) goto rollback;
    }
    uint16_t leaf = level_index(virtual_address, 0);
    if ((*table)[leaf] & VMM_PRESENT) goto rollback;
    (*table)[leaf] = (physical_address & VMM_PHYS_MASK) | VMM_PRESENT | (flags & LEAF_ALLOWED);
    return true;
rollback:
    while (created_count > 0) {
        --created_count;
        (*parents[created_count])[parent_indices[created_count]] = 0;
        pmm_free_phys_page(created_phys[created_count]);
    }
    return false;
}

bool vmm_map_range(struct vmm_space *space, uint64_t virtual_address, uint64_t physical_address, uint64_t page_count, uint64_t flags) {
    if (page_count == 0 || page_count > UINT64_MAX / VMM_PAGE_SIZE) return false;
    uint64_t bytes = page_count * VMM_PAGE_SIZE;
    if (virtual_address > UINT64_MAX - (bytes - 1u) || physical_address > UINT64_MAX - (bytes - 1u)) return false;
    if (!vmm_is_canonical(virtual_address) || !vmm_is_canonical(virtual_address + bytes - 1u)) return false;
    uint64_t mapped = 0;
    for (; mapped < page_count; ++mapped) {
        if (!vmm_map_page(space, virtual_address + mapped * VMM_PAGE_SIZE, physical_address + mapped * VMM_PAGE_SIZE, flags)) break;
    }
    if (mapped == page_count) return true;
    while (mapped > 0) { --mapped; vmm_unmap_page(space, virtual_address + mapped * VMM_PAGE_SIZE, 0); }
    return false;
}

bool vmm_unmap_range(struct vmm_space *space, uint64_t virtual_address, uint64_t page_count) {
    if (page_count == 0 || page_count > UINT64_MAX / VMM_PAGE_SIZE) return false;
    uint64_t bytes = page_count * VMM_PAGE_SIZE;
    if (virtual_address > UINT64_MAX - (bytes - 1u)) return false;
    for (uint64_t i = 0; i < page_count; ++i) if (!vmm_query(space, virtual_address + i * VMM_PAGE_SIZE, 0, 0)) return false;
    for (uint64_t i = 0; i < page_count; ++i) if (!vmm_unmap_page(space, virtual_address + i * VMM_PAGE_SIZE, 0)) return false;
    return true;
}

bool vmm_query(const struct vmm_space *space, uint64_t virtual_address, uint64_t *physical_address, uint64_t *flags) {
    if (!space || space->root_phys == PMM_INVALID_PHYS || !vmm_is_canonical(virtual_address)) return false;
    page_table *table = table_from_phys(space->root_phys);
    if (!table) return false;
    for (unsigned level = 3; level > 0; --level) {
        uint64_t entry = (*table)[level_index(virtual_address, level)];
        if (!(entry & VMM_PRESENT)) return false;
        if ((entry & VMM_LARGE_PAGE) && level <= 2) {
            uint64_t mask = level == 2 ? LARGE_1G_MASK : LARGE_2M_MASK;
            uint64_t offset_mask = level == 2 ? ((1ULL << 30) - 1u) : ((1ULL << 21) - 1u);
            if (physical_address) *physical_address = (entry & mask) | (virtual_address & offset_mask);
            if (flags) *flags = entry & ~mask;
            return true;
        }
        table = table_from_phys(entry);
        if (!table) return false;
    }
    uint64_t entry = (*table)[level_index(virtual_address, 0)];
    if (!(entry & VMM_PRESENT)) return false;
    if (physical_address) *physical_address = (entry & VMM_PHYS_MASK) | (virtual_address & (VMM_PAGE_SIZE - 1u));
    if (flags) *flags = entry & ~VMM_PHYS_MASK;
    return true;
}

bool vmm_protect_page(struct vmm_space *space, uint64_t virtual_address, uint64_t flags) {
    if (!space || !space->owned || (virtual_address & (VMM_PAGE_SIZE - 1u)) || !vmm_is_canonical(virtual_address)) return false;
    page_table *table = table_from_phys(space->root_phys);
    if (!table) return false;
    for (unsigned level = 3; level > 0; --level) {
        uint64_t entry = (*table)[level_index(virtual_address, level)];
        if (!(entry & VMM_PRESENT) || (entry & VMM_LARGE_PAGE)) return false;
        table = table_from_phys(entry);
        if (!table) return false;
    }
    uint16_t index = level_index(virtual_address, 0);
    uint64_t entry = (*table)[index];
    if (!(entry & VMM_PRESENT)) return false;
    (*table)[index] = (entry & VMM_PHYS_MASK) | VMM_PRESENT | (flags & LEAF_ALLOWED);
#if defined(__x86_64__) && !defined(ANIX_HOST_TEST)
    __asm__ volatile("invlpg (%0)" : : "r"((void *)(uintptr_t)virtual_address) : "memory");
#endif
    return true;
}

bool vmm_unmap_page(struct vmm_space *space, uint64_t virtual_address, uint64_t *old_physical_address) {
    if (!space || !space->owned || (virtual_address & (VMM_PAGE_SIZE - 1u)) || !vmm_is_canonical(virtual_address)) return false;
    page_table *tables[4]; uint64_t table_phys[4]; uint16_t indices[4];
    table_phys[3] = space->root_phys; tables[3] = table_from_phys(table_phys[3]);
    if (!tables[3]) return false;
    for (unsigned level = 3; level > 0; --level) {
        indices[level] = level_index(virtual_address, level);
        uint64_t entry = (*tables[level])[indices[level]];
        if (!(entry & VMM_PRESENT) || (entry & VMM_LARGE_PAGE)) return false;
        table_phys[level - 1u] = entry & VMM_PHYS_MASK;
        tables[level - 1u] = table_from_phys(table_phys[level - 1u]);
        if (!tables[level - 1u]) return false;
    }
    indices[0] = level_index(virtual_address, 0);
    uint64_t entry = (*tables[0])[indices[0]];
    if (!(entry & VMM_PRESENT)) return false;
    if (old_physical_address) *old_physical_address = entry & VMM_PHYS_MASK;
    (*tables[0])[indices[0]] = 0;
    for (unsigned level = 0; level < 3; ++level) {
        if (!table_empty(tables[level])) break;
        pmm_free_phys_page(table_phys[level]);
        (*tables[level + 1u])[indices[level + 1u]] = 0;
    }
#if defined(__x86_64__) && !defined(ANIX_HOST_TEST)
    __asm__ volatile("invlpg (%0)" : : "r"((void *)(uintptr_t)virtual_address) : "memory");
#endif
    return true;
}

struct clone_context {
    struct vmm_space *destination;
    uint64_t range_start;
    uint64_t range_end;
    uint64_t mapped_start;
};

static bool clone_mapping(uint64_t virtual_address, uint64_t physical_address, uint64_t size, uint64_t flags, void *opaque) {
    struct clone_context *context = opaque;
    uint64_t start = virtual_address < context->range_start ? context->range_start : virtual_address;
    uint64_t mapping_end = virtual_address + size;
    uint64_t end = mapping_end > context->range_end ? context->range_end : mapping_end;
    start = (start + VMM_PAGE_SIZE - 1u) & ~(uint64_t)(VMM_PAGE_SIZE - 1u);
    end &= ~(uint64_t)(VMM_PAGE_SIZE - 1u);
    if (end <= start) return true;
    uint64_t leaf_flags = flags & LEAF_ALLOWED;
    if (start == virtual_address && end - start == size && size != VMM_SIZE_4K) {
        if (!vmm_map_large(context->destination, start, physical_address, size, leaf_flags)) return false;
    } else {
        uint64_t source_phys = physical_address + (start - virtual_address);
        if (!vmm_map_range(context->destination, start, source_phys, (end - start) / VMM_PAGE_SIZE, leaf_flags)) return false;
    }
    if (start < context->mapped_start) context->mapped_start = start;
    return true;
}

bool vmm_clone_range(const struct vmm_space *source, struct vmm_space *destination, uint64_t virtual_start, uint64_t virtual_end) {
    if (!source || !destination || !destination->owned || virtual_end <= virtual_start) return false;
    if ((virtual_start & (VMM_PAGE_SIZE - 1u)) || (virtual_end & (VMM_PAGE_SIZE - 1u))) return false;
    struct clone_context context = { destination, virtual_start, virtual_end, virtual_end };
    if (vmm_walk(source, virtual_start, virtual_end, clone_mapping, &context)) return true;
    if (context.mapped_start < virtual_end) {
        for (uint64_t address = context.mapped_start; address < virtual_end; address += VMM_PAGE_SIZE) {
            if (vmm_query(destination, address, 0, 0)) vmm_unmap_page(destination, address, 0);
        }
    }
    return false;
}

static bool sync_missing_mapping(uint64_t virtual_address, uint64_t physical_address, uint64_t size,
                                uint64_t flags, void *opaque) {
    struct clone_context *context = opaque;
    uint64_t start = virtual_address < context->range_start ? context->range_start : virtual_address;
    uint64_t end = virtual_address + size;
    if (end > context->range_end) end = context->range_end;
    start = (start + VMM_PAGE_SIZE - 1u) & ~(uint64_t)(VMM_PAGE_SIZE - 1u);
    end &= ~(uint64_t)(VMM_PAGE_SIZE - 1u);
    for (uint64_t address = start; address < end; address += VMM_PAGE_SIZE) {
        if (vmm_query(context->destination, address, 0, 0)) continue;
        uint64_t source_phys = physical_address + (address - virtual_address);
        if (!vmm_map_page(context->destination, address, source_phys, flags & LEAF_ALLOWED)) return false;
    }
    return true;
}

bool vmm_sync_missing_range(const struct vmm_space *source, struct vmm_space *destination,
                            uint64_t virtual_start, uint64_t virtual_end) {
    if (!source || !destination || !destination->owned || virtual_end <= virtual_start ||
        (virtual_start & (VMM_PAGE_SIZE - 1u)) || (virtual_end & (VMM_PAGE_SIZE - 1u))) return false;
    struct clone_context context = { destination, virtual_start, virtual_end, virtual_end };
    return vmm_walk(source, virtual_start, virtual_end, sync_missing_mapping, &context);
}

static uint64_t canonicalize(uint64_t address) {
    return (address & (1ULL << 47)) ? address | 0xffff000000000000ULL : address;
}

static bool walk_level(uint64_t table_phys, unsigned level, uint64_t prefix, uint64_t start, uint64_t end, vmm_walk_callback callback, void *context) {
    page_table *table = table_from_phys(table_phys);
    if (!table) return false;
    unsigned shift = 12u + level * 9u;
    for (uint64_t i = 0; i < ENTRIES_PER_TABLE; ++i) {
        uint64_t entry = (*table)[i];
        if (!(entry & VMM_PRESENT)) continue;
        uint64_t virtual_address = canonicalize(prefix | (i << shift));
        if ((entry & VMM_LARGE_PAGE) && level <= 2) {
            uint64_t size = level == 2 ? VMM_SIZE_1G : VMM_SIZE_2M;
            uint64_t mask = level == 2 ? LARGE_1G_MASK : LARGE_2M_MASK;
            if (virtual_address < end && (virtual_address >= start || size > start - virtual_address) && !callback(virtual_address, entry & mask, size, entry & ~mask, context)) return false;
        } else if (level == 0) {
            if (virtual_address < end && (virtual_address >= start || VMM_SIZE_4K > start - virtual_address) && !callback(virtual_address, entry & VMM_PHYS_MASK, VMM_SIZE_4K, entry & ~VMM_PHYS_MASK, context)) return false;
        } else if (!walk_level(entry & VMM_PHYS_MASK, level - 1u, prefix | (i << shift), start, end, callback, context)) return false;
    }
    return true;
}

bool vmm_walk(const struct vmm_space *space, uint64_t virtual_start, uint64_t virtual_end, vmm_walk_callback callback, void *context) {
    if (!space || !callback || virtual_end <= virtual_start || !vmm_is_canonical(virtual_start) || !vmm_is_canonical(virtual_end - 1u)) return false;
    return walk_level(space->root_phys, 3, 0, virtual_start, virtual_end, callback, context);
}

uint64_t vmm_active_root(void) {
    uint64_t value;
    __asm__ volatile("mov %%cr3, %0" : "=r"(value));
    return value & VMM_PHYS_MASK;
}

void vmm_activate(const struct vmm_space *space) {
    if (!space || space->root_phys == PMM_INVALID_PHYS) return;
    __asm__ volatile("mov %0, %%cr3" : : "r"(space->root_phys) : "memory");
}
