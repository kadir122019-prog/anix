#ifndef ANIX_ANIXFS_H
#define ANIX_ANIXFS_H
#include <anix/block.h>

#define ANIXFS_SLOT_COUNT 64u
#define ANIXFS_SLOT_SECTORS 128u
#define ANIXFS_MAX_FILE_SIZE (ANIXFS_SLOT_SECTORS * ANIX_BLOCK_SECTOR_SIZE)
#define ANIXFS_NAME_CAPACITY 48u

enum anixfs_entry_type { ANIXFS_ENTRY_FILE = 1, ANIXFS_ENTRY_DIRECTORY = 2 };
struct anixfs_file_info {
    char name[ANIXFS_NAME_CAPACITY];
    uint64_t size;
    uint32_t slot;
    enum anixfs_entry_type type;
};

bool anixfs_mount(struct block_device *device);
size_t anixfs_file_count(void);
bool anixfs_file_at(size_t index, struct anixfs_file_info *out);
bool anixfs_create(const char *name, uint32_t *slot_out);
bool anixfs_mkdir(const char *path, uint32_t *slot_out);
bool anixfs_unlink(uint32_t slot);
bool anixfs_rmdir(uint32_t slot);
bool anixfs_truncate_empty(uint32_t slot);
void anixfs_release(uint32_t slot);
size_t anixfs_read(uint32_t slot, uint64_t offset, void *buffer, size_t length);
size_t anixfs_write(uint32_t slot, uint64_t offset, const void *buffer, size_t length);

#endif
