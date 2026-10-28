#include <anix/pmm.h>
#include <anix/vmm.h>
#include <limine.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static uint8_t physical_memory[0x200000] __attribute__((aligned(PMM_PAGE_SIZE)));

struct walk_state { size_t count; bool saw_large; };
static bool count_mapping(uint64_t virtual_address, uint64_t physical_address, uint64_t size, uint64_t flags, void *context) {
    (void)virtual_address; (void)physical_address; (void)flags;
    struct walk_state *state = context;
    ++state->count;
    if (size == VMM_SIZE_1G) state->saw_large = true;
    return true;
}

int main(void) {
    struct limine_memmap_entry usable = { .base = 0x1000, .length = sizeof(physical_memory) - 0x1000, .type = LIMINE_MEMMAP_USABLE };
    struct limine_memmap_entry *entries[] = { &usable };
    struct limine_memmap_response map = { .entry_count = 1, .entries = entries };
    assert(pmm_init(&map, (uint64_t)(uintptr_t)physical_memory));
    uint64_t initial_free = pmm_free_page_count();

    assert(vmm_is_canonical(0));
    assert(vmm_is_canonical(0x00007fffffffffffULL));
    assert(vmm_is_canonical(0xffff800000000000ULL));
    assert(!vmm_is_canonical(0x0000800000000000ULL));
    assert(!vmm_is_canonical(0xffff7fffffffffffULL));

    struct vmm_space space = { .root_phys = PMM_INVALID_PHYS };
    assert(vmm_create(&space));
    assert(pmm_free_page_count() == initial_free - 1);

    uint64_t virtual_address = 0xffff900000123000ULL;
    uint64_t physical_address = 0x180000;
    assert(vmm_map_page(&space, virtual_address, physical_address, VMM_WRITABLE | VMM_NO_EXECUTE));
    assert(!vmm_map_page(&space, virtual_address, physical_address, VMM_WRITABLE));
    assert(!vmm_map_page(&space, virtual_address + 1, physical_address, 0));

    uint64_t queried_phys = 0, queried_flags = 0;
    assert(vmm_query(&space, virtual_address + 0x321, &queried_phys, &queried_flags));
    assert(queried_phys == physical_address + 0x321);
    assert((queried_flags & (VMM_PRESENT | VMM_WRITABLE | VMM_NO_EXECUTE)) == (VMM_PRESENT | VMM_WRITABLE | VMM_NO_EXECUTE));
    assert(vmm_protect_page(&space, virtual_address, VMM_NO_EXECUTE));
    assert(vmm_query(&space, virtual_address, &queried_phys, &queried_flags));
    assert((queried_flags & VMM_WRITABLE) == 0);
    assert((queried_flags & VMM_NO_EXECUTE) != 0);

    uint64_t adjacent = virtual_address + VMM_PAGE_SIZE;
    assert(vmm_map_page(&space, adjacent, physical_address + VMM_PAGE_SIZE, VMM_USER));
    uint64_t old = 0;
    assert(vmm_unmap_page(&space, virtual_address, &old));
    assert(old == physical_address);
    assert(!vmm_query(&space, virtual_address, 0, 0));
    assert(vmm_query(&space, adjacent, 0, 0));
    assert(!vmm_unmap_page(&space, virtual_address, 0));
    assert(vmm_unmap_page(&space, adjacent, &old));
    assert(old == physical_address + VMM_PAGE_SIZE);

    const uint64_t range_virtual = 0xffffa00000000000ULL;
    assert(vmm_map_range(&space, range_virtual, 0x100000, 4, VMM_WRITABLE));
    for (uint64_t i = 0; i < 4; ++i) {
        assert(vmm_query(&space, range_virtual + i * VMM_PAGE_SIZE, &queried_phys, 0));
        assert(queried_phys == 0x100000 + i * VMM_PAGE_SIZE);
    }
    assert(!vmm_map_range(&space, range_virtual - VMM_PAGE_SIZE, 0x110000, 3, VMM_WRITABLE));
    assert(!vmm_query(&space, range_virtual - VMM_PAGE_SIZE, 0, 0));
    assert(vmm_unmap_range(&space, range_virtual, 4));
    assert(!vmm_unmap_range(&space, range_virtual, 4));

    const uint64_t huge_base = 0xffffb00000000000ULL;
    const uint64_t huge_virtual = huge_base + 0x12345678ULL;
    assert(vmm_map_large(&space, huge_base, 0x40000000ULL, VMM_SIZE_1G, VMM_WRITABLE | VMM_NO_EXECUTE));
    assert(!vmm_map_large(&space, huge_base + VMM_PAGE_SIZE, 0x40000000ULL, VMM_SIZE_1G, 0));
    assert(vmm_query(&space, huge_virtual, &queried_phys, &queried_flags));
    assert(queried_phys == 0x40000000ULL + (huge_virtual & (VMM_SIZE_1G - 1u)));
    assert((queried_flags & (VMM_LARGE_PAGE | VMM_NO_EXECUTE)) == (VMM_LARGE_PAGE | VMM_NO_EXECUTE));
    assert(!vmm_protect_page(&space, huge_virtual & ~(VMM_PAGE_SIZE - 1u), 0));
    assert(!vmm_unmap_page(&space, huge_virtual & ~(VMM_PAGE_SIZE - 1u), 0));
    struct walk_state walk = {0};
    assert(vmm_walk(&space, huge_base, huge_base + VMM_SIZE_1G, count_mapping, &walk));
    assert(walk.count == 1 && walk.saw_large);

    struct vmm_space clone = { .root_phys = PMM_INVALID_PHYS };
    assert(vmm_create(&clone));
    assert(vmm_clone_range(&space, &clone, huge_base, huge_base + VMM_SIZE_1G));
    assert(vmm_query(&clone, huge_virtual, &queried_phys, &queried_flags));
    assert(queried_phys == 0x40000000ULL + (huge_virtual & (VMM_SIZE_1G - 1u)));
    assert((queried_flags & VMM_LARGE_PAGE) != 0);
    vmm_destroy(&clone);

    struct vmm_space partial = { .root_phys = PMM_INVALID_PHYS };
    assert(vmm_create(&partial));
    uint64_t partial_start = huge_base + VMM_SIZE_2M;
    uint64_t partial_end = partial_start + 2 * VMM_PAGE_SIZE;
    assert(vmm_clone_range(&space, &partial, partial_start, partial_end));
    assert(vmm_query(&partial, partial_start + 0x123, &queried_phys, &queried_flags));
    assert(queried_phys == 0x40000000ULL + VMM_SIZE_2M + 0x123);
    assert((queried_flags & VMM_LARGE_PAGE) == 0);
    vmm_destroy(&partial);

    struct vmm_space borrowed;
    assert(vmm_import(&borrowed, space.root_phys));
    assert(!borrowed.owned);
    assert(vmm_query(&borrowed, huge_virtual, &queried_phys, 0));
    assert(!vmm_map_page(&borrowed, 0x1000, 0x1000, 0));
    uint64_t free_before_borrowed_destroy = pmm_free_page_count();
    vmm_destroy(&borrowed);
    assert(pmm_free_page_count() == free_before_borrowed_destroy);

    vmm_destroy(&space);
    assert(space.root_phys == PMM_INVALID_PHYS);
    assert(pmm_free_page_count() == initial_free);
    puts("[pass] four-level VMM mapping, querying, unmapping, and table reclamation");
    return 0;
}
