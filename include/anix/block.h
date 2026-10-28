#ifndef ANIX_BLOCK_H
#define ANIX_BLOCK_H
#include <anix/types.h>

#define ANIX_BLOCK_SECTOR_SIZE 512u

struct block_device {
    const char *name;
    uint64_t sector_count;
    bool (*read_sectors)(struct block_device *device, uint64_t lba, uint32_t count, void *buffer);
    bool (*write_sectors)(struct block_device *device, uint64_t lba, uint32_t count, const void *buffer);
    bool (*flush)(struct block_device *device);
    void *context;
};

bool block_register(struct block_device *device);
struct block_device *block_find(const char *name);
bool block_read(struct block_device *device, uint64_t lba, uint32_t count, void *buffer);
bool block_write(struct block_device *device, uint64_t lba, uint32_t count, const void *buffer);
bool block_flush(struct block_device *device);
size_t block_device_count(void);
size_t block_scan_mbr(struct block_device *device);

#endif
