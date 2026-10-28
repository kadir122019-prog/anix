#include <anix/interrupts.h>
#include <anix/acpi.h>
#include <anix/input.h>
#include <anix/scheduler.h>
#include <anix/serial.h>
#include <anix/stat.h>
#include <anix/string.h>
#include <anix/syscall_abi.h>
#include <anix/terminal.h>
#include <anix/timer.h>
#include <anix/uaccess.h>
#include <anix/vfs.h>
#include "../userspace/internal.h"

#define user_files (current_process()->files)

struct list_context { char *buffer; size_t capacity, used; bool overflow; };
static bool append_child(const struct vfs_node *node, void *opaque) {
    struct list_context *context = opaque;
    size_t name_length = kstrlen(node->name);
    size_t extra = name_length + (node->type == VFS_DIRECTORY ? 1u : 0u) + 1u;
    if (extra > context->capacity - context->used) { context->overflow = true; return false; }
    kmemcpy(context->buffer + context->used, node->name, name_length); context->used += name_length;
    if (node->type == VFS_DIRECTORY) context->buffer[context->used++] = '/';
    context->buffer[context->used++] = '\n';
    return true;
}

static uint64_t open_user_file(const char *path, uint64_t flags) {
    const uint64_t supported = ANIX_OPEN_WRITE | ANIX_OPEN_APPEND | ANIX_OPEN_TRUNCATE;
    if (flags & ~supported) return (uint64_t)-1;
    const bool writable = (flags & ANIX_OPEN_WRITE) != 0;
    const bool append = (flags & ANIX_OPEN_APPEND) != 0;
    const bool truncate = (flags & ANIX_OPEN_TRUNCATE) != 0;
    if ((append || truncate) && !writable) return (uint64_t)-1;
    struct vfs_node *node = (struct vfs_node *)vfs_lookup(path);
    if (!node || node->type != VFS_REGULAR_FILE || (writable && !node->writable)) return (uint64_t)-1;
    size_t slot = sizeof(user_files) / sizeof(user_files[0]);
    for (size_t i = 0; i < sizeof(user_files) / sizeof(user_files[0]); ++i)
        if (!user_files[i]) { slot = i; break; }
    if (slot == sizeof(user_files) / sizeof(user_files[0])) return (uint64_t)-1;
    if (truncate && !vfs_truncate_empty(node)) return (uint64_t)-1;
    struct user_file *file = user_file_create(node, append ? node->size : 0, writable, append);
    if (!file) return (uint64_t)-1;
    user_files[slot] = file;
    return (uint64_t)(slot + 3u);
}

static uint64_t write_user_file_object(struct user_file *file, uint64_t address, uint64_t length) {
    if (!file || !file->writable || length > 4096u || !user_range(address, length, false)) return (uint64_t)-1;
    if (file->kind == USER_FILE_SERIAL) {
        uint8_t serial_buffer[128];
        for (uint64_t offset = 0; offset < length;) {
            size_t amount = (size_t)(length - offset);
            if (amount > sizeof(serial_buffer)) amount = sizeof(serial_buffer);
            uaccess_copy_from(serial_buffer, (const void *)(uintptr_t)(address + offset), amount);
            serial_write_n((const char *)serial_buffer, amount);
            offset += amount;
        }
        return length;
    }
    if (file->kind == USER_FILE_PIPE_WRITE) {
        uint8_t buffer[256];
        uint64_t total = 0;
        while (total < length) {
            size_t amount = (size_t)(length - total);
            if (amount > sizeof(buffer)) amount = sizeof(buffer);
            uaccess_copy_from(buffer, (const void *)(uintptr_t)(address + total), amount);
            const uint64_t written = user_pipe_write(file, buffer, amount);
            if (written == (uint64_t)-1) return total ? total : (uint64_t)-1;
            total += written;
            if (written != amount) break;
        }
        return total;
    }
    if (file->kind != USER_FILE_NODE || !file->node) return (uint64_t)-1;
    uint8_t buffer[512];
    uint64_t total = 0;
    if (file->append) file->offset = file->node->size;
    while (total < length) {
        size_t amount = (size_t)(length - total);
        if (amount > sizeof(buffer)) amount = sizeof(buffer);
        uaccess_copy_from(buffer, (const void *)(uintptr_t)(address + total), amount);
        const size_t written = vfs_write(file->node, file->offset, buffer, amount);
        if (written == 0) break;
        total += written;
        file->offset += written;
        if (written != amount) break;
    }
    return total || length == 0 ? total : (uint64_t)-1;
}

static uint64_t write_user_file(struct user_process *process, uint64_t descriptor,
                                uint64_t address, uint64_t length) {
    if (!process || descriptor < 3u || descriptor >= 3u + sizeof(process->files) / sizeof(process->files[0]))
        return (uint64_t)-1;
    return write_user_file_object(process->files[descriptor - 3u], address, length);
}

static struct user_file *descriptor_file(struct user_process *process, uint64_t descriptor,
                                         bool *created_standard) {
    if (created_standard) *created_standard = false;
    if (!process || descriptor >= 3u + sizeof(process->files) / sizeof(process->files[0])) return 0;
    if (descriptor < 3u) {
        if (process->standard_state[descriptor] == 2u) return 0;
        if (process->standard_state[descriptor] == 1u) return process->standard_files[descriptor];
        struct user_file *file = user_file_create_standard(descriptor);
        if (file && created_standard) *created_standard = true;
        return file;
    }
    return process->files[descriptor - 3u];
}

bool syscall_interrupt(struct interrupt_frame *frame) {
    if (!frame || (frame->cs & 3u) != 3u) return false;
    struct user_process *process = current_process();
    if (!process) { frame->rax = (uint64_t)-1; return false; }
    const uint64_t syscall_number = frame->rax;
    uint64_t result = (uint64_t)-1;
    switch (syscall_number) {
    case ANIX_SYS_WRITE: /* write(fd, buffer, length) */
        if (frame->rdi < 3u) {
            if (process->standard_state[frame->rdi] == 1u)
                result = write_user_file_object(process->standard_files[frame->rdi], frame->rsi, frame->rdx);
            else if (process->standard_state[frame->rdi] == 0u && (frame->rdi == 1u || frame->rdi == 2u) &&
                     frame->rdx <= 4096u && user_range(frame->rsi, frame->rdx, false)) {
                char buffer[128];
                for (uint64_t offset = 0; offset < frame->rdx;) {
                    size_t amount = (size_t)(frame->rdx - offset);
                    if (amount > sizeof(buffer)) amount = sizeof(buffer);
                    uaccess_copy_from(buffer, (const void *)(uintptr_t)(frame->rsi + offset), amount);
                    serial_write_n(buffer, amount);
                    offset += amount;
                }
                result = frame->rdx;
            }
        } else result = write_user_file(process, frame->rdi, frame->rsi, frame->rdx);
        break;
    case ANIX_SYS_READLINE: /* read(line_buffer, capacity) */
        if (frame->rsi >= 2u && frame->rsi <= 256u && user_range(frame->rdi, frame->rsi, true)) {
            char buffer[256];
            if (terminal_read_line(buffer, (size_t)frame->rsi)) {
                result = kstrlen(buffer);
                uaccess_copy_to((void *)(uintptr_t)frame->rdi, buffer, (size_t)result + 1u);
            }
        }
        break;
    case ANIX_SYS_READ_CHAR: {
        char character;
        if (terminal_read_character(&character)) result = (uint8_t)character;
        break;
    }
    case ANIX_SYS_POWEROFF:
        if (acpi_poweroff_available() && acpi_poweroff()) result = 0;
        break;
    case ANIX_SYS_INPUT_EVENT: {
        struct input_event event;
        if (user_range(frame->rdi, sizeof(event), true) && input_event_wait(&event)) {
            uaccess_copy_to((void *)(uintptr_t)frame->rdi, &event, sizeof(event));
            result = 0;
        }
        break;
    }
    case ANIX_SYS_TICKS: result = timer_ticks(); break;
    case ANIX_SYS_GETPID: result = process->pid; break;
    case ANIX_SYS_GETPPID: result = user_process_parent_pid(process); break;
    case ANIX_SYS_GETPGID:
        result = user_process_getpgid(process, frame->rdi);
        break;
    case ANIX_SYS_SETPGID:
        if (frame->rdi <= INT64_MAX && frame->rsi <= INT64_MAX &&
            user_process_setpgid(process, frame->rdi, frame->rsi)) result = frame->rsi ? frame->rsi :
                (frame->rdi ? frame->rdi : process->pid);
        break;
    case ANIX_SYS_SLEEP_TICKS:
        if (scheduler_sleep_ticks(frame->rdi)) result = 0;
        break;
    case ANIX_SYS_EXEC: { /* execve(path) */
        char user_path[128], path[USER_PATH_MAX]; struct user_launch launch;
        if (user_string(frame->rdi, user_path, sizeof(user_path)) &&
            resolve_process_path(process, user_path, path, sizeof(path))) {
            default_launch(&launch, path);
            if (replace_process_image(process, path, &launch, frame)) result = 0;
        }
        break;
    }
    case ANIX_SYS_EXIT: process->exit_status = frame->rdi & 0xffu; frame->rax = process->exit_status; return true; /* exit(status) */
    case ANIX_SYS_OPEN: { /* open(path), read-only; retained for ABI v1 compatibility */
        char user_path[128], path[USER_PATH_MAX];
        if (user_string(frame->rdi, user_path, sizeof(user_path)) &&
            resolve_process_path(process, user_path, path, sizeof(path)))
            result = open_user_file(path, 0);
        break;
    }
    case ANIX_SYS_OPEN_MODE: { /* open_mode(path, flags) */
        char user_path[128], path[USER_PATH_MAX];
        if (user_string(frame->rdi, user_path, sizeof(user_path)) &&
            resolve_process_path(process, user_path, path, sizeof(path)))
            result = open_user_file(path, frame->rsi);
        break;
    }
    case ANIX_SYS_READ: { /* read(fd, buffer, capacity) */
        if (frame->rdx <= 4096u && user_range(frame->rsi, frame->rdx, true)) {
            if (frame->rdx == 0) result = 0;
            else {
                struct user_file *file = 0;
                if (frame->rdi < 3u) {
                    if (process->standard_state[frame->rdi] == 1u)
                        file = process->standard_files[frame->rdi];
                    else if (process->standard_state[frame->rdi] == 0u && frame->rdi == 0u) {
                        uint8_t buffer[256];
                        size_t amount = frame->rdx > sizeof(buffer) ? sizeof(buffer) : (size_t)frame->rdx;
                        result = terminal_read((char *)buffer, amount);
                        if (result) uaccess_copy_to((void *)(uintptr_t)frame->rsi, buffer, (size_t)result);
                    }
                } else if (frame->rdi < 3u + sizeof(user_files) / sizeof(user_files[0]))
                    file = user_files[frame->rdi - 3u];
                if (file && file->kind == USER_FILE_STDIN) {
                    uint8_t buffer[256];
                    size_t amount = frame->rdx > sizeof(buffer) ? sizeof(buffer) : (size_t)frame->rdx;
                    result = terminal_read((char *)buffer, amount);
                    if (result) uaccess_copy_to((void *)(uintptr_t)frame->rsi, buffer, (size_t)result);
                } else if (file && file->kind == USER_FILE_NODE && file->node) {
                    uint8_t buffer[512];
                    size_t amount = frame->rdx > sizeof(buffer) ? sizeof(buffer) : (size_t)frame->rdx;
                    result = vfs_read(file->node, file->offset, buffer, amount);
                    if (result) uaccess_copy_to((void *)(uintptr_t)frame->rsi, buffer, (size_t)result);
                    file->offset += result;
                } else if (file && file->kind == USER_FILE_PIPE_READ) {
                    uint8_t buffer[256];
                    size_t amount = frame->rdx > sizeof(buffer) ? sizeof(buffer) : (size_t)frame->rdx;
                    result = user_pipe_read(file, buffer, amount);
                    if (result && result != (uint64_t)-1)
                        uaccess_copy_to((void *)(uintptr_t)frame->rsi, buffer, (size_t)result);
                }
            }
        }
        break;
    }
    case ANIX_SYS_LIST: { /* list(path, newline-delimited buffer) */
        char user_path[128], path[USER_PATH_MAX];
        if (user_string(frame->rdi, user_path, sizeof(user_path)) &&
            resolve_process_path(process, user_path, path, sizeof(path)) &&
            frame->rdx <= 2048u && user_range(frame->rsi, frame->rdx, true)) {
            const struct vfs_node *node = vfs_lookup(path);
            char buffer[2048];
            struct list_context context = { buffer, (size_t)frame->rdx, 0, false };
            if (node && node->type == VFS_DIRECTORY && vfs_list(node, append_child, &context) && !context.overflow) {
                if (context.used) uaccess_copy_to((void *)(uintptr_t)frame->rsi, buffer, context.used);
                result = context.used;
            }
        }
        break;
    }
    case ANIX_SYS_STAT: { /* stat(path, metadata) */
        char user_path[128], path[USER_PATH_MAX];
        if (user_string(frame->rdi, user_path, sizeof(user_path)) &&
            resolve_process_path(process, user_path, path, sizeof(path)) &&
            user_range(frame->rsi, sizeof(struct anix_stat), true)) {
            const struct vfs_node *node = vfs_lookup(path);
            if (node) {
                const struct anix_stat status = {
                    .size = node->type == VFS_REGULAR_FILE ? node->size : 0,
                    .type = node->type == VFS_REGULAR_FILE ? ANIX_FILE_REGULAR : ANIX_FILE_DIRECTORY,
                    .flags = (node->writable ? ANIX_STAT_WRITABLE : 0u) |
                             (node->persistent ? ANIX_STAT_PERSISTENT : 0u),
                };
                uaccess_copy_to((void *)(uintptr_t)frame->rsi, &status, sizeof(status));
                result = 0;
            }
        }
        break;
    }
    case ANIX_SYS_PIPE: { /* pipe(int descriptors[2]) */
        if (user_range(frame->rdi, sizeof(int) * 2u, true)) {
            int descriptors[2];
            if (user_pipe_create(process, descriptors)) {
                uaccess_copy_to((void *)(uintptr_t)frame->rdi, descriptors, sizeof(descriptors));
                result = 0;
            }
        }
        break;
    }
    case ANIX_SYS_CLOSE: { /* close(fd) */
        if (frame->rdi < 3u) {
            if (process->standard_state[frame->rdi] == 1u) user_file_release(process->standard_files[frame->rdi]);
            else if (process->standard_state[frame->rdi] == 2u) break;
            process->standard_files[frame->rdi] = 0;
            process->standard_state[frame->rdi] = 2u;
            result = 0;
        } else if (frame->rdi < 3u + sizeof(user_files) / sizeof(user_files[0])) {
            struct user_file *file = user_files[frame->rdi - 3u];
            if (file) { user_file_release(file); user_files[frame->rdi - 3u] = 0; result = 0; }
        }
        break;
    }
    case ANIX_SYS_DUP: { /* dup(fd), share the underlying open-file description */
        if (frame->rdi < 3u + sizeof(user_files) / sizeof(user_files[0])) {
            size_t slot = sizeof(user_files) / sizeof(user_files[0]);
            for (size_t i = 0; i < sizeof(user_files) / sizeof(user_files[0]); ++i)
                if (!user_files[i]) { slot = i; break; }
            if (slot < sizeof(user_files) / sizeof(user_files[0])) {
                bool created_standard;
                struct user_file *file = descriptor_file(process, frame->rdi, &created_standard);
                if (file && (created_standard || user_file_retain(file))) {
                    user_files[slot] = file;
                    result = (uint64_t)(slot + 3u);
                }
            }
        }
        break;
    }
    case ANIX_SYS_DUP2: { /* dup2(source, target), atomically replace target */
        const uint64_t source = frame->rdi, target = frame->rsi;
        const uint64_t descriptor_limit = 3u + sizeof(process->files) / sizeof(process->files[0]);
        if (source < descriptor_limit && target < descriptor_limit && source == target) {
            if (source < 3u ? process->standard_state[source] != 2u :
                process->files[source - 3u] != 0) result = target;
        } else if (source < descriptor_limit && target < descriptor_limit) {
            bool created_standard;
            struct user_file *file = descriptor_file(process, source, &created_standard);
            const bool source_open = source < 3u ? process->standard_state[source] != 2u : file != 0;
            if (source_open && file) {
                if (created_standard || user_file_retain(file)) {
                    if (target < 3u) {
                        if (process->standard_state[target] == 1u)
                            user_file_release(process->standard_files[target]);
                        process->standard_files[target] = file;
                        process->standard_state[target] = 1u;
                    } else {
                        const size_t slot = (size_t)(target - 3u);
                        user_file_release(process->files[slot]);
                        process->files[slot] = file;
                    }
                    result = target;
                }
            }
        }
        break;
    }
    case ANIX_SYS_YIELD: /* yield() */
        scheduler_yield();
        result = 0;
        break;
    case ANIX_SYS_CREATE: { /* create(path), new volatile regular file */
        char user_path[128], path[USER_PATH_MAX];
        if (user_string(frame->rdi, user_path, sizeof(user_path)) &&
            resolve_process_path(process, user_path, path, sizeof(path))) {
            size_t slot = sizeof(user_files) / sizeof(user_files[0]);
            for (size_t i = 0; i < sizeof(user_files) / sizeof(user_files[0]); ++i)
                if (!user_files[i]) { slot = i; break; }
            if (slot < sizeof(user_files) / sizeof(user_files[0])) {
                struct vfs_node *node = vfs_create_file(path);
                struct user_file *file = node ? user_file_create(node, 0, true, false) : 0;
                if (file) {
                    user_files[slot] = file;
                    result = (uint64_t)(slot + 3u);
                }
            }
        }
        break;
    }
    case ANIX_SYS_WRITE_FILE: { /* write(fd, buffer, length) */
        result = write_user_file(process, frame->rdi, frame->rsi, frame->rdx);
        break;
    }
    case ANIX_SYS_SEEK: { /* seek(fd, absolute_offset), bounded to the current file */
        if (frame->rdi >= 3u && frame->rdi < 3u + sizeof(user_files) / sizeof(user_files[0])) {
            struct user_file *file = user_files[frame->rdi - 3u];
            if (file && file->node && frame->rsi <= file->node->size) {
                file->offset = frame->rsi;
                result = file->offset;
            }
        }
        break;
    }
    case ANIX_SYS_UNLINK: { /* unlink(path), volatile files only */
        char user_path[128], path[USER_PATH_MAX];
        if (user_string(frame->rdi, user_path, sizeof(user_path)) &&
            resolve_process_path(process, user_path, path, sizeof(path)) && vfs_unlink(path)) result = 0;
        break;
    }
    case ANIX_SYS_SPAWN: { /* spawn(path), start an independent user address space */
        char user_path[128], path[USER_PATH_MAX];
        if (user_string(frame->rdi, user_path, sizeof(user_path)) &&
            resolve_process_path(process, user_path, path, sizeof(path))) {
            struct user_launch launch;
            default_launch(&launch, path);
            result = spawn_process(process, path, &launch);
        }
        break;
    }
    case ANIX_SYS_EXECV: { /* execve(path, argv, envp) */
        char user_path[128], path[USER_PATH_MAX];
        struct user_launch launch;
        if (user_string(frame->rdi, user_path, sizeof(user_path)) &&
            resolve_process_path(process, user_path, path, sizeof(path)) &&
            copy_user_launch(frame->rsi, frame->rdx, &launch) &&
            replace_process_image(process, path, &launch, frame)) result = 0;
        break;
    }
    case ANIX_SYS_SPAWNV: { /* spawn(path, argv, envp) */
        char user_path[128], path[USER_PATH_MAX];
        struct user_launch launch;
        if (user_string(frame->rdi, user_path, sizeof(user_path)) &&
            resolve_process_path(process, user_path, path, sizeof(path)) &&
            copy_user_launch(frame->rsi, frame->rdx, &launch))
            result = spawn_process(process, path, &launch);
        break;
    }
    case ANIX_SYS_WAITPID: { /* waitpid(pid, status), where pid -1 selects any direct child */
        const int64_t requested_pid = (int64_t)frame->rdi;
        const uint64_t status_address = frame->rsi;
        if ((requested_pid == -1 || requested_pid > 0) &&
            (!status_address || user_range(status_address, sizeof(uint64_t), true))) {
            for (;;) {
                uintptr_t interrupt_flags;
                __asm__ volatile("pushfq; popq %0; cli" : "=r"(interrupt_flags) : : "memory", "cc");
                bool has_match;
                struct user_process *child = find_wait_child(process, requested_pid, true, &has_match);
                if (child) {
                    const uint64_t child_pid = child->pid;
                    uint64_t child_status;
                    if (!user_process_exit_status(child, &child_status)) {
                        __asm__ volatile("pushq %0; popfq" : : "r"(interrupt_flags) : "memory", "cc");
                        continue;
                    }
                    if (status_address)
                        uaccess_copy_to((void *)(uintptr_t)status_address, &child_status, sizeof(child_status));
                    __asm__ volatile("pushq %0; popfq" : : "r"(interrupt_flags) : "memory", "cc");
                    serial_write("[user] waitpid reaped pid "); serial_write_dec(child_pid);
                    serial_write(" status "); serial_write_dec(child_status); serial_write("\n");
                    discard_process_record(child);
                    result = child_pid;
                    break;
                }
                if (!has_match) {
                    __asm__ volatile("pushq %0; popfq" : : "r"(interrupt_flags) : "memory", "cc");
                    break;
                }
                wait_queue_wait(&process->child_waiters);
                __asm__ volatile("pushq %0; popfq" : : "r"(interrupt_flags) : "memory", "cc");
            }
        }
        break;
    }
    case ANIX_SYS_SBRK: { /* sbrk(increment), return previous break or -1 */
        result = user_heap_sbrk(process, (int64_t)frame->rdi);
        break;
    }
    case ANIX_SYS_WAIT: { /* wait(pid), block until the direct child exits */
        struct user_process *child = find_child(process, frame->rdi);
        if (child) {
            uint64_t child_status = 0;
            for (;;) {
                uintptr_t interrupt_flags;
                __asm__ volatile("pushfq; popq %0; cli" : "=r"(interrupt_flags) : : "memory", "cc");
                if (user_process_exit_status(child, &child_status)) {
                    __asm__ volatile("pushq %0; popfq" : : "r"(interrupt_flags) : "memory", "cc");
                    break;
                }
                /* Keep the exit check and transition to BLOCKED indivisible. */
                wait_queue_wait(&process->child_waiters);
                __asm__ volatile("pushq %0; popfq" : : "r"(interrupt_flags) : "memory", "cc");
            }
            result = child_status;
            serial_write("[user] reaped pid "); serial_write_dec(child->pid); serial_write(" status ");
            serial_write_dec(child_status); serial_write("\n");
            discard_process_record(child);
        }
        break;
    }
    case ANIX_SYS_MKDIR: { /* mkdir(path), create within volatile writable directories */
        char user_path[128], path[USER_PATH_MAX];
        if (user_string(frame->rdi, user_path, sizeof(user_path)) &&
            resolve_process_path(process, user_path, path, sizeof(path)) && vfs_mkdir(path)) result = 0;
        break;
    }
    case ANIX_SYS_CHDIR: { /* chdir(path) */
        char user_path[128], path[USER_PATH_MAX];
        if (user_string(frame->rdi, user_path, sizeof(user_path)) &&
            resolve_process_path(process, user_path, path, sizeof(path))) {
            struct vfs_node *node = (struct vfs_node *)vfs_lookup(path);
            if (node && node->type == VFS_DIRECTORY && vfs_open(node)) {
                if (process->cwd_node) vfs_close(process->cwd_node);
                process->cwd_node = node;
                const size_t length = kstrlen(path);
                kmemcpy(process->cwd, path, length + 1u);
                result = 0;
            }
        }
        break;
    }
    case ANIX_SYS_GETCWD: { /* getcwd(buffer, capacity), return path length excluding NUL */
        const size_t length = kstrlen(process->cwd);
        if (frame->rsi > length && frame->rsi <= sizeof(process->cwd) &&
            user_range(frame->rdi, frame->rsi, true)) {
            uaccess_copy_to((void *)(uintptr_t)frame->rdi, process->cwd, length + 1u);
            result = length;
        }
        break;
    }
    case ANIX_SYS_RMDIR: { /* rmdir(path), empty writable volatile directories only */
        char user_path[128], path[USER_PATH_MAX];
        if (user_string(frame->rdi, user_path, sizeof(user_path)) &&
            resolve_process_path(process, user_path, path, sizeof(path)) && vfs_rmdir(path)) result = 0;
        break;
    }
    default: break;
    }
    frame->rax = result;
    return false;
}
