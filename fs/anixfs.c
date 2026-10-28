#include <anix/anixfs.h>
#include <anix/heap.h>
#include <anix/string.h>

#define ANIXFS_VERSION 1u
#define ANIXFS_DATA_START (1u + ANIXFS_SLOT_COUNT * 2u)
#define ANIXFS_MAX_PATH_NAME (ANIXFS_NAME_CAPACITY - 1u)
#define ANIXFS_SUPER_MAGIC "ANIXFS01"
#define ANIXFS_ENTRY_MAGIC "ANXENT01"

struct anixfs_superblock {
    char magic[8];
    uint32_t version, slot_count, slot_sectors, data_start;
    uint64_t total_sectors;
    uint32_t checksum;
    uint8_t reserved[476];
} __attribute__((packed));

struct anixfs_disk_entry {
    char magic[8];
    uint64_t sequence;
    uint32_t used, size;
    char name[ANIXFS_NAME_CAPACITY];
    uint32_t checksum;
    uint8_t reserved[436];
} __attribute__((packed));

_Static_assert(sizeof(struct anixfs_superblock) == ANIX_BLOCK_SECTOR_SIZE, "AnixFS superblock must fill one sector");
_Static_assert(sizeof(struct anixfs_disk_entry) == ANIX_BLOCK_SECTOR_SIZE, "AnixFS entry must fill one sector");

struct anixfs_slot {
    bool valid_record, busy_unlinked;
    uint8_t active_copy;
    uint64_t sequence;
    uint32_t type;
    uint32_t size;
    char name[ANIXFS_NAME_CAPACITY];
};

static struct block_device *mounted_device;
static struct anixfs_slot slots[ANIXFS_SLOT_COUNT];

static uint32_t record_checksum(const uint8_t *bytes, size_t checksum_offset) {
    uint32_t value = 2166136261u;
    for (size_t i = 0; i < ANIX_BLOCK_SECTOR_SIZE; ++i) {
        uint8_t byte = (i >= checksum_offset && i < checksum_offset + sizeof(uint32_t)) ? 0 : bytes[i];
        value = (value ^ byte) * 16777619u;
    }
    return value;
}

static bool valid_name(const char *name, size_t *length_out) {
    if (!name) return false;
    size_t length = 0;
    while (name[length] && length < ANIXFS_NAME_CAPACITY) ++length;
    if (length == 0 || length >= ANIXFS_NAME_CAPACITY || name[0] == '/' || name[length - 1u] == '/') return false;
    size_t start = 0;
    for (size_t i = 0; i <= length; ++i) {
        if (i < length && name[i] != '/') {
            if (name[i] == '\\') return false;
            continue;
        }
        const size_t component_length = i - start;
        if (component_length == 0 || component_length > 63u ||
            (component_length == 1u && name[start] == '.') ||
            (component_length == 2u && name[start] == '.' && name[start + 1u] == '.')) return false;
        start = i + 1u;
    }
    if (length_out) *length_out = length;
    return true;
}

static bool parent_directory_exists(const char *path) {
    size_t length = 0;
    while (path[length]) ++length;
    size_t slash = length;
    while (slash && path[slash - 1u] != '/') --slash;
    if (!slash) return true;
    const size_t parent_length = slash - 1u;
    for (uint32_t slot = 0; slot < ANIXFS_SLOT_COUNT; ++slot)
        if (slots[slot].type == ANIXFS_ENTRY_DIRECTORY &&
            kmemcmp(slots[slot].name, path, parent_length) == 0 && slots[slot].name[parent_length] == '\0') return true;
    return false;
}

static uint64_t entry_lba(uint32_t slot, uint8_t copy) {
    return 1u + (uint64_t)slot * 2u + copy;
}

static uint64_t data_lba(uint32_t slot) {
    return ANIXFS_DATA_START + (uint64_t)slot * ANIXFS_SLOT_SECTORS;
}

static bool record_valid(struct anixfs_disk_entry *entry) {
    if (kmemcmp(entry->magic, ANIXFS_ENTRY_MAGIC, sizeof(entry->magic)) != 0 ||
        entry->used > ANIXFS_ENTRY_DIRECTORY || entry->size > ANIXFS_MAX_FILE_SIZE ||
        entry->checksum != record_checksum((const uint8_t *)entry, offsetof(struct anixfs_disk_entry, checksum))) return false;
    if (entry->used && (!valid_name(entry->name, 0) ||
        (entry->used == ANIXFS_ENTRY_DIRECTORY && entry->size != 0))) return false;
    if (!entry->used && (entry->size != 0 || entry->name[0] != '\0')) return false;
    return true;
}

static bool commit_entry(uint32_t slot, uint32_t type, const char *name, uint32_t size) {
    if (!mounted_device || slot >= ANIXFS_SLOT_COUNT || slots[slot].sequence == UINT64_MAX) return false;
    struct anixfs_disk_entry entry = {0};
    kmemcpy(entry.magic, ANIXFS_ENTRY_MAGIC, sizeof(entry.magic));
    entry.sequence = slots[slot].sequence + 1u;
    entry.used = type;
    entry.size = type ? size : 0u;
    if (type) {
        size_t name_length;
        if (!valid_name(name, &name_length)) return false;
        kmemcpy(entry.name, name, name_length);
    }
    entry.checksum = record_checksum((const uint8_t *)&entry, offsetof(struct anixfs_disk_entry, checksum));
    const uint8_t copy = slots[slot].valid_record ? (uint8_t)(slots[slot].active_copy ^ 1u) : 0u;
    if (!block_write(mounted_device, entry_lba(slot, copy), 1u, &entry) || !block_flush(mounted_device)) return false;
    slots[slot].valid_record = true;
    slots[slot].active_copy = copy;
    slots[slot].sequence = entry.sequence;
    slots[slot].type = type;
    slots[slot].size = entry.size;
    slots[slot].name[0] = '\0';
    if (type) kmemcpy(slots[slot].name, entry.name, sizeof(slots[slot].name));
    return true;
}

bool anixfs_mount(struct block_device *device) {
    if (!device || mounted_device || device->sector_count < ANIXFS_DATA_START +
        (uint64_t)ANIXFS_SLOT_COUNT * ANIXFS_SLOT_SECTORS) return false;
    struct anixfs_superblock superblock;
    if (!block_read(device, 0, 1u, &superblock) || kmemcmp(superblock.magic, ANIXFS_SUPER_MAGIC, 8u) != 0 ||
        superblock.version != ANIXFS_VERSION || superblock.slot_count != ANIXFS_SLOT_COUNT ||
        superblock.slot_sectors != ANIXFS_SLOT_SECTORS || superblock.data_start != ANIXFS_DATA_START ||
        superblock.total_sectors > device->sector_count || superblock.total_sectors < ANIXFS_DATA_START +
            (uint64_t)ANIXFS_SLOT_COUNT * ANIXFS_SLOT_SECTORS ||
        superblock.checksum != record_checksum((const uint8_t *)&superblock, offsetof(struct anixfs_superblock, checksum))) return false;

    kmemset(slots, 0, sizeof(slots));
    for (uint32_t index = 0; index < ANIXFS_SLOT_COUNT; ++index) {
        struct anixfs_disk_entry copies[2] = {0};
        if (!block_read(device, entry_lba(index, 0), 1u, &copies[0]) ||
            !block_read(device, entry_lba(index, 1), 1u, &copies[1])) return false;
        bool valid[2] = { record_valid(&copies[0]), record_valid(&copies[1]) };
        if (!valid[0] && !valid[1]) {
            const uint8_t *first = (const uint8_t *)&copies[0];
            const uint8_t *second = (const uint8_t *)&copies[1];
            bool blank = true;
            for (size_t byte = 0; byte < ANIX_BLOCK_SECTOR_SIZE; ++byte)
                if (first[byte] != 0 || second[byte] != 0) { blank = false; break; }
            if (!blank) return false;
            slots[index].active_copy = 1u;
            continue;
        }
        uint8_t selected = valid[1] && (!valid[0] || copies[1].sequence > copies[0].sequence) ? 1u : 0u;
        struct anixfs_disk_entry *entry = &copies[selected];
        slots[index].valid_record = true;
        slots[index].active_copy = selected;
        slots[index].sequence = entry->sequence;
        slots[index].type = entry->used;
        slots[index].size = entry->size;
        if (entry->used) kmemcpy(slots[index].name, entry->name, sizeof(entry->name));
    }
    for (size_t i = 0; i < ANIXFS_SLOT_COUNT; ++i) {
        if (!slots[i].type) continue;
        for (size_t j = 0; j < i; ++j)
            if (slots[j].type && kmemcmp(slots[j].name, slots[i].name, sizeof(slots[i].name)) == 0) return false;
    }
    for (size_t i = 0; i < ANIXFS_SLOT_COUNT; ++i) {
        if (slots[i].type && !parent_directory_exists(slots[i].name)) return false;
    }
    mounted_device = device;
    return true;
}

size_t anixfs_file_count(void) {
    size_t count = 0;
    for (size_t i = 0; i < ANIXFS_SLOT_COUNT; ++i) if (slots[i].type) ++count;
    return count;
}

bool anixfs_file_at(size_t index, struct anixfs_file_info *out) {
    if (!out) return false;
    for (uint32_t slot = 0; slot < ANIXFS_SLOT_COUNT; ++slot) {
        if (!slots[slot].type) continue;
        if (index-- == 0) {
            kmemset(out, 0, sizeof(*out));
            kmemcpy(out->name, slots[slot].name, sizeof(out->name));
            out->size = slots[slot].size;
            out->slot = slot;
            out->type = (enum anixfs_entry_type)slots[slot].type;
            return true;
        }
    }
    return false;
}

static bool create_entry(const char *name, uint32_t type, uint32_t *slot_out) {
    size_t length;
    if (!mounted_device || !valid_name(name, &length) || !slot_out ||
        (type != ANIXFS_ENTRY_FILE && type != ANIXFS_ENTRY_DIRECTORY) || !parent_directory_exists(name)) return false;
    for (uint32_t slot = 0; slot < ANIXFS_SLOT_COUNT; ++slot)
        if (slots[slot].type && kmemcmp(slots[slot].name, name, length + 1u) == 0) return false;
    for (uint32_t slot = 0; slot < ANIXFS_SLOT_COUNT; ++slot) {
        if (slots[slot].type || slots[slot].busy_unlinked) continue;
        if (!commit_entry(slot, type, name, 0u)) return false;
        *slot_out = slot;
        return true;
    }
    return false;
}

bool anixfs_create(const char *name, uint32_t *slot_out) {
    return create_entry(name, ANIXFS_ENTRY_FILE, slot_out);
}

bool anixfs_mkdir(const char *path, uint32_t *slot_out) {
    return create_entry(path, ANIXFS_ENTRY_DIRECTORY, slot_out);
}

bool anixfs_unlink(uint32_t slot) {
    if (!mounted_device || slot >= ANIXFS_SLOT_COUNT || slots[slot].type != ANIXFS_ENTRY_FILE) return false;
    if (!commit_entry(slot, 0, 0, 0u)) return false;
    slots[slot].busy_unlinked = true;
    return true;
}

bool anixfs_rmdir(uint32_t slot) {
    if (!mounted_device || slot >= ANIXFS_SLOT_COUNT || slots[slot].type != ANIXFS_ENTRY_DIRECTORY) return false;
    const size_t length = kstrlen(slots[slot].name);
    for (uint32_t child = 0; child < ANIXFS_SLOT_COUNT; ++child) {
        if (child == slot || !slots[child].type) continue;
        if (kmemcmp(slots[child].name, slots[slot].name, length) == 0 && slots[child].name[length] == '/') return false;
    }
    if (!commit_entry(slot, 0, 0, 0u)) return false;
    slots[slot].busy_unlinked = true;
    return true;
}

bool anixfs_truncate_empty(uint32_t slot) {
    if (!mounted_device || slot >= ANIXFS_SLOT_COUNT || slots[slot].type != ANIXFS_ENTRY_FILE) return false;
    return commit_entry(slot, ANIXFS_ENTRY_FILE, slots[slot].name, 0u);
}

void anixfs_release(uint32_t slot) {
    if (slot < ANIXFS_SLOT_COUNT && !slots[slot].type) slots[slot].busy_unlinked = false;
}

size_t anixfs_read(uint32_t slot, uint64_t offset, void *buffer, size_t length) {
    if (!mounted_device || slot >= ANIXFS_SLOT_COUNT || slots[slot].type != ANIXFS_ENTRY_FILE || !buffer || offset >= slots[slot].size) return 0;
    const size_t remaining = slots[slot].size - (size_t)offset;
    const size_t amount = remaining < length ? remaining : length;
    uint8_t sector[ANIX_BLOCK_SECTOR_SIZE];
    size_t copied = 0;
    while (copied < amount) {
        const uint64_t position = offset + copied;
        const size_t in_sector = (size_t)(position % ANIX_BLOCK_SECTOR_SIZE);
        size_t chunk = ANIX_BLOCK_SECTOR_SIZE - in_sector;
        if (chunk > amount - copied) chunk = amount - copied;
        if (!block_read(mounted_device, data_lba(slot) + position / ANIX_BLOCK_SECTOR_SIZE, 1u, sector)) break;
        kmemcpy((uint8_t *)buffer + copied, sector + in_sector, chunk);
        copied += chunk;
    }
    return copied;
}

size_t anixfs_write(uint32_t slot, uint64_t offset, const void *buffer, size_t length) {
    if (!mounted_device || slot >= ANIXFS_SLOT_COUNT || slots[slot].type != ANIXFS_ENTRY_FILE || (!buffer && length) ||
        offset > slots[slot].size || offset > ANIXFS_MAX_FILE_SIZE ||
        length > ANIXFS_MAX_FILE_SIZE - (size_t)offset) return 0;
    if (!length) return 0;
    const size_t new_size = offset + length > slots[slot].size ? (size_t)offset + length : slots[slot].size;
    uint8_t sector[ANIX_BLOCK_SECTOR_SIZE];
    size_t copied = 0;
    while (copied < length) {
        const uint64_t position = offset + copied;
        const uint64_t sector_index = position / ANIX_BLOCK_SECTOR_SIZE;
        const size_t in_sector = (size_t)(position % ANIX_BLOCK_SECTOR_SIZE);
        size_t chunk = ANIX_BLOCK_SECTOR_SIZE - in_sector;
        if (chunk > length - copied) chunk = length - copied;
        const uint64_t sector_start = sector_index * ANIX_BLOCK_SECTOR_SIZE;
        if (in_sector != 0 || chunk != ANIX_BLOCK_SECTOR_SIZE) {
            kmemset(sector, 0, sizeof(sector));
            if (sector_start < slots[slot].size) {
                if (!block_read(mounted_device, data_lba(slot) + sector_index, 1u, sector)) break;
                if (slots[slot].size < sector_start + ANIX_BLOCK_SECTOR_SIZE)
                    kmemset(sector + (slots[slot].size - sector_start), 0,
                        (size_t)(sector_start + ANIX_BLOCK_SECTOR_SIZE - slots[slot].size));
            }
            kmemcpy(sector + in_sector, (const uint8_t *)buffer + copied, chunk);
            if (!block_write(mounted_device, data_lba(slot) + sector_index, 1u, sector)) break;
        } else if (!block_write(mounted_device, data_lba(slot) + sector_index, 1u,
                (const uint8_t *)buffer + copied)) break;
        copied += chunk;
    }
    if (copied != length || !block_flush(mounted_device)) return copied;
    if (new_size != slots[slot].size && !commit_entry(slot, ANIXFS_ENTRY_FILE, slots[slot].name, (uint32_t)new_size)) return 0;
    return length;
}
