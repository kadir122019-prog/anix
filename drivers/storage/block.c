#include <anix/block.h>
#include <anix/string.h>

#define BLOCK_DEVICE_LIMIT 8u
static struct block_device *devices[BLOCK_DEVICE_LIMIT];
static size_t device_count;

bool block_register(struct block_device *device) {
    if (!device || !device->name || !device->sector_count || !device->read_sectors || !device->write_sectors || !device->flush ||
        device_count == BLOCK_DEVICE_LIMIT || block_find(device->name)) return false;
    devices[device_count++] = device;
    return true;
}

struct block_device *block_find(const char *name) {
    if (!name) return 0;
    for (size_t i = 0; i < device_count; ++i)
        if (kmemcmp(devices[i]->name, name, kstrlen(name) + 1u) == 0) return devices[i];
    return 0;
}

bool block_read(struct block_device *device, uint64_t lba, uint32_t count, void *buffer) {
    return device && buffer && count && lba < device->sector_count &&
        (uint64_t)count <= device->sector_count - lba && device->read_sectors(device, lba, count, buffer);
}

bool block_write(struct block_device *device, uint64_t lba, uint32_t count, const void *buffer) {
    return device && buffer && count && lba < device->sector_count &&
        (uint64_t)count <= device->sector_count - lba && device->write_sectors(device, lba, count, buffer);
}

bool block_flush(struct block_device *device) { return device && device->flush(device); }

size_t block_device_count(void) { return device_count; }
