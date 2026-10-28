#ifndef ANIX_PMM_H
#define ANIX_PMM_H
#include <anix/types.h>
#define PMM_PAGE_SIZE 4096u
#define PMM_INVALID_PHYS UINT64_MAX
struct limine_memmap_response;
struct pmm_region { uint64_t start, end; };
bool pmm_init(const struct limine_memmap_response *map, uint64_t hhdm_offset);
uint64_t pmm_alloc_phys_page(void);
bool pmm_free_phys_page(uint64_t physical_address);
bool pmm_reserve_range(uint64_t physical_address, uint64_t length);
bool pmm_release_range(uint64_t physical_address, uint64_t length);
void *pmm_phys_to_virt(uint64_t physical_address);
uint64_t pmm_free_page_count(void);
uint64_t pmm_total_page_count(void);
uint64_t pmm_bitmap_physical_address(void);
uint64_t pmm_bitmap_page_count(void);
void *pmm_bitmap_virtual_address(void);
uint64_t pmm_hhdm_offset(void);
size_t pmm_usable_regions(struct pmm_region *regions, size_t capacity);
#endif
