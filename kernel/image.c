#include <anix/kernel_image.h>
#include <anix/pmm.h>
#include <anix/live_ranges.h>
#include <anix/boot.h>
#include <anix/serial.h>
#include <anix/string.h>
#include <anix/vmm.h>
extern char __text_start[], __text_end[], __rodata_start[], __rodata_end[], __data_start[], __kernel_end[];
size_t kernel_image_sections(struct kernel_section *out, size_t capacity) {
    const struct kernel_section sections[] = {
        { ".text", (uint64_t)__text_start, (uint64_t)__text_end, VMM_PRESENT, VMM_WRITABLE | VMM_NO_EXECUTE },
        { ".rodata", (uint64_t)__rodata_start, (uint64_t)__rodata_end, VMM_PRESENT, VMM_WRITABLE },
        { ".data/.bss", (uint64_t)__data_start, (uint64_t)__kernel_end, VMM_PRESENT | VMM_WRITABLE, 0 }
    };
    size_t count = sizeof(sections) / sizeof(sections[0]);
    if (out && capacity) {
        size_t copy = capacity < count ? capacity : count;
        for (size_t i = 0; i < copy; ++i) out[i] = sections[i];
    }
    return count;
}
bool kernel_image_validate_mappings(uint64_t active_root_phys) {
    struct vmm_space active = { .root_phys = active_root_phys };
    struct kernel_section sections[3];
    size_t count = kernel_image_sections(sections, 3);
    for (size_t i = 0; i < count; ++i) {
        if (sections[i].end <= sections[i].start) return false;
        uint64_t first = sections[i].start & ~(uint64_t)(VMM_PAGE_SIZE - 1u);
        uint64_t last = (sections[i].end - 1u) & ~(uint64_t)(VMM_PAGE_SIZE - 1u);
        uint64_t physical, flags;
        if (!vmm_query(&active, first, &physical, &flags)) return false;
        if ((flags & sections[i].required_flags) != sections[i].required_flags || (flags & sections[i].forbidden_flags)) return false;
        if (!vmm_query(&active, last, &physical, &flags)) return false;
        if ((flags & sections[i].required_flags) != sections[i].required_flags || (flags & sections[i].forbidden_flags)) return false;
        serial_write("[mm] section "); serial_write(sections[i].name); serial_write(": ");
        serial_write_hex(sections[i].start); serial_write("-"); serial_write_hex(sections[i].end); serial_write("\n");
    }
    return true;
}

static uint64_t page_down(uint64_t value) { return value & ~(uint64_t)(VMM_PAGE_SIZE - 1u); }
static uint64_t page_up(uint64_t value) {
    if (value > UINT64_MAX - (VMM_PAGE_SIZE - 1u)) return UINT64_MAX;
    return (value + VMM_PAGE_SIZE - 1u) & ~(uint64_t)(VMM_PAGE_SIZE - 1u);
}

bool kernel_image_build_candidate(const struct vmm_space *source, struct vmm_space *candidate, bool nx_enabled) {
    if (!source || !candidate || candidate->root_phys != PMM_INVALID_PHYS) return false;
    struct kernel_section sections[3];
    size_t count = kernel_image_sections(sections, 3);
    uint64_t image_start = page_down(sections[0].start);
    uint64_t image_end = page_up(sections[count - 1u].end);
    if (!vmm_create(candidate)) return false;
    if (!vmm_clone_range(source, candidate, image_start, image_end)) { vmm_destroy(candidate); return false; }
    for (size_t i = 0; i < count; ++i) {
        uint64_t flags = 0;
        if (i == 2) flags |= VMM_WRITABLE;
        if (i != 0 && nx_enabled) flags |= VMM_NO_EXECUTE;
        for (uint64_t address = page_down(sections[i].start); address < page_up(sections[i].end); address += VMM_PAGE_SIZE) {
            if (!vmm_protect_page(candidate, address, flags)) { vmm_destroy(candidate); return false; }
        }
    }
    return true;
}

static bool range_is_mapped(const struct vmm_space *space, uint64_t start, uint64_t end) {
    uint64_t first = page_down(start);
    uint64_t limit = page_up(end);
    for (uint64_t address = first; address < limit; address += VMM_PAGE_SIZE) {
        if (!vmm_query(space, address, 0, 0)) return false;
    }
    return true;
}

bool kernel_candidate_clone_live_ranges(const struct vmm_space *source, struct vmm_space *candidate) {
    struct live_range ranges[8];
    size_t count = arch_live_ranges(ranges, sizeof(ranges) / sizeof(ranges[0]));
    if (count > sizeof(ranges) / sizeof(ranges[0])) return false;
    for (size_t i = 0; i < count; ++i) {
        uint64_t start = page_down(ranges[i].start);
        uint64_t end = page_up(ranges[i].end);
        if (range_is_mapped(candidate, start, end)) continue;
        if (!vmm_clone_range(source, candidate, start, end)) return false;
        serial_write("[mm] live range "); serial_write(ranges[i].name); serial_write(": ");
        serial_write_hex(start); serial_write("-"); serial_write_hex(end); serial_write("\n");
    }
    return true;
}

bool kernel_candidate_validate_live_ranges(const struct vmm_space *candidate) {
    struct live_range ranges[8];
    size_t count = arch_live_ranges(ranges, sizeof(ranges) / sizeof(ranges[0]));
    if (count > sizeof(ranges) / sizeof(ranges[0])) return false;
    for (size_t i = 0; i < count; ++i) {
        if (!range_is_mapped(candidate, ranges[i].start, ranges[i].end)) return false;
    }
    return true;
}
struct hhdm_clone_context {
    struct vmm_space *candidate;
};

static bool clone_hhdm_leaf(uint64_t virtual_address, uint64_t physical_address, uint64_t size, uint64_t flags, void *opaque) {
    struct hhdm_clone_context *context = opaque;
    uint64_t leaf_flags = flags & (VMM_WRITABLE | VMM_USER | VMM_WRITE_THROUGH | VMM_CACHE_DISABLE | VMM_GLOBAL | VMM_NO_EXECUTE);
    bool any_existing = false;
    for (uint64_t offset = 0; offset < size; offset += VMM_PAGE_SIZE) {
        if (vmm_query(context->candidate, virtual_address + offset, 0, 0)) { any_existing = true; break; }
    }
    if (!any_existing && size != VMM_SIZE_4K) return vmm_map_large(context->candidate, virtual_address, physical_address, size, leaf_flags);
    for (uint64_t offset = 0; offset < size; offset += VMM_PAGE_SIZE) {
        uint64_t existing;
        if (vmm_query(context->candidate, virtual_address + offset, &existing, 0)) {
            if (existing != physical_address + offset) return false;
            continue;
        }
        if (!vmm_map_page(context->candidate, virtual_address + offset, physical_address + offset, leaf_flags)) return false;
    }
    return true;
}

bool kernel_candidate_clone_hhdm(const struct vmm_space *source, struct vmm_space *candidate) {
    struct pmm_region regions[128];
    size_t count = pmm_usable_regions(regions, sizeof(regions) / sizeof(regions[0]));
    if (count > sizeof(regions) / sizeof(regions[0])) return false;
    uint64_t offset = pmm_hhdm_offset();
    struct hhdm_clone_context context = { candidate };
    for (size_t i = 0; i < count; ++i) {
        if (regions[i].start > UINT64_MAX - offset || regions[i].end > UINT64_MAX - offset) return false;
        uint64_t start = regions[i].start + offset;
        uint64_t end = regions[i].end + offset;
        if (!vmm_walk(source, start, end, clone_hhdm_leaf, &context)) return false;
    }
    return true;
}

bool kernel_candidate_validate_hhdm(const struct vmm_space *candidate) {
    struct pmm_region regions[128];
    size_t count = pmm_usable_regions(regions, sizeof(regions) / sizeof(regions[0]));
    if (count > sizeof(regions) / sizeof(regions[0])) return false;
    uint64_t offset = pmm_hhdm_offset();
    for (size_t i = 0; i < count; ++i) {
        uint64_t start = regions[i].start + offset;
        uint64_t end = regions[i].end + offset;
        uint64_t physical, flags;
        if (!vmm_query(candidate, start, &physical, &flags) || physical != regions[i].start) return false;
        if (!vmm_query(candidate, end - 1u, &physical, &flags) || physical != regions[i].end - 1u) return false;
        if (!(flags & VMM_WRITABLE)) return false;
    }
    return true;
}
struct boot_clone_context {
    const struct vmm_space *source;
    struct vmm_space *candidate;
    bool validate_only;
};

static bool process_boot_resource(const struct boot_resource *resource, void *opaque) {
    struct boot_clone_context *context = opaque;
    uint64_t start = page_down(resource->start);
    uint64_t end = page_up(resource->end);
    if (end == UINT64_MAX || end <= start) {
        serial_write("[boot] invalid resource range: "); serial_write(resource->name); serial_write("\n");
        return false;
    }
    if (context->validate_only) return range_is_mapped(context->candidate, start, end);
    if (range_is_mapped(context->candidate, start, end)) return true;
    if (kmemcmp(resource->name, "framebuffer-memory", sizeof("framebuffer-memory")) == 0) {
        uint64_t physical_start, physical_last;
        bool source_mapping = vmm_query(context->source, start, &physical_start, 0) &&
            vmm_query(context->source, end - 1u, &physical_last, 0) &&
            physical_last >= physical_start && physical_last - physical_start == end - start - 1u;
        if (!source_mapping) {
            uint64_t hhdm = pmm_hhdm_offset();
            if (start < hhdm) return false;
            physical_start = start - hhdm;
        }
        if (!vmm_map_range(context->candidate, start, physical_start,
                (end - start) / VMM_PAGE_SIZE,
                VMM_WRITABLE | VMM_NO_EXECUTE | VMM_CACHE_DISABLE)) {
            serial_write("[boot] failed to map framebuffer memory\n");
            return false;
        }
        serial_write("[boot] mapped framebuffer memory with cache disabled: ");
        serial_write_hex(start); serial_write("-"); serial_write_hex(end); serial_write("\n");
        return true;
    }
    if (!vmm_clone_range(context->source, context->candidate, start, end)) {
        serial_write("[boot] failed to preserve "); serial_write(resource->name); serial_write(": ");
        serial_write_hex(start); serial_write("-"); serial_write_hex(end); serial_write("\n");
        return false;
    }
    serial_write("[boot] preserved "); serial_write(resource->name); serial_write(": ");
    serial_write_hex(start); serial_write("-"); serial_write_hex(end); serial_write("\n");
    return true;
}

bool kernel_candidate_clone_boot_resources(const struct vmm_space *source, struct vmm_space *candidate) {
    struct boot_clone_context context = { source, candidate, false };
    return boot_visit_resources(process_boot_resource, &context);
}

bool kernel_candidate_validate_boot_resources(const struct vmm_space *candidate) {
    struct boot_clone_context context = { 0, (struct vmm_space *)candidate, true };
    return boot_visit_resources(process_boot_resource, &context);
}
