#ifndef ANIX_BOOT_H
#define ANIX_BOOT_H
#include <anix/types.h>
struct anix_boot_info {
    uint64_t hhdm_offset;
    uint64_t usable_bytes;
    uint64_t memory_map_entries;
    bool framebuffer_available;
};
struct boot_framebuffer {
    volatile uint8_t *address;
    uint64_t width, height, pitch;
    uint16_t bpp;
    uint8_t memory_model;
    uint8_t red_mask_size, red_mask_shift;
    uint8_t green_mask_size, green_mask_shift;
    uint8_t blue_mask_size, blue_mask_shift;
};
struct boot_resource {
    const char *name;
    uint64_t start;
    uint64_t end;
};
struct boot_module { const uint8_t *data; uint64_t size; const char *name; };
typedef bool (*boot_resource_callback)(const struct boot_resource *resource, void *context);
bool boot_collect_info(struct anix_boot_info *out);
bool boot_framebuffer_get(struct boot_framebuffer *out);
bool boot_visit_resources(boot_resource_callback callback, void *context);
void *boot_rsdp_address(void);
bool boot_find_module(const char *name, struct boot_module *out);
bool boot_cache_modules(void);
#endif
