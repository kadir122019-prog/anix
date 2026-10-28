#include <anix/pmm.h>
#include <limine.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static uint8_t physical_memory[0x10000] __attribute__((aligned(PMM_PAGE_SIZE)));

int main(void) {
    struct limine_memmap_entry reserved = { .base = 0, .length = 0x1000, .type = 1 };
    struct limine_memmap_entry first = { .base = 0x1234, .length = 0x3dcc, .type = LIMINE_MEMMAP_USABLE };
    struct limine_memmap_entry overflow = { .base = UINT64_MAX - 0xff, .length = 0x1000, .type = LIMINE_MEMMAP_USABLE };
    struct limine_memmap_entry second = { .base = 0x9000, .length = 0x2000, .type = LIMINE_MEMMAP_USABLE };
    struct limine_memmap_entry *entries[] = { &reserved, &first, &overflow, &second };
    struct limine_memmap_response map = { .entry_count = 4, .entries = entries };
    uint64_t hhdm = (uint64_t)(uintptr_t)physical_memory;

    assert(pmm_init(&map, hhdm));
    assert(pmm_total_page_count() == 5);
    assert(pmm_bitmap_physical_address() == 0x2000);
    assert(pmm_bitmap_page_count() == 1);
    assert(pmm_free_page_count() == 4);
    assert(pmm_reserve_range(0x3001, 0x1fff));
    assert(pmm_free_page_count() == 2);
    assert(!pmm_reserve_range(0x3000, PMM_PAGE_SIZE));
    assert(pmm_release_range(0x3001, 0x1fff));
    assert(pmm_free_page_count() == 4);
    assert(!pmm_release_range(0x3000, PMM_PAGE_SIZE));
    assert(!pmm_reserve_range(UINT64_MAX - 0x100, 0x200));
    assert(pmm_phys_to_virt(0x9000) == &physical_memory[0x9000]);
    assert(pmm_hhdm_offset() == hhdm);
    struct pmm_region regions[4];
    assert(pmm_usable_regions(regions, 4) == 2);
    assert(regions[0].start == 0x2000 && regions[0].end == 0x5000);
    assert(regions[1].start == 0x9000 && regions[1].end == 0xb000);

    uint64_t a = pmm_alloc_phys_page();
    uint64_t b = pmm_alloc_phys_page();
    uint64_t c = pmm_alloc_phys_page();
    uint64_t d = pmm_alloc_phys_page();
    assert(a == 0x3000 && b == 0x4000 && c == 0x9000 && d == 0xa000);
    assert(pmm_alloc_phys_page() == PMM_INVALID_PHYS);
    assert(pmm_free_page_count() == 0);

    assert(!pmm_free_phys_page(0));
    assert(!pmm_free_phys_page(0x2000));
    assert(!pmm_free_phys_page(a + 1));
    assert(pmm_free_phys_page(b));
    assert(!pmm_free_phys_page(b));
    assert(pmm_alloc_phys_page() == b);
    assert(pmm_free_page_count() == 0);
    puts("[pass] bitmap PMM allocation, freeing, reservations, and overflow rejection");
    return 0;
}
