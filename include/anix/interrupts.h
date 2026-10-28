#ifndef ANIX_INTERRUPTS_H
#define ANIX_INTERRUPTS_H
#include <anix/types.h>
#include <anix/live_ranges.h>
struct interrupt_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error_code;
    uint64_t rip, cs, rflags;
};
typedef void (*irq_handler)(struct interrupt_frame *frame);
void idt_init(void);
size_t idt_live_ranges(struct live_range *ranges, size_t capacity);
void exception_dispatch(const struct interrupt_frame *frame);
bool irq_register(uint8_t vector, irq_handler handler);
void irq_dispatch(struct interrupt_frame *frame);
bool syscall_interrupt(struct interrupt_frame *frame);
#endif
