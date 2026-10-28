#include <anix/input.h>
#include <anix/scheduler.h>

#define INPUT_EVENT_QUEUE_SIZE 128u

static struct input_event queue[INPUT_EVENT_QUEUE_SIZE];
static volatile uint16_t queue_head, queue_tail;
static volatile uint64_t dropped_events;
static struct wait_queue event_waiters;

static void push_event(struct input_event event) {
    const uint16_t head = __atomic_load_n(&queue_head, __ATOMIC_RELAXED);
    const uint16_t next = (uint16_t)((head + 1u) & (INPUT_EVENT_QUEUE_SIZE - 1u));
    const uint16_t tail = __atomic_load_n(&queue_tail, __ATOMIC_ACQUIRE);
    if (next == tail) {
        __atomic_add_fetch(&dropped_events, 1u, __ATOMIC_RELAXED);
        return;
    }
    queue[head] = event;
    __atomic_store_n(&queue_head, next, __ATOMIC_RELEASE);
    wait_queue_wake_all(&event_waiters);
}

bool input_event_poll(struct input_event *event) {
    if (!event) return false;
    const uint16_t tail = __atomic_load_n(&queue_tail, __ATOMIC_RELAXED);
    const uint16_t head = __atomic_load_n(&queue_head, __ATOMIC_ACQUIRE);
    if (tail == head) return false;
    *event = queue[tail];
    __atomic_store_n(&queue_tail, (uint16_t)((tail + 1u) & (INPUT_EVENT_QUEUE_SIZE - 1u)), __ATOMIC_RELEASE);
    return true;
}

bool input_event_wait(struct input_event *event) {
    if (!event) return false;
    for (;;) {
        uintptr_t interrupt_flags;
        __asm__ volatile("pushfq; popq %0; cli" : "=r"(interrupt_flags) : : "memory", "cc");
        const bool available = input_event_poll(event);
        if (!available) wait_queue_wait(&event_waiters);
        __asm__ volatile("pushq %0; popfq" : : "r"(interrupt_flags) : "memory", "cc");
        if (available) return true;
    }
}

uint64_t input_event_dropped(void) {
    return __atomic_load_n(&dropped_events, __ATOMIC_RELAXED);
}

void input_event_push_key(uint8_t scan_code, bool pressed, uint8_t modifiers) {
    push_event((struct input_event){
        .type = INPUT_EVENT_KEY,
        .code = scan_code,
        .modifiers = modifiers,
        .pressed = pressed,
    });
}

void input_event_push_pointer(int16_t dx, int16_t dy, uint16_t x, uint16_t y, uint8_t buttons) {
    push_event((struct input_event){
        .type = INPUT_EVENT_POINTER,
        .buttons = buttons,
        .x = x,
        .y = y,
        .dx = dx,
        .dy = dy,
    });
}
