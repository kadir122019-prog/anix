#include <anix/apic.h>
#include <anix/interrupts.h>
#include <anix/kva.h>
#include <anix/pmm.h>
#include <anix/serial.h>

#define IA32_APIC_BASE_MSR 0x1bu
#define IA32_APIC_BASE_ENABLE (1ULL << 11)
#define IA32_APIC_BASE_X2APIC (1ULL << 10)
#define APIC_REG_ID 0x020u
#define APIC_REG_TPR 0x080u
#define APIC_REG_EOI 0x0b0u
#define APIC_REG_SVR 0x0f0u
#define APIC_REG_LVT_TIMER 0x320u
#define APIC_REG_LVT_THERMAL 0x330u
#define APIC_REG_LVT_PERF 0x340u
#define APIC_REG_LVT_LINT0 0x350u
#define APIC_REG_LVT_LINT1 0x360u
#define APIC_REG_LVT_ERROR 0x370u
#define APIC_REG_TIMER_DIVIDE 0x3e0u
#define APIC_REG_TIMER_INITIAL 0x380u
#define APIC_SVR_ENABLE 0x100u
#define APIC_SPURIOUS_VECTOR 47u
#define IOAPIC_REG_SELECT 0x00u
#define IOAPIC_REG_WINDOW 0x10u
#define IOAPIC_REG_ID 0x00u
#define IOAPIC_REG_VERSION 0x01u
#define IOAPIC_REG_REDIRECTION 0x10u

struct ioapic_state {
    volatile uint32_t *registers;
    uint32_t gsi_base;
    uint32_t redirection_count;
};
static volatile uint32_t *local_apic;
static struct ioapic_state ioapics[ACPI_MAX_IOAPICS];
static struct acpi_platform_info platform_info;
static size_t ioapic_count;
static uint32_t local_apic_id;
static bool x2apic_mode;
static bool initialized;

static uint64_t rdmsr(uint32_t msr) {
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

static void wrmsr(uint32_t msr, uint64_t value) {
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)) : "memory");
}

static uint32_t lapic_read(uint32_t offset) {
    if (x2apic_mode) return (uint32_t)rdmsr(0x800u + offset / 16u);
    __asm__ volatile("mfence" : : : "memory");
    return local_apic[offset / sizeof(uint32_t)];
}

static void lapic_write(uint32_t offset, uint32_t value) {
    if (x2apic_mode) {
        wrmsr(0x800u + offset / 16u, value);
        return;
    }
    local_apic[offset / sizeof(uint32_t)] = value;
    __asm__ volatile("mfence" : : : "memory");
    (void)local_apic[APIC_REG_ID / sizeof(uint32_t)];
}

static uint32_t ioapic_read(struct ioapic_state *ioapic, uint8_t reg) {
    ioapic->registers[IOAPIC_REG_SELECT / 4u] = reg;
    __asm__ volatile("mfence" : : : "memory");
    return ioapic->registers[IOAPIC_REG_WINDOW / 4u];
}

static void ioapic_write(struct ioapic_state *ioapic, uint8_t reg, uint32_t value) {
    ioapic->registers[IOAPIC_REG_SELECT / 4u] = reg;
    __asm__ volatile("mfence" : : : "memory");
    ioapic->registers[IOAPIC_REG_WINDOW / 4u] = value;
    __asm__ volatile("mfence" : : : "memory");
}

bool apic_init(const struct acpi_platform_info *platform) {
    if (!platform || !platform->local_apic_physical || platform->ioapic_count == 0 ||
        platform->ioapic_count > ACPI_MAX_IOAPICS || initialized) return false;
    platform_info = *platform;
    uint64_t base_msr = rdmsr(IA32_APIC_BASE_MSR);
    x2apic_mode = (base_msr & IA32_APIC_BASE_X2APIC) != 0;
    if (x2apic_mode) {
        uint32_t eax, ebx, ecx, edx;
        __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1u), "c"(0u));
        (void)eax; (void)ebx; (void)edx;
        if (!(ecx & (1u << 21))) return false;
        wrmsr(IA32_APIC_BASE_MSR, base_msr | IA32_APIC_BASE_ENABLE);
    } else {
        base_msr = (base_msr & ~VMM_PHYS_MASK) |
            (platform->local_apic_physical & VMM_PHYS_MASK) | IA32_APIC_BASE_ENABLE;
        wrmsr(IA32_APIC_BASE_MSR, base_msr);
        local_apic = kva_map_physical(platform->local_apic_physical & ~(uint64_t)(VMM_PAGE_SIZE - 1u),
            1, VMM_WRITABLE | VMM_NO_EXECUTE | VMM_CACHE_DISABLE);
        if (!local_apic) return false;
    }

    for (size_t i = 0; i < platform->ioapic_count; ++i) {
        const uint64_t physical = platform->ioapics[i].physical_address;
        volatile uint32_t *registers = kva_map_physical(physical & ~(uint64_t)(VMM_PAGE_SIZE - 1u),
            1, VMM_WRITABLE | VMM_NO_EXECUTE | VMM_CACHE_DISABLE);
        if (!registers) return false;
        ioapics[ioapic_count].registers = registers;
        ioapics[ioapic_count].gsi_base = platform->ioapics[i].gsi_base;
        ioapics[ioapic_count].redirection_count = ((ioapic_read(&ioapics[ioapic_count], IOAPIC_REG_VERSION) >> 16) & 0xffu) + 1u;
        ++ioapic_count;
    }

    /* Route all external interrupts through the I/O APIC. */
    __asm__ volatile("outb %0, %1" : : "a"((uint8_t)0xff), "Nd"((uint16_t)0x21));
    __asm__ volatile("outb %0, %1" : : "a"((uint8_t)0xff), "Nd"((uint16_t)0xa1));
    const uint32_t id_register = lapic_read(APIC_REG_ID);
    local_apic_id = x2apic_mode ? id_register : id_register >> 24;
    lapic_write(APIC_REG_TPR, 0);
    lapic_write(APIC_REG_EOI, 0);
    lapic_write(APIC_REG_LVT_TIMER, 1u << 16);
    lapic_write(APIC_REG_LVT_THERMAL, 1u << 16);
    lapic_write(APIC_REG_LVT_PERF, 1u << 16);
    lapic_write(APIC_REG_LVT_LINT0, 1u << 16);
    lapic_write(APIC_REG_LVT_LINT1, 1u << 16);
    lapic_write(APIC_REG_LVT_ERROR, 1u << 16);
    lapic_write(APIC_REG_SVR, APIC_SVR_ENABLE | APIC_SPURIOUS_VECTOR);
    initialized = true;
    return true;
}

bool apic_route_isa(uint8_t irq, uint8_t vector) {
    if (!initialized || vector < 32 || vector >= 48 || local_apic_id > 0xffu) return false;
    uint32_t gsi = irq;
    uint16_t flags = 0;
    const struct acpi_platform_info *platform = &platform_info;
    for (size_t i = 0; platform && i < platform->irq_override_count; ++i) {
        if (platform->irq_overrides[i].source_irq == irq) {
            gsi = platform->irq_overrides[i].global_irq;
            flags = platform->irq_overrides[i].flags;
            break;
        }
    }
    for (size_t i = 0; i < ioapic_count; ++i) {
        struct ioapic_state *ioapic = &ioapics[i];
        if (gsi < ioapic->gsi_base || gsi - ioapic->gsi_base >= ioapic->redirection_count) continue;
        const uint32_t index = gsi - ioapic->gsi_base;
        const uint8_t low_reg = (uint8_t)(IOAPIC_REG_REDIRECTION + index * 2u);
        uint32_t low = vector | (1u << 16);
        const uint8_t polarity = flags & 3u;
        const uint8_t trigger = (flags >> 2) & 3u;
        if (polarity == 3u) low |= 1u << 13;
        else if (polarity == 2u) return false;
        if (trigger == 3u) low |= 1u << 15;
        else if (trigger == 2u) return false;
        ioapic_write(ioapic, (uint8_t)(low_reg + 1u), (uint32_t)local_apic_id << 24);
        ioapic_write(ioapic, low_reg, low);
        ioapic_write(ioapic, low_reg, low & ~(1u << 16));
        return true;
    }
    return false;
}

bool apic_timer_start(uint32_t initial_count) {
    if (!initialized || initial_count == 0) return false;
    lapic_write(APIC_REG_TIMER_DIVIDE, 0x3u); /* divide by 16 */
    lapic_write(APIC_REG_LVT_TIMER, (1u << 17) | 32u); /* periodic, vector 32 */
    lapic_write(APIC_REG_TIMER_INITIAL, initial_count);
    return true;
}

void apic_eoi(void) {
    if (initialized) lapic_write(APIC_REG_EOI, 0);
}

bool apic_x2apic_active(void) { return initialized && x2apic_mode; }
