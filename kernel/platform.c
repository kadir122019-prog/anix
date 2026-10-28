#include <anix/acpi.h>
#include <anix/apic.h>
#include <anix/kernel_init.h>
#include <anix/keyboard.h>
#include <anix/mouse.h>
#include <anix/panic.h>
#include <anix/serial.h>
#include <anix/timer.h>

bool kernel_platform_discover(const struct vmm_space *active_space, struct acpi_platform_info *info) {
    if (!active_space || !info || !acpi_discover(active_space, info)) return false;
    serial_write("[acpi] local APIC: "); serial_write_hex(info->local_apic_physical);
    serial_write(", I/O APIC count: "); serial_write_dec(info->ioapic_count); serial_write("\n");
    return true;
}

bool kernel_platform_start(const struct acpi_platform_info *info) {
    if (!info || !apic_init(info) || !timer_init()) return false;
    serial_write("[irq] local APIC mode: ");
    serial_write(apic_x2apic_active() ? "x2APIC\n" : "xAPIC\n");
    const bool serial_irq = serial_available() && serial_irq_init();
    const bool keyboard_ready = keyboard_init();
#if defined(ANIX_ENABLE_DESKTOP)
    const bool mouse_ready = mouse_init();
#endif
    timer_sleep_ticks(2u);
    if (timer_ticks() < 2u) return false;
    serial_write("[irq] APIC/IOAPIC and timer: ok; ticks: "); serial_write_dec(timer_ticks()); serial_write("\n");
    serial_write("[input] keyboard: "); serial_write(keyboard_ready ? "ready" : "unavailable");
    serial_write("; serial RX IRQ: ");
    serial_write(serial_irq ? "active\n" : "unavailable, polling fallback\n");
#if defined(ANIX_ENABLE_DESKTOP)
    serial_write("[input] PS/2 mouse: ");
    serial_write(mouse_ready ? "ready\n" : "unavailable\n");
#else
    serial_write("[input] desktop pointer support: disabled\n");
#endif
    return true;
}
