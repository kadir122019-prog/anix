#include <anix/acpi.h>
#include <anix/boot.h>
#include <anix/pmm.h>
#include <anix/string.h>

#define ACPI_MAX_TABLE_BYTES (1024u * 1024u)
#define ACPI_MAX_ROOT_ENTRIES 256u

struct rsdp_v1 {
    char signature[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address;
} __attribute__((packed));
struct rsdp_v2 {
    struct rsdp_v1 first;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t extended_checksum;
    uint8_t reserved[3];
} __attribute__((packed));
struct sdt_header {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));
struct madt_header {
    struct sdt_header header;
    uint32_t local_apic_address;
    uint32_t flags;
} __attribute__((packed));
struct madt_entry_header { uint8_t type, length; } __attribute__((packed));
struct acpi_gas { uint8_t space, width, offset, access_size; uint64_t address; } __attribute__((packed));

static struct acpi_platform_info poweroff_platform;

static uint32_t read_u32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
        ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static uint64_t read_u64(const uint8_t *bytes) {
    return (uint64_t)read_u32(bytes) | ((uint64_t)read_u32(bytes + 4) << 32);
}

static const uint8_t *mapped_physical(const struct vmm_space *space, uint64_t physical, uint64_t length) {
    if (!space || !length || physical > UINT64_MAX - (length - 1u)) return 0;
    const uint64_t virtual_address = physical + pmm_hhdm_offset();
    if (virtual_address < physical || !vmm_is_canonical(virtual_address) ||
        !vmm_is_canonical(virtual_address + length - 1u) ||
        !vmm_query(space, virtual_address, 0, 0) ||
        !vmm_query(space, virtual_address + length - 1u, 0, 0)) return 0;
    return (const uint8_t *)(uintptr_t)virtual_address;
}

static const uint8_t *resolve_rsdp(const struct vmm_space *space, const void *address) {
    if (!address) return 0;
    const uint64_t raw = (uint64_t)(uintptr_t)address;
    if (vmm_is_canonical(raw) && vmm_query(space, raw, 0, 0)) return (const uint8_t *)address;
    return mapped_physical(space, raw, sizeof(struct rsdp_v2));
}

static bool checksum_valid(const uint8_t *bytes, size_t length) {
    uint8_t sum = 0;
    for (size_t i = 0; i < length; ++i) sum = (uint8_t)(sum + bytes[i]);
    return sum == 0;
}

static bool parse_madt(const uint8_t *bytes, size_t length, struct acpi_platform_info *out) {
    if (length < sizeof(struct madt_header)) return false;
    const struct madt_header *madt = (const struct madt_header *)bytes;
    out->local_apic_physical = madt->local_apic_address;
    size_t offset = sizeof(*madt);
    while (offset < length) {
        if (length - offset < sizeof(struct madt_entry_header)) return false;
        const struct madt_entry_header *entry = (const struct madt_entry_header *)(bytes + offset);
        if (entry->length < sizeof(*entry) || entry->length > length - offset) return false;
        switch (entry->type) {
            case 1: { /* I/O APIC */
                if (entry->length < 12u) return false;
                const uint8_t *item = bytes + offset;
                if (out->ioapic_count < ACPI_MAX_IOAPICS) {
                    out->ioapics[out->ioapic_count++] = (struct acpi_ioapic_info){
                        .physical_address = *(const uint32_t *)(const void *)(item + 4),
                        .gsi_base = *(const uint32_t *)(const void *)(item + 8)
                    };
                }
                break;
            }
            case 2: { /* Interrupt-source override */
                if (entry->length < 10u) return false;
                const uint8_t *item = bytes + offset;
                if (out->irq_override_count < ACPI_MAX_IRQ_OVERRIDES) {
                    out->irq_overrides[out->irq_override_count++] = (struct acpi_irq_override){
                        .source_irq = item[3],
                        .global_irq = *(const uint32_t *)(const void *)(item + 4),
                        .flags = *(const uint16_t *)(const void *)(item + 8)
                    };
                }
                break;
            }
            case 5: /* Local APIC address override */
                if (entry->length < 12u) return false;
                out->local_apic_physical = *(const uint64_t *)(const void *)(bytes + offset + 4);
                break;
            default:
                break;
        }
        offset += entry->length;
    }
    return out->local_apic_physical != 0 && out->ioapic_count != 0;
}

static bool aml_read_integer(const uint8_t *bytes, size_t end, size_t *offset, uint64_t *value) {
    if (!bytes || !offset || !value || *offset >= end) return false;
    const uint8_t opcode = bytes[(*offset)++];
    if (opcode == 0x00u) { *value = 0; return true; } /* ZeroOp */
    if (opcode == 0x01u) { *value = 1; return true; } /* OneOp */
    if (opcode == 0xffu) { *value = UINT64_MAX; return true; } /* OnesOp */
    size_t width;
    if (opcode == 0x0au) width = 1u;
    else if (opcode == 0x0bu) width = 2u;
    else if (opcode == 0x0cu) width = 4u;
    else if (opcode == 0x0eu) width = 8u;
    else return false;
    if (width > end - *offset) return false;
    *value = width == 8u ? read_u64(bytes + *offset) : read_u32(bytes + *offset);
    if (width == 1u) *value = bytes[*offset];
    else if (width == 2u) *value = (uint16_t)(bytes[*offset] | ((uint16_t)bytes[*offset + 1u] << 8));
    *offset += width;
    return true;
}

static bool aml_package_length(const uint8_t *bytes, size_t end, size_t offset,
                               uint32_t *length, size_t *encoded_size) {
    if (!bytes || !length || !encoded_size || offset >= end) return false;
    const uint8_t lead = bytes[offset];
    const size_t follow = lead >> 6;
    if (follow > end - offset - 1u) return false;
    uint32_t value = follow ? lead & 0x0fu : lead & 0x3fu;
    for (size_t i = 0; i < follow; ++i)
        value |= (uint32_t)bytes[offset + i + 1u] << (4u + 8u * i);
    *length = value;
    *encoded_size = follow + 1u;
    return true;
}

static bool parse_s5_types(const uint8_t *aml, size_t length, uint8_t *type_a, uint8_t *type_b) {
    if (!aml || !type_a || !type_b) return false;
    for (size_t name_offset = 0; name_offset + 6u < length; ++name_offset) {
        if (aml[name_offset] != 0x08u) continue; /* NameOp */
        size_t name_size = name_offset + 1u;
        if (aml[name_size] == 0x5cu) ++name_size; /* optional RootChar */
        if (name_size + 5u >= length || kmemcmp(aml + name_size, "_S5_", 4u) != 0) continue;
        size_t package_offset = name_size + 4u;
        if (aml[package_offset++] != 0x12u) continue; /* PackageOp */
        uint32_t package_length;
        size_t encoded_length;
        const size_t package_start = package_offset;
        if (!aml_package_length(aml, length, package_start, &package_length, &encoded_length) ||
            package_length < encoded_length + 1u || package_length > length - package_start) continue;
        const size_t package_end = package_start + package_length;
        size_t item_offset = package_start + encoded_length;
        const uint8_t item_count = aml[item_offset++];
        if (item_count < 2u) continue;
        uint64_t first, second;
        item_offset = package_start + encoded_length + 1u;
        if (!aml_read_integer(aml, package_end, &item_offset, &first) ||
            !aml_read_integer(aml, package_end, &item_offset, &second) || first > 7u || second > 7u) continue;
        *type_a = (uint8_t)first;
        *type_b = (uint8_t)second;
        return true;
    }
    return false;
}

static uint16_t pm1_control_port(const uint8_t *fadt, size_t length,
                                size_t gas_offset, size_t legacy_offset, uint8_t block_length) {
    if (length >= gas_offset + sizeof(struct acpi_gas)) {
        const struct acpi_gas *gas = (const struct acpi_gas *)(const void *)(fadt + gas_offset);
        if (gas->space == 1u && gas->width >= 16u && gas->offset == 0u &&
            gas->address != 0 && gas->address <= UINT16_MAX)
            return (uint16_t)gas->address;
    }
    if (block_length >= 2u && length >= legacy_offset + sizeof(uint32_t)) {
        const uint32_t port = read_u32(fadt + legacy_offset);
        if (port != 0 && port <= UINT16_MAX) return (uint16_t)port;
    }
    return 0;
}

static void parse_fadt(const struct vmm_space *space, const uint8_t *fadt, size_t length,
                       struct acpi_platform_info *out) {
    if (length < 90u) return;
    const uint8_t block_length = fadt[89u];
    out->pm1a_control_port = pm1_control_port(fadt, length, 172u, 64u, block_length);
    out->pm1b_control_port = pm1_control_port(fadt, length, 184u, 68u, block_length);
    uint64_t dsdt_physical = length >= 148u ? read_u64(fadt + 140u) : 0;
    if (!dsdt_physical) dsdt_physical = read_u32(fadt + 40u);
    const struct sdt_header *header = (const struct sdt_header *)mapped_physical(space, dsdt_physical, sizeof(*header));
    if (!header || kmemcmp(header->signature, "DSDT", 4u) != 0 ||
        header->length < sizeof(*header) || header->length > ACPI_MAX_TABLE_BYTES) return;
    const uint8_t *dsdt = mapped_physical(space, dsdt_physical, header->length);
    uint8_t type_a, type_b;
    if (!dsdt || !checksum_valid(dsdt, header->length) ||
        !parse_s5_types(dsdt + sizeof(*header), header->length - sizeof(*header), &type_a, &type_b)) return;
    out->s5_type_a = type_a;
    out->s5_type_b = type_b;
    out->poweroff_supported = out->pm1a_control_port != 0;
}

bool acpi_discover(const struct vmm_space *active_space, struct acpi_platform_info *out) {
    if (!active_space || !out) return false;
    *out = (struct acpi_platform_info){0};
    const uint8_t *rsdp_bytes = resolve_rsdp(active_space, boot_rsdp_address());
    if (!rsdp_bytes) return false;
    const struct rsdp_v1 *rsdp = (const struct rsdp_v1 *)rsdp_bytes;
    if (kmemcmp(rsdp->signature, "RSD PTR ", 8) != 0 || !checksum_valid(rsdp_bytes, sizeof(*rsdp))) return false;

    uint64_t root_physical;
    bool use_xsdt = false;
    if (rsdp->revision >= 2) {
        const struct rsdp_v2 *extended = (const struct rsdp_v2 *)rsdp_bytes;
        if (extended->length < sizeof(*extended) || extended->length > 4096u ||
            !checksum_valid(rsdp_bytes, extended->length)) return false;
        root_physical = extended->xsdt_address;
        use_xsdt = true;
    } else root_physical = rsdp->rsdt_address;
    if (!root_physical) return false;

    const struct sdt_header *root = (const struct sdt_header *)mapped_physical(active_space, root_physical, sizeof(*root));
    if (!root || root->length < sizeof(*root) || root->length > ACPI_MAX_TABLE_BYTES) return false;
    root = (const struct sdt_header *)mapped_physical(active_space, root_physical, root->length);
    if (!root || !checksum_valid((const uint8_t *)root, root->length)) return false;
    const size_t entry_size = use_xsdt ? 8u : 4u;
    const size_t payload_size = root->length - sizeof(*root);
    if (payload_size % entry_size != 0 || payload_size / entry_size > ACPI_MAX_ROOT_ENTRIES) return false;
    const uint8_t *entries = (const uint8_t *)root + sizeof(*root);
    const uint8_t *madt_bytes = 0;
    size_t madt_length = 0;
    const uint8_t *fadt_bytes = 0;
    size_t fadt_length = 0;
    for (size_t i = 0; i < payload_size / entry_size; ++i) {
        uint64_t table_physical = use_xsdt ? *(const uint64_t *)(const void *)(entries + i * entry_size) :
            *(const uint32_t *)(const void *)(entries + i * entry_size);
        const struct sdt_header *header = (const struct sdt_header *)mapped_physical(active_space, table_physical, sizeof(*header));
        if (!header || header->length < sizeof(*header) || header->length > ACPI_MAX_TABLE_BYTES) continue;
        if (kmemcmp(header->signature, "APIC", 4u) == 0) {
            madt_bytes = mapped_physical(active_space, table_physical, header->length);
            madt_length = header->length;
        } else if (kmemcmp(header->signature, "FACP", 4u) == 0) {
            fadt_bytes = mapped_physical(active_space, table_physical, header->length);
            fadt_length = header->length;
        }
    }
    if (!madt_bytes || !checksum_valid(madt_bytes, madt_length) || !parse_madt(madt_bytes, madt_length, out)) return false;
    if (fadt_bytes && checksum_valid(fadt_bytes, fadt_length)) parse_fadt(active_space, fadt_bytes, fadt_length, out);
    poweroff_platform = *out;
    return true;
}

bool acpi_poweroff_available(void) { return poweroff_platform.poweroff_supported; }

bool acpi_poweroff(void) {
    if (!poweroff_platform.poweroff_supported || !poweroff_platform.pm1a_control_port) return false;
    const uint16_t slp_en = 1u << 13;
    const uint16_t type_mask = 7u << 10;
    uint16_t value;
    __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(poweroff_platform.pm1a_control_port));
    value = (uint16_t)((value & ~type_mask) | ((uint16_t)poweroff_platform.s5_type_a << 10) | slp_en);
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(poweroff_platform.pm1a_control_port));
    if (poweroff_platform.pm1b_control_port) {
        __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(poweroff_platform.pm1b_control_port));
        value = (uint16_t)((value & ~type_mask) | ((uint16_t)poweroff_platform.s5_type_b << 10) | slp_en);
        __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(poweroff_platform.pm1b_control_port));
    }
    __asm__ volatile("cli" : : : "memory");
    for (;;) __asm__ volatile("hlt" : : : "memory");
}
