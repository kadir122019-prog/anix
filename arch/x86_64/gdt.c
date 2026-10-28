#include <anix/gdt.h>
#include <anix/string.h>
#include <anix/types.h>
#define KERNEL_CODE_SELECTOR 0x08u
#define KERNEL_DATA_SELECTOR 0x10u
#define TSS_SELECTOR 0x18u
struct tss64 {
    uint32_t reserved0;
    uint64_t rsp[3];
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));
struct gdtr { uint16_t limit; uint64_t base; } __attribute__((packed));
static uint64_t gdt[7];
static struct tss64 tss;
static uint8_t rsp0_stack[16384] __attribute__((aligned(16)));
static uint8_t double_fault_stack[16384] __attribute__((aligned(16)));
extern void gdt_load(const struct gdtr *descriptor, uint16_t data_selector, uint16_t code_selector);
static void set_tss_descriptor(uintptr_t base, uint32_t limit) {
    uint64_t low = 0;
    low |= (uint64_t)(limit & 0xffffu);
    low |= (uint64_t)(base & 0xffffffu) << 16;
    low |= (uint64_t)0x89u << 40;
    low |= (uint64_t)((limit >> 16) & 0x0fu) << 48;
    low |= (uint64_t)((base >> 24) & 0xffu) << 56;
    gdt[3] = low;
    gdt[4] = (uint64_t)(base >> 32);
}
void gdt_init(void) {
    kmemset(&tss, 0, sizeof(tss));
    tss.rsp[0] = (uintptr_t)(rsp0_stack + sizeof(rsp0_stack));
    tss.ist[0] = (uintptr_t)(double_fault_stack + sizeof(double_fault_stack));
    tss.iomap_base = sizeof(tss);
    gdt[0] = 0;
    gdt[1] = 0x00af9a000000ffffULL;
    gdt[2] = 0x00cf92000000ffffULL;
    set_tss_descriptor((uintptr_t)&tss, sizeof(tss) - 1u);
    gdt[5] = 0x00cff2000000ffffULL; /* user data, selector 0x2b */
    gdt[6] = 0x00affa000000ffffULL; /* user code, selector 0x33 */
    const struct gdtr gdtr = { .limit = sizeof(gdt) - 1u, .base = (uintptr_t)gdt };
    gdt_load(&gdtr, KERNEL_DATA_SELECTOR, KERNEL_CODE_SELECTOR);
    __asm__ volatile("ltr %0" : : "rm"((uint16_t)TSS_SELECTOR) : "memory");
}
void gdt_set_rsp0(uintptr_t stack_top) {
    tss.rsp[0] = stack_top;
    __asm__ volatile("" : : : "memory");
}
size_t gdt_live_ranges(struct live_range *ranges, size_t capacity) {
    const struct live_range live[] = {
        { "gdt", (uint64_t)gdt, (uint64_t)gdt + sizeof(gdt) },
        { "tss", (uint64_t)&tss, (uint64_t)&tss + sizeof(tss) },
        { "rsp0", (uint64_t)rsp0_stack, (uint64_t)rsp0_stack + sizeof(rsp0_stack) },
        { "double-fault-ist", (uint64_t)double_fault_stack, (uint64_t)double_fault_stack + sizeof(double_fault_stack) }
    };
    size_t count = sizeof(live) / sizeof(live[0]);
    size_t copy = capacity < count ? capacity : count;
    for (size_t i = 0; ranges && i < copy; ++i) ranges[i] = live[i];
    return count;
}
