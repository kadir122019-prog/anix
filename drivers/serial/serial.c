#include <anix/apic.h>
#include <anix/framebuffer.h>
#include <anix/interrupts.h>
#include <anix/serial.h>
#include <anix/terminal.h>
#define COM1 0x3f8u
#define COM1_IER (COM1 + 1u)
#define COM1_IIR (COM1 + 2u)
#define COM1_LSR (COM1 + 5u)
#define COM1_MSR (COM1 + 6u)
#define SERIAL_RX_QUEUE_SIZE 1024u
static inline void outb(uint16_t port, uint8_t value) { __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port)); }
static inline uint8_t inb(uint16_t port) { uint8_t value; __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port)); return value; }
static char receive_queue[SERIAL_RX_QUEUE_SIZE];
static volatile uint16_t receive_head, receive_tail;
static bool receive_irq_enabled;
static bool serial_ready;

static void serial_interrupt(struct interrupt_frame *frame) {
    (void)frame;
    bool received = false;
    while (!(inb(COM1_IIR) & 1u)) {
        const uint8_t reason = inb(COM1_IIR) & 0x0eu;
        if (reason == 0x04u || reason == 0x0cu) {
            while (inb(COM1_LSR) & 1u) {
                const char character = (char)inb(COM1);
                const uint16_t next = (uint16_t)((receive_head + 1u) & (SERIAL_RX_QUEUE_SIZE - 1u));
                const uint16_t tail = __atomic_load_n(&receive_tail, __ATOMIC_ACQUIRE);
                if (next != tail) {
                    receive_queue[receive_head] = character;
                    __atomic_store_n(&receive_head, next, __ATOMIC_RELEASE);
                    received = true;
                }
            }
        } else if (reason == 0x06u) {
            (void)inb(COM1_LSR);
        } else if (reason == 0x00u) {
            (void)inb(COM1_MSR);
        } else break;
    }
    apic_eoi();
    if (received) terminal_input_notify();
}

bool serial_init(void) {
    if (serial_ready) return true;
    outb(COM1_IER, 0x00); outb(COM1 + 3u, 0x80); outb(COM1, 0x01);
    outb(COM1_IER, 0x00); outb(COM1 + 3u, 0x03); outb(COM1 + 2u, 0xc7); outb(COM1 + 4u, 0x0b);
    outb(COM1 + 4u, 0x1e); outb(COM1, 0xae);
    if (inb(COM1) != 0xae) return false;
    outb(COM1 + 4u, 0x0f);
    serial_ready = true;
    return true;
}

bool serial_available(void) { return serial_ready; }

bool serial_irq_init(void) {
    if (!serial_ready || receive_irq_enabled || !irq_register(36u, serial_interrupt) || !apic_route_isa(4u, 36u)) return false;
    while (inb(COM1_LSR) & 1u) (void)inb(COM1);
    __atomic_store_n(&receive_head, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&receive_tail, 0, __ATOMIC_RELAXED);
    outb(COM1_IER, 0x01u);
    receive_irq_enabled = true;
    return true;
}

bool serial_rx_irq_active(void) { return receive_irq_enabled; }

static void serial_send_uart(char c) {
    if (!serial_ready) return;
    if (c == '\n') { while ((inb(COM1 + 5) & 0x20u) == 0) {} outb(COM1, '\r'); }
    while ((inb(COM1 + 5) & 0x20u) == 0) {}
    outb(COM1, (uint8_t)c);
}
void serial_putc(char c) {
    framebuffer_console_putc(c);
    serial_send_uart(c);
}
void serial_write_n(const char *s, size_t length) {
    if (!s || length == 0u) return;
    framebuffer_console_write(s, length);
    for (size_t i = 0; i < length; ++i) serial_send_uart(s[i]);
}
void serial_write(const char *s) {
    if (!s) return;
    size_t length = 0;
    while (s[length]) ++length;
    serial_write_n(s, length);
}
void serial_write_ansi_color(uint8_t code) {
    if (!((code >= 30u && code <= 37u) || (code >= 40u && code <= 47u) ||
          (code >= 90u && code <= 97u) || (code >= 100u && code <= 107u))) return;
    serial_write("\033[");
    serial_write_dec(code);
    serial_putc('m');
}
void serial_write_ansi_reset(void) { serial_write("\033[0m"); }
bool serial_color_test(void) {
    static const char *const names[16] = {
        "black", "red", "green", "yellow", "blue", "magenta", "cyan", "white",
        "bright-black", "bright-red", "bright-green", "bright-yellow",
        "bright-blue", "bright-magenta", "bright-cyan", "bright-white"
    };
    serial_write("[tty] ANSI 16-color foreground test:");
    for (uint8_t i = 0; i < 16u; ++i) {
        serial_putc(' ');
        serial_write_ansi_color(i < 8u ? (uint8_t)(30u + i) : (uint8_t)(82u + i));
        if (i == 0u) serial_write_ansi_color(47u);
        serial_write(names[i]);
        serial_write_ansi_reset();
    }
    serial_putc('\n');
    serial_write("[tty] ANSI 16-color background test:");
    for (uint8_t i = 0; i < 16u; ++i) {
        serial_putc(' ');
        serial_write_ansi_color(i == 7u || i == 15u ? 30u : 97u);
        serial_write_ansi_color(i < 8u ? (uint8_t)(40u + i) : (uint8_t)(92u + i));
        serial_write("  ");
        serial_write_ansi_reset();
    }
    serial_putc('\n');
    serial_write("ANIX_TTY_COLOR_TEST_OK\n");
    return true;
}
void serial_write_service_ok(const char *name) {
    serial_write("  "); serial_write_ansi_color(32u); serial_write("[OK]"); serial_write_ansi_reset();
    serial_putc(' '); serial_write(name); serial_putc('\n');
}
void serial_write_service_skip(const char *name) {
    serial_write("  "); serial_write_ansi_color(33u); serial_write("[SKIP]"); serial_write_ansi_reset();
    serial_putc(' '); serial_write(name); serial_putc('\n');
}
void serial_write_hex(uint64_t v) { static const char d[]="0123456789abcdef"; serial_write("0x"); for (int i=15;i>=0;--i) serial_putc(d[(v >> (i*4)) & 0xf]); }
void serial_write_dec(uint64_t v) { char b[21]; size_t n=0; if (!v) { serial_putc('0'); return; } while(v){b[n++]=(char)('0'+v%10);v/=10;} while(n) serial_putc(b[--n]); }
bool serial_try_read(char *character) {
    if (!character || !serial_ready) return false;
    if (!receive_irq_enabled) {
        if (!(inb(COM1_LSR) & 1u)) return false;
        *character = (char)inb(COM1);
        return true;
    }
    const uint16_t tail = __atomic_load_n(&receive_tail, __ATOMIC_RELAXED);
    const uint16_t head = __atomic_load_n(&receive_head, __ATOMIC_ACQUIRE);
    if (tail == head) return false;
    *character = receive_queue[tail];
    __atomic_store_n(&receive_tail, (uint16_t)((tail + 1u) & (SERIAL_RX_QUEUE_SIZE - 1u)), __ATOMIC_RELEASE);
    return true;
}
