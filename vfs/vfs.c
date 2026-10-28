#include <anix/heap.h>
#include <anix/anixfs.h>
#include <anix/string.h>
#include <anix/vfs.h>

#define CPIO_NEWC_HEADER_SIZE 110u
#define CPIO_DIRECTORY 0040000u
#define CPIO_REGULAR 0100000u
#define VFS_MAX_PATH 256u
#define VFS_MAX_COMPONENT 63u
#define VFS_MAX_FILE_SIZE (1024u * 1024u)

static struct vfs_node *filesystem_root;

static uint32_t parse_hex8(const uint8_t *text, bool *ok) {
    uint32_t value = 0;
    for (size_t i = 0; i < 8; ++i) {
        const uint8_t ch = text[i];
        uint8_t digit;
        if (ch >= '0' && ch <= '9') digit = ch - '0';
        else if (ch >= 'a' && ch <= 'f') digit = ch - 'a' + 10u;
        else if (ch >= 'A' && ch <= 'F') digit = ch - 'A' + 10u;
        else { *ok = false; return 0; }
        value = (value << 4) | digit;
    }
    return value;
}

static char *copy_name(const char *name, size_t length) {
    char *result = kmalloc(length + 1u);
    if (!result) return 0;
    kmemcpy(result, name, length);
    result[length] = '\0';
    return result;
}

static struct vfs_node *find_child(struct vfs_node *parent, const char *name, size_t length) {
    for (struct vfs_node *node = parent->children; node; node = node->next) {
        if (kstrlen(node->name) == length && kmemcmp(node->name, name, length) == 0) return node;
    }
    return 0;
}

static struct vfs_node *make_child(struct vfs_node *parent, const char *name, size_t length,
    enum vfs_node_type type, const uint8_t *data, uint64_t size) {
    struct vfs_node *node = find_child(parent, name, length);
    if (node) {
        if (node->type != type || (type == VFS_REGULAR_FILE && node->data != data)) return 0;
        return node;
    }
    node = kcalloc(1, sizeof(*node));
    if (!node) return 0;
    node->name = copy_name(name, length);
    if (!node->name) { kfree(node); return 0; }
    node->type = type;
    node->parent = parent;
    node->storage_slot = UINT32_MAX;
    node->writable = type == VFS_DIRECTORY && parent == filesystem_root &&
        length == 3u && kmemcmp(name, "tmp", 3u) == 0;
    node->data = data;
    node->size = size;
    node->capacity = (size_t)size;
    node->next = parent->children;
    parent->children = node;
    return node;
}

static bool insert_path(const char *path, size_t path_length, enum vfs_node_type type,
    const uint8_t *data, uint64_t size, bool persistent, uint32_t storage_slot) {
    struct vfs_node *parent = filesystem_root;
    size_t position = 0;
    while (position < path_length && path[position] == '/') ++position;
    if (position == path_length || path_length > 256u) return false;
    while (position < path_length) {
        const size_t component_start = position;
        while (position < path_length && path[position] != '/') ++position;
        const size_t component_length = position - component_start;
        while (position < path_length && path[position] == '/') ++position;
        if (component_length == 0 || component_length > 63u ||
            (component_length == 1u && path[component_start] == '.') ||
            (component_length == 2u && path[component_start] == '.' && path[component_start + 1u] == '.')) return false;
        const bool is_leaf = position >= path_length;
        const enum vfs_node_type node_type = is_leaf ? type : VFS_DIRECTORY;
        struct vfs_node *node = make_child(parent, path + component_start, component_length,
            node_type, is_leaf ? data : 0, is_leaf ? size : 0);
        if (!node) return false;
        if (persistent) {
            node->persistent = true;
            node->writable = true;
            if (is_leaf) node->storage_slot = storage_slot;
        }
        parent = node;
    }
    return true;
}

bool vfs_mount_initramfs(const uint8_t *archive, size_t size) {
    if (!archive || size < CPIO_NEWC_HEADER_SIZE || filesystem_root) return false;
    filesystem_root = kcalloc(1, sizeof(*filesystem_root));
    if (!filesystem_root) return false;
    filesystem_root->name = copy_name("/", 1u);
    if (!filesystem_root->name) return false;
    filesystem_root->type = VFS_DIRECTORY;
    size_t offset = 0;
    bool saw_trailer = false;
    while (offset <= size && size - offset >= CPIO_NEWC_HEADER_SIZE) {
        const uint8_t *header = archive + offset;
        if (kmemcmp(header, "070701", 6u) != 0) return false;
        bool valid = true;
        uint32_t mode = parse_hex8(header + 14u, &valid);
        uint32_t file_size = parse_hex8(header + 54u, &valid);
        uint32_t name_size = parse_hex8(header + 94u, &valid);
        if (!valid || name_size == 0 || name_size > 257u) return false;
        offset += CPIO_NEWC_HEADER_SIZE;
        if (name_size > size - offset) return false;
        const char *name = (const char *)(archive + offset);
        if (name[name_size - 1u] != '\0') return false;
        const size_t name_length = name_size - 1u;
        offset = (offset + name_size + 3u) & ~(size_t)3u;
        if (offset > size || file_size > size - offset) return false;
        const uint8_t *data = archive + offset;
        offset = (offset + file_size + 3u) & ~(size_t)3u;
        if (offset > size) return false;
        if (name_length == 10u && kmemcmp(name, "TRAILER!!!", 10u) == 0) {
            saw_trailer = true;
            break;
        }
        if (name_length == 1u && name[0] == '.') continue;
        const uint32_t file_type = mode & 0170000u;
        if (file_type == CPIO_DIRECTORY) {
            if (!insert_path(name, name_length, VFS_DIRECTORY, 0, 0, false, UINT32_MAX)) return false;
        } else if (file_type == CPIO_REGULAR) {
            if (!insert_path(name, name_length, VFS_REGULAR_FILE, data, file_size, false, UINT32_MAX)) return false;
        }
    }
    return saw_trailer;
}

bool vfs_mount_anixfs(struct block_device *device) {
    if (!filesystem_root || !device || find_child(filesystem_root, "persist", 7u) || !anixfs_mount(device)) return false;
    struct vfs_node *directory = make_child(filesystem_root, "persist", 7u, VFS_DIRECTORY, 0, 0);
    if (!directory) return false;
    directory->writable = true;
    directory->persistent = true;
    for (size_t index = 0; index < anixfs_file_count(); ++index) {
        struct anixfs_file_info info;
        if (!anixfs_file_at(index, &info)) return false;
        uint8_t *data = 0;
        if (info.type == ANIXFS_ENTRY_FILE && info.size) {
            data = kmalloc((size_t)info.size);
            if (!data || anixfs_read(info.slot, 0, data, (size_t)info.size) != info.size) {
                if (data) (void)kfree(data);
                return false;
            }
        }
        const enum vfs_node_type type = info.type == ANIXFS_ENTRY_DIRECTORY ? VFS_DIRECTORY : VFS_REGULAR_FILE;
        char full_path[VFS_MAX_PATH + 10u];
        const size_t name_length = kstrlen(info.name);
        if (name_length >= sizeof(full_path) - 9u) { if (data) (void)kfree(data); return false; }
        kmemcpy(full_path, "/persist/", 9u);
        kmemcpy(full_path + 9u, info.name, name_length + 1u);
        if (!insert_path(full_path, 9u + name_length, type, data, info.size, true, info.slot)) {
            if (data) (void)kfree(data);
            return false;
        }
        struct vfs_node *node = (struct vfs_node *)vfs_lookup(full_path);
        if (!node) return false;
        if (type == VFS_REGULAR_FILE && data) {
            node->data = data;
            node->data_owned = true;
            node->capacity = (size_t)info.size;
        }
        node->size = info.size;
    }
    return true;
}

const struct vfs_node *vfs_root(void) { return filesystem_root; }

const struct vfs_node *vfs_lookup(const char *path) {
    if (!filesystem_root || !path) return 0;
    struct vfs_node *node = filesystem_root;
    size_t position = 0;
    while (path[position] == '/') ++position;
    while (path[position]) {
        const size_t start = position;
        while (path[position] && path[position] != '/') ++position;
        const size_t length = position - start;
        while (path[position] == '/') ++position;
        if (length == 0) break;
        if (length == 1u && path[start] == '.') continue;
        if (length == 2u && path[start] == '.' && path[start + 1u] == '.') {
            if (node->parent) node = node->parent;
            continue;
        }
        if (node->type != VFS_DIRECTORY || !(node = find_child(node, path + start, length))) return 0;
    }
    return node;
}

static void destroy_node(struct vfs_node *node) {
    if (!node) return;
    if (node->persistent && node->unlinked) anixfs_release(node->storage_slot);
    if (node->data_owned && node->data) (void)kfree((void *)node->data);
    if (node->name) (void)kfree(node->name);
    (void)kfree(node);
}

bool vfs_open(struct vfs_node *node) {
    if (!node || node->unlinked || node->open_count == UINT32_MAX) return false;
    ++node->open_count;
    return true;
}

void vfs_close(struct vfs_node *node) {
    if (!node || node->open_count == 0) return;
    --node->open_count;
    if (node->unlinked && node->open_count == 0) destroy_node(node);
}

bool vfs_unlink(const char *path) {
    struct vfs_node *node = (struct vfs_node *)vfs_lookup(path);
    if (!node || node->type != VFS_REGULAR_FILE || !node->writable || !node->parent) return false;
    if (node->persistent && !anixfs_unlink(node->storage_slot)) return false;
    struct vfs_node **link = &node->parent->children;
    while (*link && *link != node) link = &(*link)->next;
    if (*link != node) return false;
    *link = node->next;
    node->next = 0;
    node->parent = 0;
    node->unlinked = true;
    if (node->open_count == 0) destroy_node(node);
    return true;
}

bool vfs_rmdir(const char *path) {
    struct vfs_node *node = (struct vfs_node *)vfs_lookup(path);
    if (!node || node->type != VFS_DIRECTORY || !node->writable ||
        !node->parent || node->children || node->open_count != 0) return false;
    if (node->persistent && !anixfs_rmdir(node->storage_slot)) return false;
    struct vfs_node **link = &node->parent->children;
    while (*link && *link != node) link = &(*link)->next;
    if (*link != node) return false;
    *link = node->next;
    node->next = 0;
    node->parent = 0;
    node->unlinked = true;
    destroy_node(node);
    return true;
}

size_t vfs_read(const struct vfs_node *node, uint64_t offset, void *buffer, size_t length) {
    if (!node || node->type != VFS_REGULAR_FILE || !buffer || offset >= node->size) return 0;
    const uint64_t remaining = node->size - offset;
    const size_t amount = remaining < length ? (size_t)remaining : length;
    kmemcpy(buffer, node->data + offset, amount);
    return amount;
}

static bool valid_component(const char *component, size_t length) {
    return length != 0 && length <= VFS_MAX_COMPONENT &&
        !(length == 1u && component[0] == '.') &&
        !(length == 2u && component[0] == '.' && component[1] == '.');
}

struct vfs_node *vfs_create_file(const char *path) {
    if (!filesystem_root || !path) return 0;
    size_t length = 0;
    while (path[length] && length <= VFS_MAX_PATH) ++length;
    if (length == 0 || length > VFS_MAX_PATH) return 0;

    size_t start = 0;
    while (start < length && path[start] == '/') ++start;
    if (start == length) return 0;
    size_t position = start;
    struct vfs_node *parent = filesystem_root;
    while (position < length) {
        const size_t component_start = position;
        while (position < length && path[position] != '/') ++position;
        const size_t component_length = position - component_start;
        if (!valid_component(path + component_start, component_length)) return 0;
        size_t next = position;
        while (next < length && path[next] == '/') ++next;
        if (next == length) {
            if (next != position) return 0; /* a file path cannot end in '/' */
            if (!parent->writable) return 0;
            if (find_child(parent, path + component_start, component_length)) return 0;
            uint32_t storage_slot = UINT32_MAX;
            if (parent->persistent) {
                char persistent_path[ANIXFS_NAME_CAPACITY];
                size_t prefix_length = 0;
                struct vfs_node *ancestor = parent;
                struct vfs_node *persist_root = find_child(filesystem_root, "persist", 7u);
                while (ancestor && ancestor != persist_root) ancestor = ancestor->parent;
                if (!persist_root || ancestor != persist_root) return 0;
                struct vfs_node *chain[ANIXFS_NAME_CAPACITY];
                size_t depth = 0;
                for (ancestor = parent; ancestor != persist_root && ancestor; ancestor = ancestor->parent) {
                    if (depth == sizeof(chain) / sizeof(chain[0])) return 0;
                    chain[depth++] = ancestor;
                }
                while (depth) {
                    const char *part = chain[--depth]->name;
                    const size_t part_length = kstrlen(part);
                    if (prefix_length && prefix_length < sizeof(persistent_path)) persistent_path[prefix_length++] = '/';
                    if (part_length >= sizeof(persistent_path) - prefix_length) return 0;
                    kmemcpy(persistent_path + prefix_length, part, part_length);
                    prefix_length += part_length;
                }
                if (prefix_length) persistent_path[prefix_length++] = '/';
                if (component_length >= sizeof(persistent_path) - prefix_length) return 0;
                kmemcpy(persistent_path + prefix_length, path + component_start, component_length);
                persistent_path[prefix_length + component_length] = '\0';
                if (!anixfs_create(persistent_path, &storage_slot)) return 0;
            }
            struct vfs_node *node = kcalloc(1, sizeof(*node));
            if (!node) {
                if (parent->persistent) { (void)anixfs_unlink(storage_slot); anixfs_release(storage_slot); }
                return 0;
            }
            node->name = copy_name(path + component_start, component_length);
            if (!node->name) {
                (void)kfree(node);
                if (parent->persistent) { (void)anixfs_unlink(storage_slot); anixfs_release(storage_slot); }
                return 0;
            }
            node->type = VFS_REGULAR_FILE;
            node->writable = true;
            node->persistent = parent->persistent;
            node->storage_slot = storage_slot;
            node->parent = parent;
            node->next = parent->children;
            parent->children = node;
            return node;
        }
        struct vfs_node *child = find_child(parent, path + component_start, component_length);
        if (!child || child->type != VFS_DIRECTORY) return 0;
        parent = child;
        position = next;
    }
    return 0;
}

struct vfs_node *vfs_mkdir(const char *path) {
    if (!filesystem_root || !path) return 0;
    size_t length = 0;
    while (path[length] && length <= VFS_MAX_PATH) ++length;
    if (length == 0 || length > VFS_MAX_PATH) return 0;

    size_t position = 0;
    while (position < length && path[position] == '/') ++position;
    if (position == length) return 0;
    struct vfs_node *parent = filesystem_root;
    while (position < length) {
        const size_t component_start = position;
        while (position < length && path[position] != '/') ++position;
        const size_t component_length = position - component_start;
        if (!valid_component(path + component_start, component_length)) return 0;
        size_t next = position;
        while (next < length && path[next] == '/') ++next;
        if (next == length) {
            if (!parent->writable ||
                find_child(parent, path + component_start, component_length)) return 0;
            uint32_t storage_slot = UINT32_MAX;
            if (parent->persistent) {
                char persistent_path[ANIXFS_NAME_CAPACITY];
                struct vfs_node *persist_root = find_child(filesystem_root, "persist", 7u);
                struct vfs_node *chain[ANIXFS_NAME_CAPACITY];
                size_t depth = 0, path_length = 0;
                struct vfs_node *ancestor = parent;
                while (ancestor && ancestor != persist_root) {
                    if (depth == sizeof(chain) / sizeof(chain[0])) return 0;
                    chain[depth++] = ancestor;
                    ancestor = ancestor->parent;
                }
                if (!persist_root || ancestor != persist_root) return 0;
                while (depth) {
                    const size_t part_length = kstrlen(chain[--depth]->name);
                    if (path_length) persistent_path[path_length++] = '/';
                    if (part_length >= sizeof(persistent_path) - path_length) return 0;
                    kmemcpy(persistent_path + path_length, chain[depth]->name, part_length);
                    path_length += part_length;
                }
                if (path_length) persistent_path[path_length++] = '/';
                if (component_length >= sizeof(persistent_path) - path_length) return 0;
                kmemcpy(persistent_path + path_length, path + component_start, component_length);
                persistent_path[path_length + component_length] = '\0';
                if (!anixfs_mkdir(persistent_path, &storage_slot)) return 0;
            }
            struct vfs_node *directory = kcalloc(1, sizeof(*directory));
            if (!directory) { if (storage_slot != UINT32_MAX) { (void)anixfs_rmdir(storage_slot); anixfs_release(storage_slot); } return 0; }
            directory->name = copy_name(path + component_start, component_length);
            if (!directory->name) {
                (void)kfree(directory);
                if (storage_slot != UINT32_MAX) { (void)anixfs_rmdir(storage_slot); anixfs_release(storage_slot); }
                return 0;
            }
            directory->type = VFS_DIRECTORY;
            directory->writable = true;
            directory->persistent = parent->persistent;
            directory->parent = parent;
            directory->storage_slot = storage_slot;
            directory->next = parent->children;
            parent->children = directory;
            return directory;
        }
        struct vfs_node *child = find_child(parent, path + component_start, component_length);
        if (!child || child->type != VFS_DIRECTORY) return 0;
        parent = child;
        position = next;
    }
    return 0;
}

bool vfs_truncate_empty(struct vfs_node *node) {
    if (!node || node->type != VFS_REGULAR_FILE || !node->writable || node->unlinked) return false;
    if (node->persistent && !anixfs_truncate_empty(node->storage_slot)) return false;
    node->size = 0;
    return true;
}

size_t vfs_write(struct vfs_node *node, uint64_t offset, const void *buffer, size_t length) {
    if (!node || node->type != VFS_REGULAR_FILE || !node->writable || (!buffer && length) ||
        offset > node->size || offset > VFS_MAX_FILE_SIZE || length > VFS_MAX_FILE_SIZE - (size_t)offset) return 0;
    if (length == 0) return 0;
    const size_t end = (size_t)offset + length;
    if (end > node->capacity) {
        size_t capacity = node->capacity ? node->capacity : 64u;
        while (capacity < end) {
            if (capacity > VFS_MAX_FILE_SIZE / 2u) { capacity = VFS_MAX_FILE_SIZE; break; }
            capacity *= 2u;
        }
        uint8_t *data;
        if (node->data_owned) data = krealloc((void *)node->data, capacity);
        else {
            data = kmalloc(capacity);
            if (data && node->size) kmemcpy(data, node->data, (size_t)node->size);
        }
        if (!data) return 0;
        node->data = data;
        node->capacity = capacity;
        node->data_owned = true;
    } else if (!node->data_owned && node->capacity == 0) {
        return 0;
    }
    if (node->persistent && !node->unlinked &&
        anixfs_write(node->storage_slot, offset, buffer, length) != length) return 0;
    if ((size_t)offset > node->size) kmemset((void *)(uintptr_t)(node->data + node->size), 0,
        (size_t)offset - (size_t)node->size);
    kmemcpy((void *)(uintptr_t)(node->data + offset), buffer, length);
    if (end > node->size) node->size = end;
    return length;
}

bool vfs_list(const struct vfs_node *node, vfs_list_callback callback, void *context) {
    if (!node || node->type != VFS_DIRECTORY || !callback) return false;
    for (const struct vfs_node *child = node->children; child; child = child->next)
        if (!callback(child, context)) return false;
    return true;
}
