#include <anix/block.h>
#include <anix/string.h>

#define MBR_ENTRY_COUNT 4u
#define MBR_ENTRY_OFFSET 446u
#define PARTITION_DEVICE_LIMIT 4u

struct partition_context {
    struct block_device *parent;
    uint64_t first_lba;
};

static struct block_device partition_devices[PARTITION_DEVICE_LIMIT];
static struct partition_context partition_contexts[PARTITION_DEVICE_LIMIT];
static char partition_names[PARTITION_DEVICE_LIMIT][8] = {"ata0p1", "ata0p2", "ata0p3", "ata0p4"};
static size_t partition_count;

static uint32_t read_le32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
        ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static bool partition_read(struct block_device *device, uint64_t lba, uint32_t count, void *buffer) {
    struct partition_context *context = device->context;
    return context && block_read(context->parent, context->first_lba + lba, count, buffer);
}

static bool partition_write(struct block_device *device, uint64_t lba, uint32_t count, const void *buffer) {
    struct partition_context *context = device->context;
    return context && block_write(context->parent, context->first_lba + lba, count, buffer);
}

static bool partition_flush(struct block_device *device) {
    struct partition_context *context = device->context;
    return context && block_flush(context->parent);
}

size_t block_scan_mbr(struct block_device *device) {
    if (!device || partition_count || device->sector_count < 2u) return 0;
    uint8_t sector[ANIX_BLOCK_SECTOR_SIZE];
    if (!block_read(device, 0, 1u, sector) || sector[510] != 0x55u || sector[511] != 0xaau) return 0;

    uint32_t starts[MBR_ENTRY_COUNT] = {0};
    uint32_t lengths[MBR_ENTRY_COUNT] = {0};
    for (size_t index = 0; index < MBR_ENTRY_COUNT; ++index) {
        const uint8_t *entry = sector + MBR_ENTRY_OFFSET + index * 16u;
        const uint8_t type = entry[4];
        const uint32_t first = read_le32(entry + 8u);
        const uint32_t length = read_le32(entry + 12u);
        if (!type && !first && !length) continue;
        if (!type || !first || !length || (uint64_t)first >= device->sector_count ||
            (uint64_t)length > device->sector_count - first) return 0;
        starts[index] = first;
        lengths[index] = length;
    }

    for (size_t i = 0; i < MBR_ENTRY_COUNT; ++i) {
        if (!lengths[i]) continue;
        const uint64_t end_i = (uint64_t)starts[i] + lengths[i];
        for (size_t j = i + 1u; j < MBR_ENTRY_COUNT; ++j) {
            if (!lengths[j]) continue;
            const uint64_t end_j = (uint64_t)starts[j] + lengths[j];
            if ((uint64_t)starts[i] < end_j && (uint64_t)starts[j] < end_i) return 0;
        }
    }

    for (size_t index = 0; index < MBR_ENTRY_COUNT; ++index) {
        if (!lengths[index]) continue;
        struct block_device *partition = &partition_devices[partition_count];
        struct partition_context *context = &partition_contexts[partition_count];
        context->parent = device;
        context->first_lba = starts[index];
        partition->name = partition_names[index];
        partition->sector_count = lengths[index];
        partition->read_sectors = partition_read;
        partition->write_sectors = partition_write;
        partition->flush = partition_flush;
        partition->context = context;
        if (!block_register(partition)) break;
        ++partition_count;
    }
    return partition_count;
}
