#include <anix/keyboard.h>
#include <anix/serial.h>
#include <anix/string.h>
#include <anix/terminal.h>
#include <anix/scheduler.h>

static struct wait_queue input_waiters;

void terminal_input_notify(void) { wait_queue_wake_all(&input_waiters); }

void terminal_write(const char *text) { serial_write(text); }
void terminal_writeln(const char *text) { serial_write(text); serial_write("\n"); }

static bool try_read_character(char *character) {
    return keyboard_try_read(character) || serial_try_read(character);
}

bool terminal_read_character(char *character) {
    if (!character) return false;
    for (;;) {
        if (try_read_character(character)) return true;
        uintptr_t interrupt_flags;
        __asm__ volatile("pushfq; popq %0; cli" : "=r"(interrupt_flags) : : "memory", "cc");
        const bool available = try_read_character(character);
        if (!available && serial_rx_irq_active()) wait_queue_wait(&input_waiters);
        __asm__ volatile("pushq %0; popfq" : : "r"(interrupt_flags) : "memory", "cc");
        if (available) return true;
        if (!serial_rx_irq_active()) {
            __asm__ volatile("sti; hlt" : : : "memory");
            scheduler_yield();
        }
    }
}

static bool collect_line(char *buffer, size_t capacity) {
    if (!buffer || capacity < 2u) return false;
    size_t length = 0;
    for (;;) {
        char character;
        if (!try_read_character(&character)) {
            uintptr_t interrupt_flags;
            __asm__ volatile("pushfq; popq %0; cli" : "=r"(interrupt_flags) : : "memory", "cc");
            const bool available = try_read_character(&character);
            if (!available && serial_rx_irq_active()) wait_queue_wait(&input_waiters);
            __asm__ volatile("pushq %0; popfq" : : "r"(interrupt_flags) : "memory", "cc");
            if (!available) {
                if (!serial_rx_irq_active()) {
                    __asm__ volatile("sti; hlt" : : : "memory");
                    scheduler_yield();
                }
                continue;
            }
        }
        if (character == '\r') character = '\n';
        if (character == '\n') {
            buffer[length] = '\0';
            serial_write("\n");
            return true;
        }
        if (character == '\b' || character == 0x7f) {
            if (length) {
                --length;
                serial_write("\b \b");
            }
            continue;
        }
        if ((unsigned char)character < 0x20u || length + 1u >= capacity) continue;
        buffer[length++] = character;
        serial_putc(character);
    }
}

static char pending_line[256];
static size_t pending_length;
static size_t pending_offset;
static bool pending_ready;

static bool ensure_pending_line(void) {
    if (pending_ready) return true;
    if (!collect_line(pending_line, sizeof(pending_line))) return false;
    pending_length = kstrlen(pending_line);
    pending_line[pending_length++] = '\n';
    pending_offset = 0;
    pending_ready = true;
    return true;
}

size_t terminal_read(char *buffer, size_t capacity) {
    if (!buffer || capacity == 0 || !ensure_pending_line()) return 0;
    size_t amount = pending_length - pending_offset;
    if (amount > capacity) amount = capacity;
    kmemcpy(buffer, pending_line + pending_offset, amount);
    pending_offset += amount;
    if (pending_offset == pending_length) pending_ready = false;
    return amount;
}

bool terminal_read_line(char *buffer, size_t capacity) {
    if (!buffer || capacity < 2u) return false;
    if (!pending_ready) return collect_line(buffer, capacity);
    size_t available = pending_length - pending_offset;
    if (available && pending_line[pending_offset + available - 1u] == '\n') --available;
    size_t amount = available < capacity - 1u ? available : capacity - 1u;
    kmemcpy(buffer, pending_line + pending_offset, amount);
    buffer[amount] = '\0';
    pending_ready = false;
    pending_length = pending_offset = 0;
    return true;
}
