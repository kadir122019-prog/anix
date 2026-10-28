#include <anix/apic.h>
#include <anix/framebuffer.h>
#include <anix/input.h>
#include <anix/interrupts.h>
#include <anix/mouse.h>

#define I8042_DATA 0x60u
#define I8042_STATUS 0x64u
#define I8042_COMMAND 0x64u

static volatile uint8_t packet[3];
static volatile uint8_t packet_index;
static int32_t pointer_x, pointer_y;
static uint8_t pointer_buttons;
static bool initialized;

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static bool wait_input_clear(void) {
    for (uint32_t i = 0; i < 100000u; ++i) {
        uint8_t status = inb(I8042_STATUS);
        if (status == 0xffu) return false;
        if (!(status & 2u)) return true;
    }
    return false;
}

static bool wait_output_full(void) {
    for (uint32_t i = 0; i < 100000u; ++i) {
        uint8_t status = inb(I8042_STATUS);
        if (status == 0xffu) return false;
        if (status & 1u) return true;
    }
    return false;
}

static bool read_command_byte(uint8_t *value) {
    if (!value || !wait_input_clear()) return false;
    outb(I8042_COMMAND, 0x20u);
    if (!wait_output_full()) return false;
    *value = inb(I8042_DATA);
    return true;
}

static bool write_command_byte(uint8_t value) {
    if (!wait_input_clear()) return false;
    outb(I8042_COMMAND, 0x60u);
    if (!wait_input_clear()) return false;
    outb(I8042_DATA, value);
    return wait_input_clear();
}

static bool mouse_command(uint8_t command) {
    if (!wait_input_clear()) return false;
    outb(I8042_COMMAND, 0xd4u); /* route the next byte to the auxiliary device */
    if (!wait_input_clear()) return false;
    outb(I8042_DATA, command);
    if (!wait_output_full()) return false;
    return inb(I8042_DATA) == 0xfau;
}

static void clamp_pointer(void) {
    struct framebuffer_info info;
    if (framebuffer_get_info(&info)) {
        if (pointer_x < 0) pointer_x = 0;
        if (pointer_y < 0) pointer_y = 0;
        if ((uint64_t)pointer_x >= info.width) pointer_x = (int32_t)(info.width - 1u);
        if ((uint64_t)pointer_y >= info.height) pointer_y = (int32_t)(info.height - 1u);
    } else {
        if (pointer_x < 0) pointer_x = 0;
        if (pointer_y < 0) pointer_y = 0;
        if (pointer_x > 32767) pointer_x = 32767;
        if (pointer_y > 32767) pointer_y = 32767;
    }
}

static void mouse_interrupt(struct interrupt_frame *frame) {
    (void)frame;
    int16_t dx = 0, dy = 0;
    bool complete = false;
    for (;;) {
        const uint8_t status = inb(I8042_STATUS);
        if (!(status & 1u) || !(status & 0x20u)) break;
        const uint8_t value = inb(I8042_DATA);
        if (packet_index == 0u && !(value & 0x08u)) continue; /* packet sync bit */
        packet[packet_index++] = value;
        if (packet_index != 3u) continue;
        packet_index = 0;
        const uint8_t flags = packet[0];
        if (flags & 0xc0u) continue; /* ignore saturated movement */
        dx = (int8_t)packet[1];
        dy = (int16_t)-(int8_t)packet[2];
        pointer_x += dx;
        pointer_y += dy;
        clamp_pointer();
        pointer_buttons = flags & 0x07u;
        complete = true;
    }
    apic_eoi();
    if (!complete) return;
    framebuffer_cursor_move((uint64_t)pointer_x, (uint64_t)pointer_y);
    input_event_push_pointer(dx, dy, (uint16_t)pointer_x, (uint16_t)pointer_y, pointer_buttons);
}

bool mouse_init(void) {
    if (initialized) return true;
    uintptr_t interrupt_flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(interrupt_flags) : : "memory", "cc");
    bool ready = false;
    uint8_t command_byte;
    if (!wait_input_clear()) goto done;
    outb(I8042_COMMAND, 0xa8u); /* enable auxiliary port */
    if (!read_command_byte(&command_byte)) goto done;
    if (!mouse_command(0xf5u)) goto done; /* disable reporting during setup */
    command_byte = (uint8_t)((command_byte | 0x02u) & ~0x20u); /* IRQ12 on, port enabled */
    if (!write_command_byte(command_byte)) goto done;
    if (!irq_register(34u, mouse_interrupt) || !apic_route_isa(12u, 34u)) goto done;
    if (!mouse_command(0xf4u)) goto done; /* enable data reporting */
    initialized = true;
    ready = true;

done:
    __asm__ volatile("pushq %0; popfq" : : "r"(interrupt_flags) : "memory", "cc");
    return ready;
}

bool mouse_available(void) { return initialized; }

void mouse_video_ready(void) {
    struct framebuffer_info info;
    if (!initialized || !framebuffer_get_info(&info)) return;
    pointer_x = (int32_t)(info.width / 2u);
    pointer_y = (int32_t)(info.height / 2u);
    framebuffer_cursor_move((uint64_t)pointer_x, (uint64_t)pointer_y);
    framebuffer_cursor_enable(true);
}
