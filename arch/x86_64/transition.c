#include <anix/transition.h>
#include <anix/pmm.h>
#include <anix/string.h>
#include <anix/live_ranges.h>
#include <anix/serial.h>

struct descriptor_register { uint16_t limit; uint64_t base; } __attribute__((packed));

_Static_assert(__builtin_offsetof(struct transition_context, old_cr3) == 0, "transition context assembly offset");
_Static_assert(__builtin_offsetof(struct transition_context, old_rsp) == 8, "transition context assembly offset");
_Static_assert(__builtin_offsetof(struct transition_context, original_rflags) == 16, "transition context assembly offset");
_Static_assert(__builtin_offsetof(struct transition_context, candidate_cr3) == 24, "transition context assembly offset");
_Static_assert(__builtin_offsetof(struct transition_context, stack_base) == 32, "transition context assembly offset");
_Static_assert(__builtin_offsetof(struct transition_context, stack_top) == 40, "transition context assembly offset");
_Static_assert(__builtin_offsetof(struct transition_context, test_result) == 48, "transition context assembly offset");

bool transition_stack_prepare(struct transition_stack *stack, struct vmm_space *candidate) {
    if (!stack || !candidate || !candidate->owned || stack->prepared) return false;
    *stack = (struct transition_stack){ .virtual_base = TRANSITION_STACK_VIRTUAL_BASE };
    for (size_t i = 0; i < TRANSITION_STACK_PAGES; ++i) stack->physical[i] = PMM_INVALID_PHYS;
    for (size_t i = 0; i < TRANSITION_STACK_PAGES; ++i) {
        uint64_t physical = pmm_alloc_phys_page();
        if (physical == PMM_INVALID_PHYS) goto rollback;
        stack->physical[i] = physical;
        void *page = pmm_phys_to_virt(physical);
        if (!page) goto rollback;
        kmemset(page, 0, VMM_PAGE_SIZE);
        uint64_t virtual_address = stack->virtual_base + i * VMM_PAGE_SIZE;
        if (!vmm_map_page(candidate, virtual_address, physical, VMM_WRITABLE | VMM_NO_EXECUTE)) goto rollback;
    }
    stack->stack_top = stack->virtual_base + TRANSITION_STACK_PAGES * VMM_PAGE_SIZE;
    stack->prepared = true;
    return true;
rollback:
    for (size_t i = 0; i < TRANSITION_STACK_PAGES; ++i) {
        uint64_t virtual_address = stack->virtual_base + i * VMM_PAGE_SIZE;
        if (vmm_query(candidate, virtual_address, 0, 0)) vmm_unmap_page(candidate, virtual_address, 0);
        if (stack->physical[i] != PMM_INVALID_PHYS) pmm_free_phys_page(stack->physical[i]);
        stack->physical[i] = PMM_INVALID_PHYS;
    }
    stack->stack_top = 0;
    return false;
}

bool transition_stack_validate(const struct transition_stack *stack, const struct vmm_space *candidate) {
    if (!stack || !candidate || !stack->prepared) return false;
    if (vmm_query(candidate, stack->virtual_base - VMM_PAGE_SIZE, 0, 0)) return false;
    if (vmm_query(candidate, stack->stack_top, 0, 0)) return false;
    for (size_t i = 0; i < TRANSITION_STACK_PAGES; ++i) {
        uint64_t physical, flags;
        if (!vmm_query(candidate, stack->virtual_base + i * VMM_PAGE_SIZE, &physical, &flags)) return false;
        if (physical != stack->physical[i] || !(flags & VMM_WRITABLE) || !(flags & VMM_NO_EXECUTE)) return false;
    }
    return true;
}

void transition_stack_release(struct transition_stack *stack, struct vmm_space *candidate) {
    if (!stack) return;
    for (size_t i = 0; i < TRANSITION_STACK_PAGES; ++i) {
        uint64_t virtual_address = stack->virtual_base + i * VMM_PAGE_SIZE;
        if (candidate && candidate->owned && vmm_query(candidate, virtual_address, 0, 0)) vmm_unmap_page(candidate, virtual_address, 0);
        if (stack->physical[i] != PMM_INVALID_PHYS) pmm_free_phys_page(stack->physical[i]);
        stack->physical[i] = PMM_INVALID_PHYS;
    }
    stack->prepared = false;
    stack->stack_top = 0;
}

static bool range_contains(const struct live_range *range, uint64_t address, uint64_t length) {
    return range && address >= range->start && length <= range->end - address && address < range->end;
}

static bool range_is_accessible(const struct vmm_space *candidate, const struct live_range *range) {
    if (!candidate || !range || range->end <= range->start) return false;
    uint64_t address = range->start & ~(uint64_t)(VMM_PAGE_SIZE - 1u);
    uint64_t end = (range->end + VMM_PAGE_SIZE - 1u) & ~(uint64_t)(VMM_PAGE_SIZE - 1u);
    for (; address < end; address += VMM_PAGE_SIZE) {
        if (!vmm_query(candidate, address, 0, 0)) return false;
    }
    volatile const uint8_t *bytes = (volatile const uint8_t *)(uintptr_t)range->start;
    volatile uint8_t first = bytes[0];
    volatile uint8_t last = bytes[range->end - range->start - 1u];
    (void)first;
    (void)last;
    return true;
}

static bool verify_descriptor_tables(const struct vmm_space *candidate) {
    struct live_range ranges[8];
    const size_t count = arch_live_ranges(ranges, sizeof(ranges) / sizeof(ranges[0]));
    if (count > sizeof(ranges) / sizeof(ranges[0])) return false;
    const struct live_range *gdt_range = 0, *idt_range = 0, *tss_range = 0;
    for (size_t i = 0; i < count; ++i) {
        if (kmemcmp(ranges[i].name, "gdt", sizeof("gdt")) == 0) gdt_range = &ranges[i];
        if (kmemcmp(ranges[i].name, "idt", sizeof("idt")) == 0) idt_range = &ranges[i];
        if (kmemcmp(ranges[i].name, "tss", sizeof("tss")) == 0) tss_range = &ranges[i];
    }
    if (!range_is_accessible(candidate, gdt_range) || !range_is_accessible(candidate, idt_range) ||
        !range_is_accessible(candidate, tss_range)) return false;

    struct descriptor_register gdtr, idtr;
    uint16_t tss_selector;
    __asm__ volatile("sgdt %0" : "=m"(gdtr));
    __asm__ volatile("sidt %0" : "=m"(idtr));
    __asm__ volatile("str %0" : "=rm"(tss_selector));
    if (!range_contains(gdt_range, gdtr.base, (uint64_t)gdtr.limit + 1u) ||
        !range_contains(idt_range, idtr.base, (uint64_t)idtr.limit + 1u) ||
        (tss_selector & 0x4u) != 0) return false;

    const uint64_t descriptor_offset = tss_selector & 0xfff8u;
    if (descriptor_offset > gdtr.limit || gdtr.limit - descriptor_offset < 15u) return false;
    volatile const uint64_t *tss_descriptor = (volatile const uint64_t *)(uintptr_t)(gdtr.base + descriptor_offset);
    const uint64_t low = tss_descriptor[0];
    const uint64_t high = tss_descriptor[1];
    const uint64_t tss_base = ((low >> 16) & 0xffffu) | ((low >> 32) & 0xffu) << 16 |
        ((low >> 56) & 0xffu) << 24 | (high << 32);
    return range_contains(tss_range, tss_base, 1u);
}

static uint64_t transition_post_switch(struct transition_context *context) {
    uint64_t result = 0;
    struct vmm_space candidate = { .root_phys = context->candidate_cr3, .owned = false };
    uint64_t active_cr3 = vmm_active_root();
    if (active_cr3 == context->candidate_cr3) result |= TRANSITION_TEST_CR3;

    uint64_t rsp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    if (rsp >= context->stack_base && rsp < context->stack_top) result |= TRANSITION_TEST_STACK;

    const uint64_t physical = pmm_alloc_phys_page();
    if (physical != PMM_INVALID_PHYS) {
        volatile uint64_t *alias = pmm_phys_to_virt(physical);
        bool hhdm_ok = false;
        if (alias) {
            const uint64_t pattern = 0xa11ce55a5eedc0deULL;
            *alias = pattern;
            hhdm_ok = *alias == pattern;
            if (hhdm_ok) result |= TRANSITION_TEST_HHDM;
        }
        const bool freed = pmm_free_phys_page(physical);
        if (freed && alias && hhdm_ok) result |= TRANSITION_TEST_PMM;
    }

    if (verify_descriptor_tables(&candidate)) result |= TRANSITION_TEST_TABLES;
    if (result == TRANSITION_TEST_ALL) serial_write("ANIX_TRANSITION_POST_SWITCH_OK\n");
    else {
        serial_write("[transition] candidate checks failed; result mask: ");
        serial_write_hex(result);
        serial_write("\n");
    }
    return result;
}

uint64_t transition_run_roundtrip(struct transition_context *context, const struct vmm_space *candidate,
    const struct transition_stack *stack) {
    if (!context || !candidate || !candidate->owned || !stack || !stack->prepared ||
        !transition_stack_validate(stack, candidate)) return 0;
    *context = (struct transition_context){
        .candidate_cr3 = candidate->root_phys,
        .stack_base = stack->virtual_base,
        .stack_top = stack->stack_top
    };
    const uint64_t result = arch_transition_roundtrip(context, transition_post_switch);
    if (result == TRANSITION_TEST_ALL && context->test_result == TRANSITION_TEST_ALL) {
        serial_write("ANIX_TRANSITION_ROUNDTRIP_OK\n");
    }
    return result;
}
