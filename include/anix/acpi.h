#ifndef ANIX_ACPI_H
#define ANIX_ACPI_H
#include <anix/types.h>
#include <anix/vmm.h>

#define ACPI_MAX_IOAPICS 4u
#define ACPI_MAX_IRQ_OVERRIDES 16u
struct acpi_ioapic_info { uint64_t physical_address; uint32_t gsi_base; };
struct acpi_irq_override { uint8_t source_irq; uint32_t global_irq; uint16_t flags; };
struct acpi_platform_info {
    uint64_t local_apic_physical;
    size_t ioapic_count;
    struct acpi_ioapic_info ioapics[ACPI_MAX_IOAPICS];
    size_t irq_override_count;
    struct acpi_irq_override irq_overrides[ACPI_MAX_IRQ_OVERRIDES];
    uint16_t pm1a_control_port;
    uint16_t pm1b_control_port;
    uint8_t s5_type_a;
    uint8_t s5_type_b;
    bool poweroff_supported;
};
bool acpi_discover(const struct vmm_space *active_space, struct acpi_platform_info *out);
bool acpi_poweroff_available(void);
bool acpi_poweroff(void);

#endif
