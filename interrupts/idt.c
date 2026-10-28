#include <anix/interrupts.h>
#include <anix/panic.h>
#include <anix/serial.h>
#include <anix/string.h>
#include <anix/user.h>
#define IDT_GATE_INTERRUPT 0x8eu
#define KERNEL_CODE_SELECTOR 0x08u
struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t attributes;
    uint16_t offset_middle;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed));
struct idtr { uint16_t limit; uint64_t base; } __attribute__((packed));
static struct idt_entry idt[256];
static irq_handler irq_handlers[256];
_Static_assert(sizeof(struct idt_entry) == 16, "x86-64 IDT entry must be 16 bytes");
_Static_assert(sizeof(struct interrupt_frame) == 160, "exception frame layout mismatch");
extern void *exception_stub_table[32];
extern void *irq_stub_table[16];
extern void *syscall_stub;
static const char *const exception_names[32] = {
    "divide error", "debug", "non-maskable interrupt", "breakpoint",
    "overflow", "bound range exceeded", "invalid opcode", "device not available",
    "double fault", "coprocessor segment overrun", "invalid TSS", "segment not present",
    "stack-segment fault", "general protection fault", "page fault", "reserved",
    "x87 floating-point exception", "alignment check", "machine check", "SIMD floating-point exception",
    "virtualization exception", "control protection exception", "reserved", "reserved",
    "reserved", "reserved", "reserved", "reserved", "hypervisor injection exception",
    "VMM communication exception", "security exception", "reserved"
};
static void idt_set_gate(uint8_t vector, const void *handler, uint8_t ist) {
    uintptr_t address = (uintptr_t)handler;
    idt[vector] = (struct idt_entry){
        .offset_low = address & 0xffffu,
        .selector = KERNEL_CODE_SELECTOR,
        .ist = ist & 0x07u,
        .attributes = IDT_GATE_INTERRUPT,
        .offset_middle = (address >> 16) & 0xffffu,
        .offset_high = address >> 32,
        .reserved = 0
    };
}
void idt_init(void) {
    kmemset(idt, 0, sizeof(idt));
    for (uint8_t vector = 0; vector < 32; ++vector) idt_set_gate(vector, exception_stub_table[vector], vector == 8 ? 1u : 0u);
    for (uint8_t vector = 32; vector < 48; ++vector) idt_set_gate(vector, irq_stub_table[vector - 32u], 0);
    idt_set_gate(0x80u, &syscall_stub, 0);
    idt[0x80u].attributes = 0xeeu;
    const struct idtr descriptor = { .limit = sizeof(idt) - 1u, .base = (uintptr_t)idt };
    __asm__ volatile("lidt %0" : : "m"(descriptor) : "memory");
}
bool irq_register(uint8_t vector, irq_handler handler) {
    if (vector < 32 || vector >= 48 || !handler || irq_handlers[vector]) return false;
    irq_handlers[vector] = handler;
    return true;
}
void irq_dispatch(struct interrupt_frame *frame) {
    if (!frame || frame->vector < 32 || frame->vector >= 48 || !irq_handlers[frame->vector])
        panic("unhandled hardware interrupt");
    irq_handlers[frame->vector](frame);
}
static void print_register(const char *name, uint64_t value) { serial_write(name); serial_write_hex(value); serial_write("\n"); }
void exception_dispatch(const struct interrupt_frame *frame) {
    if (frame && (frame->cs & 3u) == 3u)
        user_exception_abort(frame->vector, frame->rip, frame->error_code);
    serial_write("\n*** ANIX CPU EXCEPTION ***\nvector: "); serial_write_dec(frame->vector);
    serial_write(" ("); serial_write(frame->vector < 32 ? exception_names[frame->vector] : "unknown"); serial_write(")\n");
    print_register("error:  ", frame->error_code);
    if (frame->vector == 14) {
        uint64_t cr2; __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        print_register("cr2:    ", cr2);
        serial_write("page fault: ");
        serial_write((frame->error_code & 1u) ? "protection, " : "not-present, ");
        serial_write((frame->error_code & 2u) ? "write, " : "read, ");
        serial_write((frame->error_code & 4u) ? "user" : "supervisor");
        if (frame->error_code & 8u) serial_write(", reserved-bit");
        if (frame->error_code & 16u) serial_write(", instruction-fetch");
        if (frame->error_code & 32u) serial_write(", protection-key");
        if (frame->error_code & 64u) serial_write(", shadow-stack");
        if (frame->error_code & 0x8000u) serial_write(", SGX");
        serial_write("\n");
    }
    print_register("rip:    ", frame->rip);
    print_register("cs:     ", frame->cs); print_register("rflags: ", frame->rflags);
    print_register("rax:    ", frame->rax); print_register("rbx:    ", frame->rbx);
    print_register("rcx:    ", frame->rcx); print_register("rdx:    ", frame->rdx);
    print_register("rsi:    ", frame->rsi); print_register("rdi:    ", frame->rdi);
    print_register("rbp:    ", frame->rbp); print_register("r8:     ", frame->r8);
    print_register("r9:     ", frame->r9); print_register("r10:    ", frame->r10);
    print_register("r11:    ", frame->r11); print_register("r12:    ", frame->r12);
    print_register("r13:    ", frame->r13); print_register("r14:    ", frame->r14);
    print_register("r15:    ", frame->r15);
    panic("unhandled CPU exception");
}
size_t idt_live_ranges(struct live_range *ranges, size_t capacity) {
    if (ranges && capacity) ranges[0] = (struct live_range){ "idt", (uint64_t)idt, (uint64_t)idt + sizeof(idt) };
    return 1;
}
