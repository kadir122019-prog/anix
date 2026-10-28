#include <anix/heap.h>
#include <anix/scheduler.h>
#include <anix/string.h>
#include <anix/vfs.h>
#include "internal.h"

struct user_file *user_file_create(struct vfs_node *node, uint64_t offset,
                                   bool writable, bool append) {
    if (!node || !vfs_open(node)) return 0;
    struct user_file *file = kmalloc(sizeof(*file));
    if (!file) {
        vfs_close(node);
        return 0;
    }
    *file = (struct user_file){
        .node = node,
        .offset = offset,
        .references = 1,
        .kind = USER_FILE_NODE,
        .writable = writable,
        .append = append,
    };
    return file;
}

struct user_file *user_file_create_standard(uint64_t descriptor) {
    if (descriptor > 2u) return 0;
    struct user_file *file = kmalloc(sizeof(*file));
    if (!file) return 0;
    *file = (struct user_file){
        .references = 1,
        .kind = descriptor == 0u ? USER_FILE_STDIN : USER_FILE_SERIAL,
        .writable = descriptor != 0u,
    };
    return file;
}

bool user_pipe_create(struct user_process *process, int descriptors[2]) {
    if (!process || !descriptors) return false;
    size_t slots[2] = { sizeof(process->files) / sizeof(process->files[0]),
                        sizeof(process->files) / sizeof(process->files[0]) };
    for (size_t slot = 0; slot < sizeof(process->files) / sizeof(process->files[0]); ++slot) {
        if (process->files[slot]) continue;
        if (slots[0] == sizeof(process->files) / sizeof(process->files[0])) slots[0] = slot;
        else { slots[1] = slot; break; }
    }
    if (slots[1] == sizeof(process->files) / sizeof(process->files[0])) return false;
    struct user_pipe *pipe = kmalloc(sizeof(*pipe));
    struct user_file *reader = kmalloc(sizeof(*reader));
    struct user_file *writer = kmalloc(sizeof(*writer));
    if (!pipe || !reader || !writer) {
        if (pipe) (void)kfree(pipe);
        if (reader) (void)kfree(reader);
        if (writer) (void)kfree(writer);
        return false;
    }
    kmemset(pipe, 0, sizeof(*pipe));
    pipe->readers = pipe->writers = 1u;
    wait_queue_init(&pipe->read_waiters);
    wait_queue_init(&pipe->write_waiters);
    *reader = (struct user_file){ .pipe = pipe, .references = 1u, .kind = USER_FILE_PIPE_READ };
    *writer = (struct user_file){ .pipe = pipe, .references = 1u, .kind = USER_FILE_PIPE_WRITE, .writable = true };
    process->files[slots[0]] = reader;
    process->files[slots[1]] = writer;
    descriptors[0] = (int)(slots[0] + 3u);
    descriptors[1] = (int)(slots[1] + 3u);
    return true;
}

static uintptr_t pipe_irq_disable(void) {
    uintptr_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory", "cc");
    return flags;
}
static void pipe_irq_restore(uintptr_t flags) {
    __asm__ volatile("pushq %0; popfq" : : "r"(flags) : "memory", "cc");
}

uint64_t user_pipe_read(struct user_file *file, void *buffer, size_t length) {
    if (!file || file->kind != USER_FILE_PIPE_READ || !file->pipe || (!buffer && length)) return (uint64_t)-1;
    if (!length) return 0;
    struct user_pipe *pipe = file->pipe;
    uintptr_t flags = pipe_irq_disable();
    while (!pipe->used && pipe->writers) wait_queue_wait(&pipe->read_waiters);
    if (!pipe->used) { pipe_irq_restore(flags); return 0; }
    size_t amount = length < pipe->used ? length : pipe->used;
    uint8_t *output = buffer;
    for (size_t index = 0; index < amount; ++index) {
        output[index] = pipe->bytes[pipe->head];
        pipe->head = (pipe->head + 1u) % USER_PIPE_CAPACITY;
    }
    pipe->used -= amount;
    wait_queue_wake_one(&pipe->write_waiters);
    pipe_irq_restore(flags);
    return amount;
}

uint64_t user_pipe_write(struct user_file *file, const void *buffer, size_t length) {
    if (!file || file->kind != USER_FILE_PIPE_WRITE || !file->pipe || (!buffer && length)) return (uint64_t)-1;
    if (!length) return 0;
    struct user_pipe *pipe = file->pipe;
    uintptr_t flags = pipe_irq_disable();
    while (pipe->used == USER_PIPE_CAPACITY && pipe->readers) wait_queue_wait(&pipe->write_waiters);
    if (!pipe->readers) { pipe_irq_restore(flags); return (uint64_t)-1; }
    size_t amount = USER_PIPE_CAPACITY - pipe->used;
    if (amount > length) amount = length;
    const uint8_t *input = buffer;
    size_t tail = (pipe->head + pipe->used) % USER_PIPE_CAPACITY;
    for (size_t index = 0; index < amount; ++index) {
        pipe->bytes[tail] = input[index];
        tail = (tail + 1u) % USER_PIPE_CAPACITY;
    }
    pipe->used += amount;
    wait_queue_wake_one(&pipe->read_waiters);
    pipe_irq_restore(flags);
    return amount;
}

bool user_file_retain(struct user_file *file) {
    if (!file || file->references == UINT32_MAX) return false;
    ++file->references;
    return true;
}

void user_file_release(struct user_file *file) {
    if (!file || file->references == 0) return;
    if (--file->references != 0) return;
    if (file->node) vfs_close(file->node);
    if (file->pipe) {
        struct user_pipe *pipe = file->pipe;
        uintptr_t flags = pipe_irq_disable();
        if (file->kind == USER_FILE_PIPE_READ && pipe->readers) {
            --pipe->readers;
            wait_queue_wake_all(&pipe->write_waiters);
        } else if (file->kind == USER_FILE_PIPE_WRITE && pipe->writers) {
            --pipe->writers;
            wait_queue_wake_all(&pipe->read_waiters);
        }
        const bool release_pipe = pipe->readers == 0 && pipe->writers == 0;
        pipe_irq_restore(flags);
        if (release_pipe) (void)kfree(pipe);
    }
    (void)kfree(file);
}
