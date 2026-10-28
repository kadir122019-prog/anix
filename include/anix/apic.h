#ifndef ANIX_APIC_H
#define ANIX_APIC_H
#include <anix/acpi.h>

bool apic_init(const struct acpi_platform_info *platform);
bool apic_route_isa(uint8_t irq, uint8_t vector);
bool apic_timer_start(uint32_t initial_count);
void apic_eoi(void);
bool apic_x2apic_active(void);

#endif
