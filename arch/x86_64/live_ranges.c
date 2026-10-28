#include <anix/live_ranges.h>
#include <anix/gdt.h>
#include <anix/interrupts.h>
#include <anix/pmm.h>

size_t arch_live_ranges(struct live_range *ranges, size_t capacity) {
    size_t total = 0;
    size_t available = capacity;
    size_t count = gdt_live_ranges(ranges, available);
    total += count;
    if (ranges) { size_t used = count < available ? count : available; ranges += used; available -= used; }
    count = idt_live_ranges(ranges, available);
    total += count;
    if (ranges) { size_t used = count < available ? count : available; ranges += used; available -= used; }
    uint64_t rsp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    uint64_t stack_page = rsp & ~(uint64_t)(PMM_PAGE_SIZE - 1u);
    if (ranges && available) ranges[0] = (struct live_range){ "current-stack-page", stack_page, stack_page + PMM_PAGE_SIZE };
    ++total;
    if (ranges && available) { ++ranges; --available; }
    uint64_t bitmap_start = (uint64_t)pmm_bitmap_virtual_address();
    uint64_t bitmap_end = bitmap_start + pmm_bitmap_page_count() * PMM_PAGE_SIZE;
    if (ranges && available) ranges[0] = (struct live_range){ "pmm-bitmap", bitmap_start, bitmap_end };
    return total + 1u;
}
