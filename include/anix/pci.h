#ifndef ANIX_PCI_H
#define ANIX_PCI_H
#include <anix/types.h>

#define PCI_MAX_DEVICES 256u

struct pci_device_info {
    uint8_t bus, device, function;
    uint8_t class_code, subclass, programming_interface, revision;
    uint8_t header_type;
    uint16_t vendor_id, device_id;
};

bool pci_init(void);
bool pci_available(void);
size_t pci_device_count(void);
size_t pci_device_overflow_count(void);
bool pci_get_device(size_t index, struct pci_device_info *out);
const char *pci_class_name(uint8_t class_code, uint8_t subclass);

#endif
