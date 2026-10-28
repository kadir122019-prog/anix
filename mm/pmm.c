#include <anix/pmm.h>
#include <anix/spinlock.h>
#include <anix/string.h>
#include <limine.h>

#define BITS_PER_BYTE 8u
#define MAX_USABLE_REGIONS 128u

struct usable_region { uint64_t start, end; };
static struct usable_region usable_regions[MAX_USABLE_REGIONS];
static size_t usable_region_count;

static uint8_t *bitmap;
static uint64_t bitmap_phys;
static uint64_t bitmap_bytes;
static uint64_t bitmap_pages;
static uint64_t frame_count;
static uint64_t total_pages;
static uint64_t free_pages;
static uint64_t next_hint;
static uint64_t hhdm;
static struct anix_spinlock pmm_lock;

static bool add_overflows(uint64_t left, uint64_t right) {
    return right > UINT64_MAX - left;
}

static uint64_t align_up_page(uint64_t value) {
    if (value > UINT64_MAX - (PMM_PAGE_SIZE - 1u)) return UINT64_MAX;
    return (value + PMM_PAGE_SIZE - 1u) & ~(uint64_t)(PMM_PAGE_SIZE - 1u);
}

static uint64_t align_down_page(uint64_t value) {
    return value & ~(uint64_t)(PMM_PAGE_SIZE - 1u);
}

static bool bit_is_set(uint64_t frame) {
    return (bitmap[frame / BITS_PER_BYTE] & (uint8_t)(1u << (frame % BITS_PER_BYTE))) != 0;
}

static void bit_set(uint64_t frame) {
    bitmap[frame / BITS_PER_BYTE] |= (uint8_t)(1u << (frame % BITS_PER_BYTE));
}

static void bit_clear(uint64_t frame) {
    bitmap[frame / BITS_PER_BYTE] &= (uint8_t)~(1u << (frame % BITS_PER_BYTE));
}

static bool usable_aligned_range(const struct limine_memmap_entry *entry, uint64_t *start, uint64_t *end) {
    if (!entry || entry->type != LIMINE_MEMMAP_USABLE || add_overflows(entry->base, entry->length)) return false;
    *start = align_up_page(entry->base);
    *end = align_down_page(entry->base + entry->length);
    return *start != UINT64_MAX && *end > *start;
}

void *pmm_phys_to_virt(uint64_t physical_address) {
    if (add_overflows(physical_address, hhdm)) return 0;
    return (void *)(uintptr_t)(physical_address + hhdm);
}

bool pmm_init(const struct limine_memmap_response *map, uint64_t hhdm_offset) {
    if (!map || !map->entries) return false;
    hhdm = hhdm_offset;
    usable_region_count = 0;
    uint64_t highest_end = 0;
    for (uint64_t i = 0; i < map->entry_count; ++i) {
        uint64_t start, end;
        if (usable_aligned_range(map->entries[i], &start, &end)) {
            if (usable_region_count >= MAX_USABLE_REGIONS) return false;
            usable_regions[usable_region_count++] = (struct usable_region){ start, end };
            if (end > highest_end) highest_end = end;
        }
    }
    if (highest_end == 0) return false;
    frame_count = highest_end / PMM_PAGE_SIZE;
    if (frame_count > UINT64_MAX - (BITS_PER_BYTE - 1u)) return false;
    bitmap_bytes = (frame_count + BITS_PER_BYTE - 1u) / BITS_PER_BYTE;
    bitmap_pages = (bitmap_bytes + PMM_PAGE_SIZE - 1u) / PMM_PAGE_SIZE;
    if (bitmap_pages == 0 || bitmap_pages > UINT64_MAX / PMM_PAGE_SIZE) return false;
    uint64_t bitmap_storage_bytes = bitmap_pages * PMM_PAGE_SIZE;
    bitmap_phys = PMM_INVALID_PHYS;
    for (uint64_t i = 0; i < map->entry_count; ++i) {
        uint64_t start, end;
        if (!usable_aligned_range(map->entries[i], &start, &end)) continue;
        if (end - start >= bitmap_storage_bytes) { bitmap_phys = start; break; }
    }
    if (bitmap_phys == PMM_INVALID_PHYS) return false;
    bitmap = pmm_phys_to_virt(bitmap_phys);
    if (!bitmap) return false;
    kmemset(bitmap, 0xff, (size_t)bitmap_storage_bytes);
    total_pages = 0;
    free_pages = 0;
    for (uint64_t i = 0; i < map->entry_count; ++i) {
        uint64_t start, end;
        if (!usable_aligned_range(map->entries[i], &start, &end)) continue;
        for (uint64_t address = start; address < end; address += PMM_PAGE_SIZE) {
            uint64_t frame = address / PMM_PAGE_SIZE;
            if (frame >= frame_count || !bit_is_set(frame)) continue;
            bit_clear(frame);
            ++total_pages;
            ++free_pages;
        }
    }
    for (uint64_t page = 0; page < bitmap_pages; ++page) {
        uint64_t frame = bitmap_phys / PMM_PAGE_SIZE + page;
        if (frame < frame_count && !bit_is_set(frame)) { bit_set(frame); --free_pages; }
    }
    next_hint = 0;
    return free_pages != 0;
}

uint64_t pmm_alloc_phys_page(void) {
    uintptr_t lock_flags = anix_spin_lock_irqsave(&pmm_lock);
    if (!bitmap || free_pages == 0) {
        anix_spin_unlock_irqrestore(&pmm_lock, lock_flags);
        return PMM_INVALID_PHYS;
    }
    for (uint64_t pass = 0; pass < 2; ++pass) {
        uint64_t begin = pass == 0 ? next_hint : 0;
        uint64_t end = pass == 0 ? frame_count : next_hint;
        for (uint64_t frame = begin; frame < end; ++frame) {
            if (bit_is_set(frame)) continue;
            bit_set(frame);
            --free_pages;
            next_hint = frame + 1u;
            anix_spin_unlock_irqrestore(&pmm_lock, lock_flags);
            return frame * PMM_PAGE_SIZE;
        }
    }
    anix_spin_unlock_irqrestore(&pmm_lock, lock_flags);
    return PMM_INVALID_PHYS;
}

bool pmm_free_phys_page(uint64_t physical_address) {
    uintptr_t lock_flags = anix_spin_lock_irqsave(&pmm_lock);
    if (!bitmap || (physical_address & (PMM_PAGE_SIZE - 1u)) != 0) {
        anix_spin_unlock_irqrestore(&pmm_lock, lock_flags);
        return false;
    }
    bool usable = false;
    for (size_t i = 0; i < usable_region_count; ++i) {
        if (physical_address >= usable_regions[i].start && physical_address < usable_regions[i].end) { usable = true; break; }
    }
    if (!usable) { anix_spin_unlock_irqrestore(&pmm_lock, lock_flags); return false; }
    uint64_t frame = physical_address / PMM_PAGE_SIZE;
    if (frame >= frame_count || !bit_is_set(frame)) {
        anix_spin_unlock_irqrestore(&pmm_lock, lock_flags);
        return false;
    }
    uint64_t bitmap_first = bitmap_phys / PMM_PAGE_SIZE;
    if (frame >= bitmap_first && frame < bitmap_first + bitmap_pages) {
        anix_spin_unlock_irqrestore(&pmm_lock, lock_flags);
        return false;
    }
    bit_clear(frame);
    ++free_pages;
    if (frame < next_hint) next_hint = frame;
    anix_spin_unlock_irqrestore(&pmm_lock, lock_flags);
    return true;
}

static bool frame_is_usable(uint64_t physical_address) {
    for (size_t i = 0; i < usable_region_count; ++i) {
        if (physical_address >= usable_regions[i].start && physical_address < usable_regions[i].end) return true;
    }
    return false;
}

static bool normalize_range(uint64_t physical_address, uint64_t length, uint64_t *first_frame, uint64_t *end_frame) {
    if (!bitmap || length == 0 || add_overflows(physical_address, length)) return false;
    uint64_t start = align_down_page(physical_address);
    uint64_t end = align_up_page(physical_address + length);
    if (end == UINT64_MAX || end <= start) return false;
    *first_frame = start / PMM_PAGE_SIZE;
    *end_frame = end / PMM_PAGE_SIZE;
    return *end_frame <= frame_count;
}

bool pmm_reserve_range(uint64_t physical_address, uint64_t length) {
    uintptr_t lock_flags = anix_spin_lock_irqsave(&pmm_lock);
    uint64_t first, end;
    if (!normalize_range(physical_address, length, &first, &end)) {
        anix_spin_unlock_irqrestore(&pmm_lock, lock_flags);
        return false;
    }
    for (uint64_t frame = first; frame < end; ++frame) {
        uint64_t address = frame * PMM_PAGE_SIZE;
        if (!frame_is_usable(address) || bit_is_set(frame)) {
            anix_spin_unlock_irqrestore(&pmm_lock, lock_flags);
            return false;
        }
    }
    for (uint64_t frame = first; frame < end; ++frame) { bit_set(frame); --free_pages; }
    if (first < next_hint) next_hint = first;
    anix_spin_unlock_irqrestore(&pmm_lock, lock_flags);
    return true;
}

bool pmm_release_range(uint64_t physical_address, uint64_t length) {
    uintptr_t lock_flags = anix_spin_lock_irqsave(&pmm_lock);
    uint64_t first, end;
    if (!normalize_range(physical_address, length, &first, &end)) {
        anix_spin_unlock_irqrestore(&pmm_lock, lock_flags);
        return false;
    }
    uint64_t bitmap_first = bitmap_phys / PMM_PAGE_SIZE;
    uint64_t bitmap_end = bitmap_first + bitmap_pages;
    for (uint64_t frame = first; frame < end; ++frame) {
        uint64_t address = frame * PMM_PAGE_SIZE;
        if (!frame_is_usable(address) || !bit_is_set(frame) || (frame >= bitmap_first && frame < bitmap_end)) {
            anix_spin_unlock_irqrestore(&pmm_lock, lock_flags);
            return false;
        }
    }
    for (uint64_t frame = first; frame < end; ++frame) { bit_clear(frame); ++free_pages; }
    if (first < next_hint) next_hint = first;
    anix_spin_unlock_irqrestore(&pmm_lock, lock_flags);
    return true;
}

uint64_t pmm_free_page_count(void) {
    uintptr_t lock_flags = anix_spin_lock_irqsave(&pmm_lock);
    uint64_t result = free_pages;
    anix_spin_unlock_irqrestore(&pmm_lock, lock_flags);
    return result;
}
uint64_t pmm_total_page_count(void) { return total_pages; }
uint64_t pmm_bitmap_physical_address(void) { return bitmap_phys; }
uint64_t pmm_bitmap_page_count(void) { return bitmap_pages; }
void *pmm_bitmap_virtual_address(void) { return bitmap; }

uint64_t pmm_hhdm_offset(void) { return hhdm; }
size_t pmm_usable_regions(struct pmm_region *out, size_t capacity) {
    size_t copy = capacity < usable_region_count ? capacity : usable_region_count;
    for (size_t i = 0; out && i < copy; ++i) {
        out[i].start = usable_regions[i].start;
        out[i].end = usable_regions[i].end;
    }
    return usable_region_count;
}
