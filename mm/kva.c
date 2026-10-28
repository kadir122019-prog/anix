#include <anix/kva.h>
#include <anix/pmm.h>
#include <anix/spinlock.h>
#include <anix/string.h>

#define KVA_MAP_BYTES (KVA_PAGE_COUNT / 8u)
static uint8_t allocation_map[KVA_MAP_BYTES];
static uint8_t owned_map[KVA_MAP_BYTES];
static uint32_t allocation_length[KVA_PAGE_COUNT];
static uint8_t allocation_kind[KVA_PAGE_COUNT];
static struct vmm_space *kernel_space;
#define KVA_MAX_ADDRESS_SPACES 32u
static struct vmm_space *address_spaces[KVA_MAX_ADDRESS_SPACES];
static struct anix_spinlock allocator_lock;
static bool initialized;

static bool slot_used(size_t index) {
    return (allocation_map[index >> 3] & (uint8_t)(1u << (index & 7u))) != 0;
}

static void set_slot(size_t index, bool used) {
    uint8_t mask = (uint8_t)(1u << (index & 7u));
    if (used) allocation_map[index >> 3] |= mask;
    else allocation_map[index >> 3] &= (uint8_t)~mask;
}

static bool slot_owned(size_t index) {
    return (owned_map[index >> 3] & (uint8_t)(1u << (index & 7u))) != 0;
}

static void set_owned(size_t index, bool owned) {
    uint8_t mask = (uint8_t)(1u << (index & 7u));
    if (owned) owned_map[index >> 3] |= mask;
    else owned_map[index >> 3] &= (uint8_t)~mask;
}

static bool find_run(size_t page_count, size_t *first_out) {
    size_t run = 0, first = 0;
    for (size_t i = 0; i < KVA_PAGE_COUNT; ++i) {
        if (!slot_used(i)) {
            if (run == 0) first = i;
            if (++run == page_count) { *first_out = first; return true; }
        } else run = 0;
    }
    return false;
}

static bool sync_registered_range(uint64_t base, size_t page_count) {
    for (size_t i = 0; i < KVA_MAX_ADDRESS_SPACES; ++i) {
        if (address_spaces[i] && !vmm_sync_missing_range(kernel_space, address_spaces[i], base,
                base + page_count * VMM_PAGE_SIZE)) return false;
    }
    return true;
}

static void unmap_registered_range(uint64_t base, size_t page_count) {
    for (size_t i = 0; i < KVA_MAX_ADDRESS_SPACES; ++i) {
        struct vmm_space *space = address_spaces[i];
        if (!space) continue;
        for (size_t page = 0; page < page_count; ++page) {
            uint64_t address = base + page * VMM_PAGE_SIZE;
            if (vmm_query(space, address, 0, 0)) (void)vmm_unmap_page(space, address, 0);
        }
    }
}

bool kva_init(struct vmm_space *space) {
    if (!space || !space->owned || space->root_phys == PMM_INVALID_PHYS ||
        vmm_active_root() != space->root_phys) return false;
    uintptr_t lock_flags = anix_spin_lock_irqsave(&allocator_lock);
    if (initialized) {
        anix_spin_unlock_irqrestore(&allocator_lock, lock_flags);
        return false;
    }
    kmemset(allocation_map, 0, sizeof(allocation_map));
    kmemset(owned_map, 0, sizeof(owned_map));
    kmemset(allocation_length, 0, sizeof(allocation_length));
    kmemset(allocation_kind, 0, sizeof(allocation_kind));
    kernel_space = space;
    kmemset(address_spaces, 0, sizeof(address_spaces));
    initialized = true;
    anix_spin_unlock_irqrestore(&allocator_lock, lock_flags);
    return true;
}

bool kva_register_address_space(struct vmm_space *space) {
    if (!initialized || !space || !space->owned || space == kernel_space) return false;
    uintptr_t lock_flags = anix_spin_lock_irqsave(&allocator_lock);
    for (size_t i = 0; i < KVA_MAX_ADDRESS_SPACES; ++i) {
        if (address_spaces[i] == space) { anix_spin_unlock_irqrestore(&allocator_lock, lock_flags); return true; }
    }
    size_t slot = KVA_MAX_ADDRESS_SPACES;
    for (size_t i = 0; i < KVA_MAX_ADDRESS_SPACES; ++i) if (!address_spaces[i]) { slot = i; break; }
    if (slot == KVA_MAX_ADDRESS_SPACES ||
        !vmm_sync_missing_range(kernel_space, space, KVA_BASE,
            KVA_BASE + KVA_PAGE_COUNT * VMM_PAGE_SIZE)) { anix_spin_unlock_irqrestore(&allocator_lock, lock_flags); return false; }
    address_spaces[slot] = space;
    anix_spin_unlock_irqrestore(&allocator_lock, lock_flags);
    return true;
}

bool kva_refresh_address_space(struct vmm_space *space) {
    if (!initialized || !space || !space->owned || space == kernel_space) return false;
    uintptr_t lock_flags = anix_spin_lock_irqsave(&allocator_lock);
    bool registered = false;
    for (size_t i = 0; i < KVA_MAX_ADDRESS_SPACES; ++i) {
        if (address_spaces[i] == space) { registered = true; break; }
    }
    const bool refreshed = registered && vmm_sync_missing_range(kernel_space, space, KVA_BASE,
        KVA_BASE + KVA_PAGE_COUNT * VMM_PAGE_SIZE);
    anix_spin_unlock_irqrestore(&allocator_lock, lock_flags);
    return refreshed;
}

void kva_unregister_address_space(struct vmm_space *space) {
    if (!space || !initialized) return;
    uintptr_t lock_flags = anix_spin_lock_irqsave(&allocator_lock);
    for (size_t i = 0; i < KVA_MAX_ADDRESS_SPACES; ++i) if (address_spaces[i] == space) {
        address_spaces[i] = 0;
        break;
    }
    anix_spin_unlock_irqrestore(&allocator_lock, lock_flags);
}

void *kva_alloc_pages(size_t page_count, uint64_t flags) {
    if (!initialized || page_count == 0 || page_count > KVA_PAGE_COUNT) return 0;
    uintptr_t lock_flags = anix_spin_lock_irqsave(&allocator_lock);
    size_t first;
    if (!find_run(page_count, &first)) {
        anix_spin_unlock_irqrestore(&allocator_lock, lock_flags);
        return 0;
    }

    const uint64_t virtual_base = KVA_BASE + first * VMM_PAGE_SIZE;
    size_t mapped = 0;
    for (; mapped < page_count; ++mapped) {
        uint64_t physical = pmm_alloc_phys_page();
        if (physical == PMM_INVALID_PHYS) break;
        void *alias = pmm_phys_to_virt(physical);
        if (!alias) {
            pmm_free_phys_page(physical);
            break;
        }
        kmemset(alias, 0, VMM_PAGE_SIZE);
        if (!vmm_map_page(kernel_space, virtual_base + mapped * VMM_PAGE_SIZE, physical, flags)) {
            pmm_free_phys_page(physical);
            break;
        }
    }
    if (mapped != page_count) {
        while (mapped > 0) {
            --mapped;
            uint64_t physical;
            if (vmm_unmap_page(kernel_space, virtual_base + mapped * VMM_PAGE_SIZE, &physical))
                pmm_free_phys_page(physical);
        }
        anix_spin_unlock_irqrestore(&allocator_lock, lock_flags);
        return 0;
    }
    for (size_t i = 0; i < page_count; ++i) {
        set_slot(first + i, true);
        set_owned(first + i, true);
    }
    allocation_length[first] = (uint32_t)page_count;
    allocation_kind[first] = 1u;
    if (!sync_registered_range(virtual_base, page_count)) {
        unmap_registered_range(virtual_base, page_count);
        for (size_t i = 0; i < page_count; ++i) {
            uint64_t physical;
            if (vmm_unmap_page(kernel_space, virtual_base + i * VMM_PAGE_SIZE, &physical)) (void)pmm_free_phys_page(physical);
            set_slot(first + i, false); set_owned(first + i, false);
        }
        allocation_length[first] = 0; allocation_kind[first] = 0;
        anix_spin_unlock_irqrestore(&allocator_lock, lock_flags);
        return 0;
    }
    anix_spin_unlock_irqrestore(&allocator_lock, lock_flags);
    return (void *)(uintptr_t)virtual_base;
}

bool kva_range_is_mapped(const void *base, size_t length) {
    const uint64_t address = (uint64_t)(uintptr_t)base;
    const uint64_t limit = KVA_BASE + (uint64_t)KVA_PAGE_COUNT * VMM_PAGE_SIZE;
    if (!initialized || !base || !length || address < KVA_BASE || address >= limit ||
        length > limit - address) return false;
    const size_t first = (size_t)((address - KVA_BASE) / VMM_PAGE_SIZE);
    const size_t last = (size_t)((address + length - 1u - KVA_BASE) / VMM_PAGE_SIZE);
    uintptr_t lock_flags = anix_spin_lock_irqsave(&allocator_lock);
    bool mapped = true;
    for (size_t page = first; page <= last; ++page) {
        if (!slot_used(page)) { mapped = false; break; }
    }
    anix_spin_unlock_irqrestore(&allocator_lock, lock_flags);
    return mapped;
}

void *kva_alloc_guarded_pages(size_t page_count, uint64_t flags) {
    if (!initialized || page_count == 0 || page_count > KVA_PAGE_COUNT - 2u) return 0;
    uintptr_t lock_flags = anix_spin_lock_irqsave(&allocator_lock);
    size_t first;
    const size_t reserved_count = page_count + 2u;
    if (!find_run(reserved_count, &first)) { anix_spin_unlock_irqrestore(&allocator_lock, lock_flags); return 0; }
    const uint64_t virtual_base = KVA_BASE + (first + 1u) * VMM_PAGE_SIZE;
    size_t mapped = 0;
    for (; mapped < page_count; ++mapped) {
        const uint64_t physical = pmm_alloc_phys_page();
        if (physical == PMM_INVALID_PHYS) break;
        void *alias = pmm_phys_to_virt(physical);
        if (!alias) { pmm_free_phys_page(physical); break; }
        kmemset(alias, 0, VMM_PAGE_SIZE);
        if (!vmm_map_page(kernel_space, virtual_base + mapped * VMM_PAGE_SIZE, physical, flags)) {
            pmm_free_phys_page(physical);
            break;
        }
    }
    if (mapped != page_count) {
        while (mapped > 0) {
            uint64_t physical;
            --mapped;
            if (vmm_unmap_page(kernel_space, virtual_base + mapped * VMM_PAGE_SIZE, &physical))
                (void)pmm_free_phys_page(physical);
        }
        anix_spin_unlock_irqrestore(&allocator_lock, lock_flags);
        return 0;
    }
    for (size_t i = 0; i < reserved_count; ++i) set_slot(first + i, true);
    for (size_t i = 0; i < page_count; ++i) set_owned(first + i + 1u, true);
    allocation_length[first] = (uint32_t)page_count;
    allocation_kind[first] = 3u;
    if (!sync_registered_range(virtual_base, page_count)) {
        unmap_registered_range(virtual_base, page_count);
        for (size_t i = 0; i < page_count; ++i) {
            uint64_t physical;
            if (vmm_unmap_page(kernel_space, virtual_base + i * VMM_PAGE_SIZE, &physical)) (void)pmm_free_phys_page(physical);
        }
        for (size_t i = 0; i < reserved_count; ++i) set_slot(first + i, false);
        for (size_t i = 0; i < page_count; ++i) set_owned(first + i + 1u, false);
        allocation_length[first] = 0; allocation_kind[first] = 0;
        anix_spin_unlock_irqrestore(&allocator_lock, lock_flags);
        return 0;
    }
    anix_spin_unlock_irqrestore(&allocator_lock, lock_flags);
    return (void *)(uintptr_t)virtual_base;
}

void *kva_map_physical(uint64_t physical_base, size_t page_count, uint64_t flags) {
    if (!initialized || page_count == 0 || page_count > KVA_PAGE_COUNT ||
        (physical_base & (VMM_PAGE_SIZE - 1u)) ||
        page_count > (UINT64_MAX - physical_base) / VMM_PAGE_SIZE) return 0;
    uintptr_t lock_flags = anix_spin_lock_irqsave(&allocator_lock);
    size_t first;
    if (!find_run(page_count, &first)) {
        anix_spin_unlock_irqrestore(&allocator_lock, lock_flags);
        return 0;
    }
    const uint64_t virtual_base = KVA_BASE + first * VMM_PAGE_SIZE;
    if (!vmm_map_range(kernel_space, virtual_base, physical_base, page_count, flags)) {
        anix_spin_unlock_irqrestore(&allocator_lock, lock_flags);
        return 0;
    }
    for (size_t i = 0; i < page_count; ++i) {
        set_slot(first + i, true);
        set_owned(first + i, false);
    }
    allocation_length[first] = (uint32_t)page_count;
    allocation_kind[first] = 2u;
    if (!sync_registered_range(virtual_base, page_count)) {
        unmap_registered_range(virtual_base, page_count);
        (void)vmm_unmap_range(kernel_space, virtual_base, page_count);
        for (size_t i = 0; i < page_count; ++i) { set_slot(first + i, false); set_owned(first + i, false); }
        allocation_length[first] = 0; allocation_kind[first] = 0;
        anix_spin_unlock_irqrestore(&allocator_lock, lock_flags);
        return 0;
    }
    anix_spin_unlock_irqrestore(&allocator_lock, lock_flags);
    return (void *)(uintptr_t)virtual_base;
}

bool kva_free_pages(void *base, size_t page_count) {
    const uint64_t address = (uint64_t)(uintptr_t)base;
    if (!initialized || !base || page_count == 0 ||
        (address & (VMM_PAGE_SIZE - 1u)) || address < KVA_BASE) return false;
    const uint64_t first64 = (address - KVA_BASE) / VMM_PAGE_SIZE;
    if (first64 >= KVA_PAGE_COUNT || page_count > KVA_PAGE_COUNT - first64) return false;
    const size_t first = (size_t)first64;

    uintptr_t lock_flags = anix_spin_lock_irqsave(&allocator_lock);
    if (allocation_length[first] != page_count || allocation_kind[first] == 0u || allocation_kind[first] == 3u) {
        anix_spin_unlock_irqrestore(&allocator_lock, lock_flags);
        return false;
    }
    for (size_t i = 0; i < page_count; ++i) {
        if (!slot_used(first + i) || !vmm_query(kernel_space, address + i * VMM_PAGE_SIZE, 0, 0)) {
            anix_spin_unlock_irqrestore(&allocator_lock, lock_flags);
            return false;
        }
    }
    unmap_registered_range(address, page_count);
    for (size_t i = 0; i < page_count; ++i) {
        uint64_t physical;
        const bool owned = slot_owned(first + i);
        if (!vmm_unmap_page(kernel_space, address + i * VMM_PAGE_SIZE, &physical) ||
            (owned && !pmm_free_phys_page(physical))) {
            anix_spin_unlock_irqrestore(&allocator_lock, lock_flags);
            return false;
        }
        set_slot(first + i, false);
        set_owned(first + i, false);
    }
    allocation_length[first] = 0;
    allocation_kind[first] = 0;
    anix_spin_unlock_irqrestore(&allocator_lock, lock_flags);
    return true;
}

bool kva_free_guarded_pages(void *base, size_t page_count) {
    const uint64_t address = (uint64_t)(uintptr_t)base;
    if (!initialized || !base || page_count == 0 || (address & (VMM_PAGE_SIZE - 1u)) ||
        address < KVA_BASE + VMM_PAGE_SIZE) return false;
    const uint64_t mapped_index = (address - KVA_BASE) / VMM_PAGE_SIZE;
    if (mapped_index == 0 || mapped_index >= KVA_PAGE_COUNT || page_count > KVA_PAGE_COUNT - mapped_index - 1u) return false;
    const size_t first = (size_t)mapped_index - 1u;
    uintptr_t lock_flags = anix_spin_lock_irqsave(&allocator_lock);
    if (allocation_kind[first] != 3u || allocation_length[first] != page_count ||
        !slot_used(first) || !slot_used(first + page_count + 1u) ||
        vmm_query(kernel_space, KVA_BASE + first * VMM_PAGE_SIZE, 0, 0) ||
        vmm_query(kernel_space, KVA_BASE + (first + page_count + 1u) * VMM_PAGE_SIZE, 0, 0)) {
        anix_spin_unlock_irqrestore(&allocator_lock, lock_flags);
        return false;
    }
    for (size_t i = 0; i < page_count; ++i) {
        if (!slot_used(first + i + 1u) || !slot_owned(first + i + 1u) ||
            !vmm_query(kernel_space, address + i * VMM_PAGE_SIZE, 0, 0)) {
            anix_spin_unlock_irqrestore(&allocator_lock, lock_flags);
            return false;
        }
    }
    unmap_registered_range(address, page_count);
    for (size_t i = 0; i < page_count; ++i) {
        uint64_t physical;
        if (!vmm_unmap_page(kernel_space, address + i * VMM_PAGE_SIZE, &physical) || !pmm_free_phys_page(physical)) {
            anix_spin_unlock_irqrestore(&allocator_lock, lock_flags);
            return false;
        }
        set_owned(first + i + 1u, false);
    }
    for (size_t i = 0; i < page_count + 2u; ++i) set_slot(first + i, false);
    allocation_length[first] = 0;
    allocation_kind[first] = 0;
    anix_spin_unlock_irqrestore(&allocator_lock, lock_flags);
    return true;
}

uint64_t kva_free_page_count(void) {
    if (!initialized) return 0;
    uintptr_t lock_flags = anix_spin_lock_irqsave(&allocator_lock);
    uint64_t count = 0;
    for (size_t i = 0; i < KVA_PAGE_COUNT; ++i) if (!slot_used(i)) ++count;
    anix_spin_unlock_irqrestore(&allocator_lock, lock_flags);
    return count;
}
