#ifndef ANIX_VMM_H
#define ANIX_VMM_H
#include <anix/types.h>
#define VMM_PAGE_SIZE 4096u
#define VMM_PHYS_MASK 0x000ffffffffff000ULL
#define VMM_PRESENT (1ULL << 0)
#define VMM_WRITABLE (1ULL << 1)
#define VMM_USER (1ULL << 2)
#define VMM_WRITE_THROUGH (1ULL << 3)
#define VMM_CACHE_DISABLE (1ULL << 4)
#define VMM_LARGE_PAGE (1ULL << 7)
#define VMM_GLOBAL (1ULL << 8)
#define VMM_NO_EXECUTE (1ULL << 63)
#define VMM_SIZE_4K (1ULL << 12)
#define VMM_SIZE_2M (1ULL << 21)
#define VMM_SIZE_1G (1ULL << 30)
struct vmm_space { uint64_t root_phys; bool owned; };
typedef bool (*vmm_walk_callback)(uint64_t virtual_address, uint64_t physical_address, uint64_t size, uint64_t flags, void *context);
bool vmm_create(struct vmm_space *space);
bool vmm_import(struct vmm_space *space, uint64_t root_phys);
bool vmm_import_active(struct vmm_space *space);
void vmm_destroy(struct vmm_space *space);
bool vmm_walk(const struct vmm_space *space, uint64_t virtual_start, uint64_t virtual_end, vmm_walk_callback callback, void *context);
bool vmm_map_large(struct vmm_space *space, uint64_t virtual_address, uint64_t physical_address, uint64_t size, uint64_t flags);
bool vmm_clone_range(const struct vmm_space *source, struct vmm_space *destination, uint64_t virtual_start, uint64_t virtual_end);
bool vmm_sync_missing_range(const struct vmm_space *source, struct vmm_space *destination,
                            uint64_t virtual_start, uint64_t virtual_end);
bool vmm_map_page(struct vmm_space *space, uint64_t virtual_address, uint64_t physical_address, uint64_t flags);
bool vmm_map_range(struct vmm_space *space, uint64_t virtual_address, uint64_t physical_address, uint64_t page_count, uint64_t flags);
bool vmm_unmap_range(struct vmm_space *space, uint64_t virtual_address, uint64_t page_count);
bool vmm_protect_page(struct vmm_space *space, uint64_t virtual_address, uint64_t flags);
bool vmm_unmap_page(struct vmm_space *space, uint64_t virtual_address, uint64_t *old_physical_address);
bool vmm_query(const struct vmm_space *space, uint64_t virtual_address, uint64_t *physical_address, uint64_t *flags);
bool vmm_is_canonical(uint64_t virtual_address);
uint64_t vmm_active_root(void);
void vmm_activate(const struct vmm_space *space);
#endif
