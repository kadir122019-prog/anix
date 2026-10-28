#ifndef ANIX_KVA_H
#define ANIX_KVA_H
#include <anix/types.h>
#include <anix/vmm.h>

#define KVA_BASE 0xffffc00000000000ULL
#define KVA_PAGE_COUNT 131072u

bool kva_init(struct vmm_space *kernel_space);
bool kva_register_address_space(struct vmm_space *space);
bool kva_refresh_address_space(struct vmm_space *space);
void kva_unregister_address_space(struct vmm_space *space);
void *kva_alloc_pages(size_t page_count, uint64_t flags);
bool kva_range_is_mapped(const void *base, size_t length);
void *kva_alloc_guarded_pages(size_t page_count, uint64_t flags);
void *kva_map_physical(uint64_t physical_base, size_t page_count, uint64_t flags);
bool kva_free_pages(void *base, size_t page_count);
bool kva_free_guarded_pages(void *base, size_t page_count);
uint64_t kva_free_page_count(void);

#endif
