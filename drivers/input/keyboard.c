#include <anix/apic.h>
#include <anix/interrupts.h>
#include <anix/input.h>
#include <anix/keyboard.h>
#include <anix/terminal.h>

#define KEYBOARD_DATA_PORT 0x60u
#define KEYBOARD_STATUS_PORT 0x64u
#define KEYBOARD_COMMAND_PORT 0x64u
#define KEYBOARD_QUEUE_SIZE 256u

static char queue[KEYBOARD_QUEUE_SIZE];
static volatile uint16_t queue_head, queue_tail;
static volatile bool left_shift, right_shift, caps_lock, extended_code;
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
        const uint8_t status = inb(KEYBOARD_STATUS_PORT);
        if (status == 0xffu) return false; /* No i8042 controller is decoded. */
        if (!(status & 2u)) return true;
    }
    return false;
}

static bool wait_output_full(void) {
    for (uint32_t i = 0; i < 100000u; ++i) {
        const uint8_t status = inb(KEYBOARD_STATUS_PORT);
        if (status == 0xffu) return false;
        if (status & 1u) return true;
    }
    return false;
}

static void drain_output(void) {
    for (uint32_t i = 0; i < 32u && (inb(KEYBOARD_STATUS_PORT) & 1u); ++i)
        (void)inb(KEYBOARD_DATA_PORT);
}

static bool controller_read_command_byte(uint8_t *command_byte) {
    if (!command_byte || !wait_input_clear()) return false;
    outb(KEYBOARD_COMMAND_PORT, 0x20u);
    if (!wait_output_full()) return false;
    *command_byte = inb(KEYBOARD_DATA_PORT);
    return true;
}

static bool controller_write_command_byte(uint8_t command_byte) {
    if (!wait_input_clear()) return false;
    outb(KEYBOARD_COMMAND_PORT, 0x60u);
    if (!wait_input_clear()) return false;
    outb(KEYBOARD_DATA_PORT, command_byte);
    return wait_input_clear();
}

static bool keyboard_command(uint8_t command) {
    for (uint32_t attempt = 0; attempt < 3u; ++attempt) {
        if (!wait_input_clear()) return false;
        outb(KEYBOARD_DATA_PORT, command);
        if (!wait_output_full()) return false;
        const uint8_t response = inb(KEYBOARD_DATA_PORT);
        if (response == 0xfau) return true; /* ACK */
        if (response != 0xfeu) return false; /* RESEND */
    }
    return false;
}

static char translate(uint8_t code, bool shifted) {
    static const char plain[58] = {
        0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t',
        'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0, 'a', 's',
        'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\', 'z', 'x', 'c', 'v',
        'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '
    };
    static const char upper[58] = {
        0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t',
        'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0, 'A', 'S',
        'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|', 'Z', 'X', 'C', 'V',
        'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' '
    };
    if (code >= sizeof(plain)) return 0;
    bool use_upper = shifted;
    if (plain[code] >= 'a' && plain[code] <= 'z') use_upper = shifted != caps_lock;
    return use_upper ? upper[code] : plain[code];
}

static void keyboard_interrupt(struct interrupt_frame *frame) {
    (void)frame;
    bool received = false;
    while (inb(KEYBOARD_STATUS_PORT) & 1u) {
        uint8_t scan = inb(KEYBOARD_DATA_PORT);
        if (scan == 0xe0u) { extended_code = true; continue; }
        const bool released = (scan & 0x80u) != 0;
        const uint8_t code = scan & 0x7fu;
        if (!extended_code && code == 0x2au) left_shift = !released;
        if (!extended_code && code == 0x36u) right_shift = !released;
        if (!extended_code && code == 0x3au && !released) caps_lock = !caps_lock;
        uint8_t modifiers = caps_lock ? INPUT_MOD_CAPS_LOCK : 0u;
        if (left_shift || right_shift) modifiers |= INPUT_MOD_SHIFT;
        input_event_push_key((uint8_t)(code | (extended_code ? 0x80u : 0u)), !released, modifiers);
        if (!extended_code && (code == 0x2au || code == 0x36u || code == 0x3au)) {
            extended_code = false;
            continue;
        }
        if (!released && !extended_code) {
            char character = translate(code, left_shift || right_shift);
            const uint16_t next = (uint16_t)((queue_head + 1u) & (KEYBOARD_QUEUE_SIZE - 1u));
            if (character && next != queue_tail) {
                queue[queue_head] = character;
                __atomic_store_n(&queue_head, next, __ATOMIC_RELEASE);
                received = true;
            }
        }
        extended_code = false;
    }
    apic_eoi();
    if (received) terminal_input_notify();
}

bool keyboard_init(void) {
    if (initialized) return true;

    /* Keep command ACKs in the controller output buffer while initializing. */
    uintptr_t interrupt_flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(interrupt_flags) : : "memory", "cc");
    bool ready = false;
    uint8_t command_byte = 0;

    if (!wait_input_clear()) goto done;
    outb(KEYBOARD_COMMAND_PORT, 0xaeu); /* enable first PS/2 port */
    if (!wait_input_clear()) goto done;
    drain_output();
    if (!controller_read_command_byte(&command_byte)) goto done;

    /* Enable IRQ1 and the keyboard clock; retain firmware scan-code translation. */
    command_byte = (uint8_t)((command_byte | 0x01u) & ~0x10u);
    if (!keyboard_command(0xf5u)) goto done; /* disable scanning during setup */
    if (!controller_write_command_byte(command_byte)) goto done;
    if (!irq_register(33u, keyboard_interrupt) || !apic_route_isa(1u, 33u)) goto done;
    if (!keyboard_command(0xf4u)) goto done; /* enable keyboard scanning */
    initialized = true;
    ready = true;

done:
    __asm__ volatile("pushq %0; popfq" : : "r"(interrupt_flags) : "memory", "cc");
    if (!ready) {
        /* Do not leave IRQ1 enabled if the device failed its initialization handshake. */
        if (command_byte && controller_write_command_byte((uint8_t)(command_byte & ~0x01u))) {}
    }
    return ready;
}

bool keyboard_available(void) { return initialized; }

bool keyboard_try_read(char *character) {
    if (!character) return false;
    const uint16_t tail = __atomic_load_n(&queue_tail, __ATOMIC_RELAXED);
    const uint16_t head = __atomic_load_n(&queue_head, __ATOMIC_ACQUIRE);
    if (tail == head) return false;
    *character = queue[tail];
    __atomic_store_n(&queue_tail, (uint16_t)((tail + 1u) & (KEYBOARD_QUEUE_SIZE - 1u)), __ATOMIC_RELEASE);
    return true;
}
