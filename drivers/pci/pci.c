#include <anix/pci.h>

#define PCI_CONFIG_ADDRESS 0x0cf8u
#define PCI_CONFIG_DATA 0x0cfcu

static struct pci_device_info devices[PCI_MAX_DEVICES];
static size_t device_count;
static size_t overflow_count;
static bool initialized;
static bool available;

static uint32_t port_read32(uint16_t port) {
    uint32_t value;
    __asm__ volatile("inl %w1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void port_write32(uint16_t port, uint32_t value) {
    __asm__ volatile("outl %0, %w1" : : "a"(value), "Nd"(port));
}

static uint32_t config_read32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    const uint32_t address = 0x80000000u | ((uint32_t)bus << 16) |
        ((uint32_t)device << 11) | ((uint32_t)function << 8) | (offset & 0xfcu);
    port_write32(PCI_CONFIG_ADDRESS, address);
    return port_read32(PCI_CONFIG_DATA);
}

static uint16_t config_read16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    const uint32_t value = config_read32(bus, device, function, offset);
    return (uint16_t)(value >> ((offset & 2u) * 8u));
}

static void record_function(uint8_t bus, uint8_t slot, uint8_t function) {
    const uint32_t identity = config_read32(bus, slot, function, 0x00u);
    const uint16_t vendor_id = (uint16_t)identity;
    if (vendor_id == 0xffffu || vendor_id == 0u) return;
    if (device_count == PCI_MAX_DEVICES) { ++overflow_count; return; }

    const uint32_t class_register = config_read32(bus, slot, function, 0x08u);
    const uint32_t header_register = config_read32(bus, slot, function, 0x0cu);
    devices[device_count++] = (struct pci_device_info){
        .bus = bus,
        .device = slot,
        .function = function,
        .vendor_id = vendor_id,
        .device_id = (uint16_t)(identity >> 16),
        .revision = (uint8_t)class_register,
        .programming_interface = (uint8_t)(class_register >> 8),
        .subclass = (uint8_t)(class_register >> 16),
        .class_code = (uint8_t)(class_register >> 24),
        .header_type = (uint8_t)(header_register >> 16),
    };
}

bool pci_init(void) {
    if (initialized) return available;
    initialized = true;

    const uint32_t previous_address = port_read32(PCI_CONFIG_ADDRESS);
    port_write32(PCI_CONFIG_ADDRESS, 0x80000000u);
    if (port_read32(PCI_CONFIG_ADDRESS) != 0x80000000u) {
        port_write32(PCI_CONFIG_ADDRESS, previous_address);
        return false;
    }
    available = true;

    /* Mechanism 1 exposes 256 buses. Scanning bus numbers directly also finds
     * devices behind PCI-to-PCI bridges without trusting firmware bus ranges. */
    for (uint32_t bus = 0; bus < 256u; ++bus) {
        for (uint32_t slot = 0; slot < 32u; ++slot) {
            const uint16_t vendor = config_read16((uint8_t)bus, (uint8_t)slot, 0u, 0x00u);
            if (vendor == 0xffffu || vendor == 0u) continue;
            const uint8_t header_type = (uint8_t)(config_read32((uint8_t)bus, (uint8_t)slot, 0u, 0x0cu) >> 16);
            record_function((uint8_t)bus, (uint8_t)slot, 0u);
            if (header_type & 0x80u) {
                for (uint32_t function = 1; function < 8u; ++function)
                    record_function((uint8_t)bus, (uint8_t)slot, (uint8_t)function);
            }
        }
    }
    port_write32(PCI_CONFIG_ADDRESS, previous_address);
    return true;
}

bool pci_available(void) { return available; }
size_t pci_device_count(void) { return device_count; }
size_t pci_device_overflow_count(void) { return overflow_count; }

bool pci_get_device(size_t index, struct pci_device_info *out) {
    if (!out || index >= device_count) return false;
    *out = devices[index];
    return true;
}

const char *pci_class_name(uint8_t class_code, uint8_t subclass) {
    switch (class_code) {
        case 0x01u: return "storage";
        case 0x02u: return "network";
        case 0x03u: return "display";
        case 0x04u: return "multimedia";
        case 0x06u:
            switch (subclass) {
                case 0x00u: return "host bridge";
                case 0x01u: return "ISA bridge";
                case 0x04u: return "PCI bridge";
                default: return "bridge";
            }
        case 0x0cu:
            switch (subclass) {
                case 0x03u: return "USB controller";
                case 0x05u: return "SMBus controller";
                case 0x08u: return "system peripheral";
                default: return "serial bus";
            }
        case 0x0du: return "wireless controller";
        default: return "other device";
    }
}
