#include <anix/apic.h>
#include <anix/interrupts.h>
#include <anix/timer.h>
#include <anix/scheduler.h>

static volatile uint64_t ticks;

static void timer_interrupt(struct interrupt_frame *frame) {
    (void)frame;
    ++ticks;
    apic_eoi();
    scheduler_on_timer_tick();
}

bool timer_init(void) {
    if (!irq_register(32u, timer_interrupt)) return false;
    return apic_timer_start(100000u);
}

uint64_t timer_ticks(void) {
    return __atomic_load_n(&ticks, __ATOMIC_RELAXED);
}

void timer_sleep_ticks(uint64_t count) {
    const uint64_t target = timer_ticks() + count;
    while (timer_ticks() < target) __asm__ volatile("sti; hlt" : : : "memory");
}
