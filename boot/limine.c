#include <anix/boot.h>
#include <anix/compiler.h>
#include <anix/string.h>
#include <limine.h>
ANIX_USED ANIX_SECTION(".limine_requests") static volatile uint64_t base_revision[3] = LIMINE_BASE_REVISION(2);
ANIX_USED ANIX_SECTION(".limine_requests_start") static volatile uint64_t requests_start[4] = LIMINE_REQUESTS_START_MARKER;
ANIX_USED ANIX_SECTION(".limine_requests") volatile struct limine_memmap_request anix_memmap_request = { .id = LIMINE_MEMMAP_REQUEST, .revision = 0, .response = 0 };
ANIX_USED ANIX_SECTION(".limine_requests") volatile struct limine_hhdm_request anix_hhdm_request = { .id = LIMINE_HHDM_REQUEST, .revision = 0, .response = 0 };
ANIX_USED ANIX_SECTION(".limine_requests") static volatile struct limine_framebuffer_request framebuffer_request = { .id = LIMINE_FRAMEBUFFER_REQUEST, .revision = 0, .response = 0 };
ANIX_USED ANIX_SECTION(".limine_requests") static volatile struct limine_rsdp_request rsdp_request = { .id = LIMINE_RSDP_REQUEST, .revision = 0, .response = 0 };
static struct limine_internal_module initramfs_module = { .path = "initramfs.cpio", .string = "initramfs", .flags = LIMINE_INTERNAL_MODULE_REQUIRED };
static struct limine_internal_module *internal_modules[] = { &initramfs_module };
ANIX_USED ANIX_SECTION(".limine_requests") static volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST, .revision = 1, .response = 0,
    .internal_module_count = 1, .internal_modules = internal_modules
};
static struct boot_module cached_modules[16];
static char cached_module_names[16][64];
static size_t cached_module_count;
static struct boot_framebuffer cached_framebuffer;
static bool cached_framebuffer_valid;
ANIX_USED ANIX_SECTION(".limine_requests_end") static volatile uint64_t requests_end[2] = LIMINE_REQUESTS_END_MARKER;

static void cache_framebuffer(void) {
    cached_framebuffer_valid = false;
    if (!framebuffer_request.response || framebuffer_request.response->framebuffer_count == 0u ||
        !framebuffer_request.response->framebuffers || !framebuffer_request.response->framebuffers[0]) return;
    const struct limine_framebuffer *framebuffer = framebuffer_request.response->framebuffers[0];
    if (!framebuffer->address || framebuffer->pitch == 0u || framebuffer->height == 0u ||
        framebuffer->height > UINT64_MAX / framebuffer->pitch) return;
    cached_framebuffer = (struct boot_framebuffer){
        .address = framebuffer->address,
        .width = framebuffer->width,
        .height = framebuffer->height,
        .pitch = framebuffer->pitch,
        .bpp = framebuffer->bpp,
        .memory_model = framebuffer->memory_model,
        .red_mask_size = framebuffer->red_mask_size,
        .red_mask_shift = framebuffer->red_mask_shift,
        .green_mask_size = framebuffer->green_mask_size,
        .green_mask_shift = framebuffer->green_mask_shift,
        .blue_mask_size = framebuffer->blue_mask_size,
        .blue_mask_shift = framebuffer->blue_mask_shift,
    };
    cached_framebuffer_valid = true;
}

bool boot_collect_info(struct anix_boot_info *out) {
    if (!out || !LIMINE_BASE_REVISION_SUPPORTED(base_revision) || !anix_memmap_request.response || !anix_hhdm_request.response) return false;
    cache_framebuffer();
    out->hhdm_offset = anix_hhdm_request.response->offset; out->memory_map_entries = anix_memmap_request.response->entry_count; out->usable_bytes = 0;
    for (uint64_t i=0;i<anix_memmap_request.response->entry_count;++i) { struct limine_memmap_entry *e=anix_memmap_request.response->entries[i]; if(e && e->type==LIMINE_MEMMAP_USABLE) out->usable_bytes += e->length; }
    out->framebuffer_available = cached_framebuffer_valid;
    return true;
}
bool boot_framebuffer_get(struct boot_framebuffer *out) {
    if (!out || !cached_framebuffer_valid) return false;
    *out = cached_framebuffer;
    return true;
}
void *boot_rsdp_address(void) {
    return rsdp_request.response ? rsdp_request.response->address : 0;
}
bool boot_find_module(const char *name, struct boot_module *out) {
    if (!name || !out) return false;
    for (size_t i = 0; i < cached_module_count; ++i) {
        if (kmemcmp(cached_modules[i].name, name, kstrlen(name) + 1u) == 0) {
            *out = cached_modules[i];
            return out->data != 0;
        }
    }
    return false;
}
bool boot_cache_modules(void) {
    if (!module_request.response || module_request.response->module_count > 16u) return false;
    cached_module_count = 0;
    for (uint64_t i = 0; i < module_request.response->module_count; ++i) {
        struct limine_file *file = module_request.response->modules[i];
        if (!file || !file->address || !file->string) continue;
        size_t length = 0;
        while (length + 1u < sizeof(cached_module_names[0]) && file->string[length]) ++length;
        if (file->string[length] != '\0') continue;
        kmemcpy(cached_module_names[cached_module_count], file->string, length + 1u);
        cached_modules[cached_module_count] = (struct boot_module){
            .data = file->address, .size = file->size,
            .name = cached_module_names[cached_module_count]
        };
        ++cached_module_count;
    }
    return cached_module_count != 0;
}
static bool visit_object(boot_resource_callback callback, void *context, const char *name, const void *object, uint64_t size) {
    if (!object || size == 0) return true;
    uint64_t start = (uint64_t)object;
    if (size > UINT64_MAX - start) return false;
    const struct boot_resource resource = { name, start, start + size };
    return callback(&resource, context);
}

bool boot_visit_resources(boot_resource_callback callback, void *context) {
    if (!callback || !anix_memmap_request.response || !anix_hhdm_request.response) return false;
    struct limine_memmap_response *memory = anix_memmap_request.response;
    if (!visit_object(callback, context, "limine-memmap-response", memory, sizeof(*memory))) return false;
    if (memory->entry_count > UINT64_MAX / sizeof(*memory->entries)) return false;
    if (!visit_object(callback, context, "limine-memmap-array", memory->entries, memory->entry_count * sizeof(*memory->entries))) return false;
    for (uint64_t i = 0; i < memory->entry_count; ++i) {
        if (!visit_object(callback, context, "limine-memmap-entry", memory->entries[i], sizeof(*memory->entries[i]))) return false;
    }
    if (!visit_object(callback, context, "limine-hhdm-response", anix_hhdm_request.response, sizeof(*anix_hhdm_request.response))) return false;
    if (rsdp_request.response) {
        if (!visit_object(callback, context, "limine-rsdp-response", rsdp_request.response, sizeof(*rsdp_request.response))) return false;
        if (!visit_object(callback, context, "limine-rsdp", rsdp_request.response->address, 36u)) return false;
    }
    if (module_request.response) {
        struct limine_module_response *modules = module_request.response;
        if (!visit_object(callback, context, "limine-module-response", modules, sizeof(*modules))) return false;
        if (modules->module_count > 64u || modules->module_count > UINT64_MAX / sizeof(*modules->modules)) return false;
        if (!visit_object(callback, context, "limine-module-array", modules->modules, modules->module_count * sizeof(*modules->modules))) return false;
        for (uint64_t i = 0; i < modules->module_count; ++i) {
            struct limine_file *file = modules->modules[i];
            if (!file || !visit_object(callback, context, "limine-module-file", file, sizeof(*file)) ||
                !visit_object(callback, context, "limine-module-data", file->address, file->size)) return false;
        }
    }
    if (!framebuffer_request.response) return true;
    struct limine_framebuffer_response *response = framebuffer_request.response;
    if (!visit_object(callback, context, "limine-framebuffer-response", response, sizeof(*response))) return false;
    if (response->framebuffer_count > UINT64_MAX / sizeof(*response->framebuffers)) return false;
    if (!visit_object(callback, context, "limine-framebuffer-array", response->framebuffers, response->framebuffer_count * sizeof(*response->framebuffers))) return false;
    for (uint64_t i = 0; i < response->framebuffer_count; ++i) {
        struct limine_framebuffer *framebuffer = response->framebuffers[i];
        if (!visit_object(callback, context, "limine-framebuffer-structure", framebuffer, sizeof(*framebuffer))) return false;
        if (!framebuffer || (framebuffer->height != 0 && framebuffer->pitch == 0) || (framebuffer->pitch != 0 && framebuffer->height > UINT64_MAX / framebuffer->pitch)) return false;
        if (!visit_object(callback, context, "framebuffer-memory", framebuffer->address, framebuffer->height * framebuffer->pitch)) return false;
        if (!visit_object(callback, context, "framebuffer-edid", framebuffer->edid, framebuffer->edid_size)) return false;
    }
    return true;
}
