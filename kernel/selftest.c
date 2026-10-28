#include <anix/heap.h>
#include <anix/kernel_init.h>
#include <anix/kva.h>
#include <anix/pmm.h>
#include <anix/serial.h>
#include <anix/string.h>

bool kernel_memory_selftest(void) {
    void *virtual_pages = kva_alloc_pages(2, VMM_WRITABLE | VMM_NO_EXECUTE);
    if (!virtual_pages) return false;
    ((volatile uint64_t *)virtual_pages)[0] = 0x4b56414d41505045ULL;
    ((volatile uint64_t *)virtual_pages)[VMM_PAGE_SIZE / sizeof(uint64_t)] = 0x484541504d415050ULL;
    bool virtual_ok = ((volatile uint64_t *)virtual_pages)[0] == 0x4b56414d41505045ULL &&
        ((volatile uint64_t *)virtual_pages)[VMM_PAGE_SIZE / sizeof(uint64_t)] == 0x484541504d415050ULL;
    if (!kva_free_pages(virtual_pages, 2) || !virtual_ok) return false;
    void *guarded_stack = kva_alloc_guarded_pages(2, VMM_WRITABLE | VMM_NO_EXECUTE);
    if (!guarded_stack) return false;
    struct vmm_space active_space;
    if (!vmm_import_active(&active_space)) { (void)kva_free_guarded_pages(guarded_stack, 2); return false; }
    bool guards_ok = !vmm_query(&active_space, (uint64_t)(uintptr_t)guarded_stack - VMM_PAGE_SIZE, 0, 0) &&
        !vmm_query(&active_space, (uint64_t)(uintptr_t)guarded_stack + 2u * VMM_PAGE_SIZE, 0, 0);
    if (!kva_free_guarded_pages(guarded_stack, 2) || !guards_ok) return false;
    void *heap_a = kmalloc(128), *heap_b = kmalloc(8192);
    if (!heap_a || !heap_b || ((uintptr_t)heap_a & 15u) || ((uintptr_t)heap_b & 15u)) {
        if (heap_a) (void)kfree(heap_a);
        if (heap_b) (void)kfree(heap_b);
        return false;
    }
    kmemset(heap_a, 0x5a, 128); kmemset(heap_b, 0xa5, 8192);
    bool heap_ok = ((uint8_t *)heap_a)[127] == 0x5a && ((uint8_t *)heap_b)[8191] == 0xa5;
    bool heap_a_free = kfree(heap_a), heap_b_free = kfree(heap_b);
    if (!heap_ok || !heap_a_free || !heap_b_free) return false;
    uint8_t *guarded = kmalloc(64u);
    if (!guarded) return false;
    kmemset(guarded, 0x41, 64u);
    uint64_t *redzone = (uint64_t *)(guarded + 64u);
    const uint64_t saved_redzone = *redzone;
    *redzone ^= 1u;
    const bool overrun_detected = !kfree(guarded);
    *redzone = saved_redzone;
    uint8_t *grown = krealloc(guarded, 256u);
    const bool realloc_preserved = grown && grown[0] == 0x41 && grown[63] == 0x41;
    const bool free_poisoned = guarded[0] == 0xddu;
    const bool bad_pointer_rejected = !kfree((void *)(uintptr_t)0x1000u) &&
        !krealloc((void *)(uintptr_t)0x1000u, 32u);
    const bool grown_freed = grown && kfree(grown);
    const bool double_free_rejected = grown && !kfree(grown);
    if (!overrun_detected || !realloc_preserved || !free_poisoned || !bad_pointer_rejected ||
        !grown_freed || !double_free_rejected) return false;
    serial_write("[mm] kernel heap redzone, free-poison, invalid-free, realloc, and double-free checks: ok\n");
    serial_write("[mm] KVA, guarded stacks, and kernel heap: ok\n");

    uint64_t page = pmm_alloc_phys_page();
    if (page == PMM_INVALID_PHYS) return false;
    serial_write("[mm] bitmap at "); serial_write_hex(pmm_bitmap_physical_address());
    serial_write(", pages: "); serial_write_dec(pmm_bitmap_page_count());
    serial_write("\n[mm] PMM: allocated page "); serial_write_hex(page);
    serial_write(", HHDM address: "); serial_write_hex((uint64_t)pmm_phys_to_virt(page));
    serial_write(", free pages: "); serial_write_dec(pmm_free_page_count());
    serial_write("/"); serial_write_dec(pmm_total_page_count()); serial_write("\n");
    if (!pmm_free_phys_page(page)) return false;

    struct vmm_space test_space = { .root_phys = PMM_INVALID_PHYS };
    uint64_t test_page = pmm_alloc_phys_page(), queried_page = 0;
    const uint64_t test_virtual = 0xffff900000000000ULL;
    if (test_page == PMM_INVALID_PHYS) return false;
    if (!vmm_create(&test_space)) { (void)pmm_free_phys_page(test_page); return false; }
    bool ok = vmm_map_page(&test_space, test_virtual, test_page, VMM_WRITABLE | VMM_NO_EXECUTE) &&
        vmm_query(&test_space, test_virtual, &queried_page, 0) && queried_page == test_page &&
        vmm_unmap_page(&test_space, test_virtual, &queried_page) && queried_page == test_page;
    vmm_destroy(&test_space);
    if (!pmm_free_phys_page(test_page)) return false;
    if (!ok) return false;
    serial_write("[mm] VMM map/query/unmap: ok; active CR3: "); serial_write_hex(vmm_active_root()); serial_write("\n");
    return true;
}
