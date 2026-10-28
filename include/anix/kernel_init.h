#ifndef ANIX_KERNEL_INIT_H
#define ANIX_KERNEL_INIT_H
#include <anix/acpi.h>
#include <anix/vmm.h>
bool kernel_platform_discover(const struct vmm_space *active_space, struct acpi_platform_info *info);
bool kernel_platform_start(const struct acpi_platform_info *info);
bool kernel_memory_selftest(void);
#endif
