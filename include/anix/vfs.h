#ifndef ANIX_VFS_H
#define ANIX_VFS_H
#include <anix/types.h>
#include <anix/block.h>

enum vfs_node_type { VFS_DIRECTORY, VFS_REGULAR_FILE };
struct vfs_node {
    char *name;
    enum vfs_node_type type;
    const uint8_t *data;
    uint64_t size;
    size_t capacity;
    bool writable;
    bool data_owned;
    bool unlinked;
    bool persistent;
    uint32_t open_count;
    uint32_t storage_slot;
    struct vfs_node *parent;
    struct vfs_node *children;
    struct vfs_node *next;
};
typedef bool (*vfs_list_callback)(const struct vfs_node *node, void *context);

bool vfs_mount_initramfs(const uint8_t *archive, size_t size);
bool vfs_mount_anixfs(struct block_device *device);
const struct vfs_node *vfs_root(void);
const struct vfs_node *vfs_lookup(const char *path);
size_t vfs_read(const struct vfs_node *node, uint64_t offset, void *buffer, size_t length);
struct vfs_node *vfs_create_file(const char *path);
struct vfs_node *vfs_mkdir(const char *path);
bool vfs_truncate_empty(struct vfs_node *node);
size_t vfs_write(struct vfs_node *node, uint64_t offset, const void *buffer, size_t length);
bool vfs_open(struct vfs_node *node);
void vfs_close(struct vfs_node *node);
bool vfs_unlink(const char *path);
bool vfs_rmdir(const char *path);
bool vfs_list(const struct vfs_node *node, vfs_list_callback callback, void *context);

#endif
