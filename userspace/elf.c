#include <anix/boot.h>
#include <anix/heap.h>
#include <anix/interrupts.h>
#include <anix/kva.h>
#include <anix/uaccess.h>
#include <anix/pmm.h>
#include <anix/serial.h>
#include <anix/scheduler.h>
#include <anix/spinlock.h>
#include <anix/string.h>
#include <anix/terminal.h>
#include <anix/timer.h>
#include <anix/user.h>
#include <anix/vfs.h>
#include <anix/vmm.h>
#include "internal.h"

#define USER_STACK_TOP 0x00007fffffffe000ULL
#define USER_STACK_PAGES 8u
#define USER_HEAP_LIMIT 0x00007ffffff00000ULL
#define ELF_PT_LOAD 1u
#define ELF_PF_W 2u
#define ELF_PF_X 1u
#define USER_MAX_IMAGE_SIZE (128u * 1024u * 1024u)
#define USER_MAX_PAGES (USER_MAX_IMAGE_SIZE / VMM_PAGE_SIZE)
struct elf64_header {
    uint8_t ident[16]; uint16_t type, machine; uint32_t version; uint64_t entry, phoff, shoff;
    uint32_t flags; uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} __attribute__((packed));
struct elf64_program { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; } __attribute__((packed));
static struct user_process process_pool[USER_MAX_PROCESSES];
static struct user_process *processes;
static struct user_process *loading_process;
static struct user_process *init_process;
static struct anix_spinlock process_table_lock;
static uint64_t next_pid = 1;
extern uint8_t anix_process_group_change_allowed(uint64_t caller_pid, uint64_t target_pid,
    uint64_t target_pgid, uint64_t requested_pgid, uint8_t target_is_caller_or_child,
    uint8_t target_exited, uint8_t group_exists_in_scope);
static struct user_process *allocate_process_record(void) {
    uintptr_t lock_flags = anix_spin_lock_irqsave(&process_table_lock);
    for (size_t i = 0; i < USER_MAX_PROCESSES; ++i) if (!process_pool[i].allocated) {
        kmemset(&process_pool[i], 0, sizeof(process_pool[i]));
        process_pool[i].allocated = true;
        process_pool[i].space.root_phys = PMM_INVALID_PHYS;
        process_pool[i].cwd[0] = '/';
        wait_queue_init(&process_pool[i].child_waiters);
        anix_spin_unlock_irqrestore(&process_table_lock, lock_flags);
        return &process_pool[i];
    }
    anix_spin_unlock_irqrestore(&process_table_lock, lock_flags);
    return 0;
}
struct user_process *current_process(void) {
    return loading_process ? loading_process : (struct user_process *)scheduler_current_argument();
}
uint64_t user_process_parent_pid(const struct user_process *process) {
    if (!process) return 0;
    uintptr_t lock_flags = anix_spin_lock_irqsave(&process_table_lock);
    const uint64_t pid = process->parent ? process->parent->pid : 0;
    anix_spin_unlock_irqrestore(&process_table_lock, lock_flags);
    return pid;
}
uint64_t user_process_getpgid(const struct user_process *process, uint64_t pid) {
    if (!process) return (uint64_t)-1;
    uintptr_t lock_flags = anix_spin_lock_irqsave(&process_table_lock);
    const uint64_t target_pid = pid ? pid : process->pid;
    struct user_process *target = 0;
    for (struct user_process *candidate = processes; candidate; candidate = candidate->next) {
        if (candidate->pid == target_pid) { target = candidate; break; }
    }
    const bool in_scope = target && (target == process || target->parent == process);
    const uint64_t group_id = in_scope ? target->process_group_id : (uint64_t)-1;
    anix_spin_unlock_irqrestore(&process_table_lock, lock_flags);
    return group_id;
}
bool user_process_setpgid(struct user_process *process, uint64_t pid, uint64_t pgid) {
    if (!process) return false;
    uintptr_t lock_flags = anix_spin_lock_irqsave(&process_table_lock);
    const uint64_t target_pid = pid ? pid : process->pid;
    struct user_process *target = 0;
    for (struct user_process *candidate = processes; candidate; candidate = candidate->next) {
        if (candidate->pid == target_pid) { target = candidate; break; }
    }
    const bool in_scope = target && (target == process || target->parent == process);
    const uint64_t requested_pgid = pgid ? pgid : target_pid;
    bool group_exists_in_scope = false;
    if (target && requested_pgid != target_pid && requested_pgid != target->process_group_id) {
        for (struct user_process *candidate = processes; candidate; candidate = candidate->next) {
            if (!candidate->exited && candidate->process_group_id == requested_pgid &&
                (candidate == process || candidate->parent == process)) {
                group_exists_in_scope = true;
                break;
            }
        }
    }
    const bool allowed = target && anix_process_group_change_allowed(
        process->pid, target_pid, target->process_group_id, requested_pgid,
        in_scope ? 1u : 0u, target->exited ? 1u : 0u,
        group_exists_in_scope ? 1u : 0u) != 0;
    if (allowed) target->process_group_id = requested_pgid;
    anix_spin_unlock_irqrestore(&process_table_lock, lock_flags);
    return allowed;
}
bool user_process_exit_status(const struct user_process *process, uint64_t *status) {
    if (!process) return false;
    uintptr_t lock_flags = anix_spin_lock_irqsave(&process_table_lock);
    const bool exited = process->exited;
    if (exited && status) *status = process->exit_status;
    anix_spin_unlock_irqrestore(&process_table_lock, lock_flags);
    return exited;
}
#define user_space (current_process()->space)
#define user_pages (current_process()->pages)
#define user_page_count (current_process()->page_count)
#define user_page_capacity (current_process()->page_capacity)
#define user_files (current_process()->files)
#define user_space_ready (current_process()->space_ready)
extern void arch_enter_user(uint64_t entry, uint64_t stack, uint64_t root);
extern void arch_user_exception_exit(void);

static bool allocate_user_page(uint64_t address, uint64_t flags) {
    if (user_page_count >= USER_MAX_PAGES) return false;
    if (user_page_count == user_page_capacity) {
        size_t capacity = user_page_capacity ? user_page_capacity * 2u : 128u;
        if (capacity > USER_MAX_PAGES) capacity = USER_MAX_PAGES;
        struct owned_user_page *pages = krealloc(user_pages, capacity * sizeof(*pages));
        if (!pages) return false;
        user_pages = pages;
        user_page_capacity = capacity;
    }
    uint64_t physical = pmm_alloc_phys_page();
    if (physical == PMM_INVALID_PHYS) return false;
    if (!vmm_map_page(&user_space, address, physical, flags)) { pmm_free_phys_page(physical); return false; }
    user_pages[user_page_count++] = (struct owned_user_page){ address, physical };
    kmemset(pmm_phys_to_virt(physical), 0, VMM_PAGE_SIZE);
    return true;
}
static void release_user_pages_to(size_t mark) {
    while (user_page_count > mark) {
        const struct owned_user_page page = user_pages[--user_page_count];
        uint64_t physical = 0;
        if (vmm_unmap_page(&user_space, page.virtual_address, &physical) && physical == page.physical_address)
            (void)pmm_free_phys_page(physical);
    }
}
uint64_t user_heap_sbrk(struct user_process *process, int64_t increment) {
    if (!process) return (uint64_t)-1;
    struct user_process *prior = loading_process;
    loading_process = process;
    const uint64_t old_break = process->heap_break;
    uint64_t new_break = old_break;
    const uint64_t old_mapped_end = process->heap_mapped_end;
    bool valid = process->heap_start <= old_break && old_break <= USER_HEAP_LIMIT &&
        old_mapped_end == ((old_break + VMM_PAGE_SIZE - 1u) & ~(uint64_t)(VMM_PAGE_SIZE - 1u));
    if (valid && increment >= 0) {
        const uint64_t amount = (uint64_t)increment;
        if (amount > USER_HEAP_LIMIT - old_break) valid = false;
        else new_break = old_break + amount;
    } else if (valid) {
        const uint64_t amount = (uint64_t)(-(increment + 1)) + 1u;
        if (amount > old_break - process->heap_start) valid = false;
        else new_break = old_break - amount;
    }
    if (!valid) {
        loading_process = prior;
        return (uint64_t)-1;
    }

    const uint64_t new_mapped_end = (new_break + VMM_PAGE_SIZE - 1u) & ~(uint64_t)(VMM_PAGE_SIZE - 1u);
    bool mapped = true;
    if (new_mapped_end > old_mapped_end) {
        for (uint64_t address = old_mapped_end; address < new_mapped_end; address += VMM_PAGE_SIZE) {
            if (!allocate_user_page(address, VMM_USER | VMM_WRITABLE | VMM_NO_EXECUTE)) {
                mapped = false;
                break;
            }
        }
        if (!mapped) {
            while (user_page_count && user_pages[user_page_count - 1u].virtual_address >= old_mapped_end)
                release_user_pages_to(user_page_count - 1u);
        }
    }
    if (mapped && new_mapped_end < old_mapped_end) {
        for (uint64_t address = new_mapped_end; address < old_mapped_end; address += VMM_PAGE_SIZE) {
            size_t i = 0;
            while (i < user_page_count && user_pages[i].virtual_address != address) ++i;
            uint64_t physical = 0;
            if (i == user_page_count || !vmm_query(&user_space, address, &physical, 0) ||
                physical != user_pages[i].physical_address) {
                mapped = false;
                break;
            }
        }
        for (uint64_t address = new_mapped_end; mapped && address < old_mapped_end; address += VMM_PAGE_SIZE) {
            for (size_t i = 0; i < user_page_count; ++i) {
                if (user_pages[i].virtual_address != address) continue;
                const struct owned_user_page page = user_pages[i];
                uint64_t physical = 0;
                if (!vmm_unmap_page(&user_space, address, &physical) || physical != page.physical_address) {
                    mapped = false;
                    break;
                }
                (void)pmm_free_phys_page(physical);
                for (size_t j = i + 1u; j < user_page_count; ++j) user_pages[j - 1u] = user_pages[j];
                --user_page_count;
                break;
            }
        }
    }
    if (mapped) {
        process->heap_break = new_break;
        process->heap_mapped_end = new_mapped_end;
    }
    loading_process = prior;
    return mapped ? old_break : (uint64_t)-1;
}
static void destroy_user_space(void) {
    release_user_pages_to(0);
    vmm_destroy(&user_space);
    user_space = (struct vmm_space){ .root_phys = PMM_INVALID_PHYS };
    user_space_ready = false;
    for (size_t i = 0; i < sizeof(user_files) / sizeof(user_files[0]); ++i)
        user_file_release(user_files[i]);
    kmemset(user_files, 0, sizeof(user_files));
    struct user_process *process = current_process();
    if (process) {
        for (size_t i = 0; i < 3u; ++i) {
            user_file_release(process->standard_files[i]);
            process->standard_files[i] = 0;
            process->standard_state[i] = 2u;
        }
    }
}

static void process_release_resources(struct user_process *process) {
    if (!process) return;
    const bool running_in_process = scheduler_current_argument() == process;
    struct user_process *prior = loading_process;
    loading_process = process;
    if (process->kva_registered) {
        kva_unregister_address_space(&process->space);
        process->kva_registered = false;
    }
    if (running_in_process && process->kernel_root_phys)
        scheduler_set_current_address_space(process->kernel_root_phys);
    destroy_user_space();
    if (process->cwd_node) {
        vfs_close(process->cwd_node);
        process->cwd_node = 0;
    }
    kfree(process->pages);
    process->pages = 0;
    process->page_count = 0;
    process->page_capacity = 0;
    process->space_ready = false;
    loading_process = prior;
}

bool user_range(uint64_t address, uint64_t length, bool writable) {
    if (length == 0) return address < 0x0000800000000000ULL;
    if (address >= 0x0000800000000000ULL || length > 0x0000800000000000ULL - address) return false;
    const uint64_t end = address + length;
    for (uint64_t page = address & ~(uint64_t)(VMM_PAGE_SIZE - 1u); page < end; page += VMM_PAGE_SIZE) {
        uint64_t flags;
        if (!vmm_query(&user_space, page, 0, &flags) || !(flags & VMM_USER) || (writable && !(flags & VMM_WRITABLE))) return false;
    }
    return true;
}
static bool write_process_memory(struct user_process *process, uint64_t address,
                                 const void *source, size_t length) {
    const uint8_t *bytes = source;
    if (!process || !source || address >= 0x0000800000000000ULL ||
        length > 0x0000800000000000ULL - address) return false;
    while (length) {
        uint64_t physical;
        if (!vmm_query(&process->space, address, &physical, 0)) return false;
        size_t amount = VMM_PAGE_SIZE - (size_t)(address & (VMM_PAGE_SIZE - 1u));
        if (amount > length) amount = length;
        kmemcpy((uint8_t *)pmm_phys_to_virt(physical & VMM_PHYS_MASK) +
                    (address & (VMM_PAGE_SIZE - 1u)), bytes, amount);
        address += amount;
        bytes += amount;
        length -= amount;
    }
    return true;
}
static bool build_initial_stack(struct user_process *process, const struct user_launch *launch) {
    if (!process || !launch || launch->argc > USER_MAX_ARGUMENTS || launch->envc > USER_MAX_ARGUMENTS) return false;
    uint64_t argv_addresses[USER_MAX_ARGUMENTS], envp_addresses[USER_MAX_ARGUMENTS];
    uint64_t cursor = USER_STACK_TOP;
    for (uint64_t i = launch->envc; i > 0; --i) {
        const size_t length = kstrlen(launch->envp[i - 1u]) + 1u;
        if (!length || length > USER_MAX_ARGUMENT_LENGTH || length > cursor - (USER_STACK_TOP - USER_STACK_PAGES * VMM_PAGE_SIZE)) return false;
        cursor = (cursor - length) & ~7ULL;
        envp_addresses[i - 1u] = cursor;
        if (!write_process_memory(process, cursor, launch->envp[i - 1u], length)) return false;
    }
    for (uint64_t i = launch->argc; i > 0; --i) {
        const size_t length = kstrlen(launch->argv[i - 1u]) + 1u;
        if (!length || length > USER_MAX_ARGUMENT_LENGTH || length > cursor - (USER_STACK_TOP - USER_STACK_PAGES * VMM_PAGE_SIZE)) return false;
        cursor = (cursor - length) & ~7ULL;
        argv_addresses[i - 1u] = cursor;
        if (!write_process_memory(process, cursor, launch->argv[i - 1u], length)) return false;
    }
    cursor &= ~15ULL;
    const uint64_t words = 1u + launch->argc + 1u + launch->envc + 1u;
    const uint64_t stack_lower = USER_STACK_TOP - USER_STACK_PAGES * VMM_PAGE_SIZE;
    if (words > (cursor - stack_lower) / sizeof(uint64_t)) return false;
    const uint64_t stack_address = (cursor - words * sizeof(uint64_t)) & ~15ULL;
    if (stack_address < stack_lower) return false;
    uint64_t word_address = stack_address;
    if (!write_process_memory(process, word_address, &launch->argc, sizeof(uint64_t))) return false;
    word_address += sizeof(uint64_t);
    for (uint64_t i = 0; i < launch->argc; ++i, word_address += sizeof(uint64_t))
        if (!write_process_memory(process, word_address, &argv_addresses[i], sizeof(uint64_t))) return false;
    const uint64_t terminator = 0;
    if (!write_process_memory(process, word_address, &terminator, sizeof(terminator))) return false;
    word_address += sizeof(uint64_t);
    for (uint64_t i = 0; i < launch->envc; ++i, word_address += sizeof(uint64_t))
        if (!write_process_memory(process, word_address, &envp_addresses[i], sizeof(uint64_t))) return false;
    if (!write_process_memory(process, word_address, &terminator, sizeof(terminator))) return false;
    process->initial_stack = stack_address;
    return true;
}
bool user_string(uint64_t address, char *out, size_t capacity) {
    if (!out || !capacity) return false;
    if (address >= 0x0000800000000000ULL) return false;
    for (size_t i = 0; i < capacity; ++i) {
        if (i >= 0x0000800000000000ULL - address) return false;
        if (!user_range(address + i, 1, false)) return false;
        uaccess_copy_from(&out[i], (const void *)(uintptr_t)(address + i), 1u);
        if (!out[i]) return true;
    }
    out[capacity - 1u] = 0;
    return false;
}
static bool copy_user_vector(uint64_t user_vector, char output[USER_MAX_ARGUMENTS][USER_MAX_ARGUMENT_LENGTH],
                             uint64_t *count_out) {
    *count_out = 0;
    if (!user_vector) return true;
    for (uint64_t i = 0; i <= USER_MAX_ARGUMENTS; ++i) {
        if (user_vector > UINT64_MAX - i * sizeof(uint64_t)) return false;
        const uint64_t slot = user_vector + i * sizeof(uint64_t);
        if (!user_range(slot, sizeof(uint64_t), false)) return false;
        uint64_t string_address;
        uaccess_copy_from(&string_address, (const void *)(uintptr_t)slot, sizeof(string_address));
        if (!string_address) { *count_out = i; return true; }
        if (i == USER_MAX_ARGUMENTS) return false;
        if (!user_string(string_address, output[i], USER_MAX_ARGUMENT_LENGTH)) return false;
    }
    return false;
}
bool copy_user_launch(uint64_t user_argv, uint64_t user_envp, struct user_launch *launch) {
    if (!launch) return false;
    kmemset(launch, 0, sizeof(*launch));
    return copy_user_vector(user_argv, launch->argv, &launch->argc) &&
           copy_user_vector(user_envp, launch->envp, &launch->envc);
}
void default_launch(struct user_launch *launch, const char *path) {
    kmemset(launch, 0, sizeof(*launch));
    launch->argc = 1;
    const size_t length = kstrlen(path);
    kmemcpy(launch->argv[0], path, length + 1u);
}
bool resolve_process_path(const struct user_process *process, const char *path,
                                 char *output, size_t output_capacity) {
    if (!process || !path || !path[0] || !output || output_capacity < 2u) return false;
    char joined[USER_PATH_MAX * 2u + 2u];
    size_t length = 0;
    if (path[0] != '/') {
        const size_t cwd_length = kstrlen(process->cwd);
        if (cwd_length == 0 || cwd_length >= USER_PATH_MAX || cwd_length >= sizeof(joined)) return false;
        kmemcpy(joined, process->cwd, cwd_length);
        length = cwd_length;
        if (joined[length - 1u] != '/') joined[length++] = '/';
    }
    const size_t path_length = kstrlen(path);
    if (path_length >= sizeof(joined) - length) return false;
    kmemcpy(joined + length, path, path_length + 1u);
    length += path_length;

    size_t component_starts[USER_PATH_MAX + 1u];
    size_t depth = 0;
    size_t output_length = 1;
    size_t position = 0;
    output[0] = '/';
    while (position < length) {
        while (position < length && joined[position] == '/') ++position;
        if (position == length) break;
        const size_t component_start = position;
        while (position < length && joined[position] != '/') ++position;
        const size_t component_length = position - component_start;
        if (component_length == 1u && joined[component_start] == '.') continue;
        if (component_length == 2u && joined[component_start] == '.' && joined[component_start + 1u] == '.') {
            if (depth) output_length = component_starts[--depth];
            continue;
        }
        if (component_length > 63u || depth >= sizeof(component_starts) / sizeof(component_starts[0])) return false;
        component_starts[depth++] = output_length;
        if (output_length > 1u) {
            if (output_length + 1u >= output_capacity) return false;
            output[output_length++] = '/';
        }
        if (component_length >= output_capacity - output_length) return false;
        kmemcpy(output + output_length, joined + component_start, component_length);
        output_length += component_length;
    }
    output[output_length] = '\0';
    return true;
}
static bool load_image(const char *path, uint64_t *entry_out, uint64_t *heap_start_out) {
    const size_t page_mark = user_page_count;
    const struct vfs_node *node = vfs_lookup(path);
    if (!node || node->type != VFS_REGULAR_FILE || node->size < sizeof(struct elf64_header)) return false;
    const struct elf64_header *elf = (const struct elf64_header *)node->data;
    if (elf->ident[0] != 0x7f || elf->ident[1] != 'E' || elf->ident[2] != 'L' || elf->ident[3] != 'F' ||
        elf->ident[4] != 2 || elf->ident[5] != 1 || elf->ident[6] != 1 || elf->machine != 62 || elf->type != 2 ||
        elf->version != 1 || elf->ehsize != sizeof(*elf) || elf->phentsize != sizeof(struct elf64_program) ||
        elf->phnum == 0 || elf->phnum > 128 || elf->phoff > node->size ||
        (uint64_t)elf->phnum * elf->phentsize > node->size - elf->phoff) return false;
    const struct elf64_program *programs = (const struct elf64_program *)(node->data + elf->phoff);
    uint64_t image_end = 0;
    for (uint16_t i = 0; i < elf->phnum; ++i) {
        const struct elf64_program *program = &programs[i];
        if (program->type != ELF_PT_LOAD || program->memsz == 0) continue;
        if ((program->flags & ~7u) != 0 || ((program->flags & (ELF_PF_W | ELF_PF_X)) == (ELF_PF_W | ELF_PF_X)) ||
            program->filesz > program->memsz || program->offset > node->size || program->filesz > node->size - program->offset ||
            program->vaddr < 0x10000u || program->vaddr >= 0x0000800000000000ULL ||
            program->memsz > USER_MAX_IMAGE_SIZE || program->memsz > 0x0000800000000000ULL - program->vaddr ||
            (program->align > 1u && ((program->align & (program->align - 1u)) != 0 ||
                (program->vaddr & (program->align - 1u)) != (program->offset & (program->align - 1u))))) goto malformed;
        const uint64_t start = program->vaddr & ~(uint64_t)(VMM_PAGE_SIZE - 1u);
        const uint64_t end = (program->vaddr + program->memsz + VMM_PAGE_SIZE - 1u) & ~(uint64_t)(VMM_PAGE_SIZE - 1u);
        if (end > image_end) image_end = end;
        if (end <= start || end - start > USER_MAX_IMAGE_SIZE || user_page_count + (end - start) / VMM_PAGE_SIZE > USER_MAX_PAGES) goto malformed;
        for (uint16_t j = 0; j < i; ++j) {
            const struct elf64_program *prior = &programs[j];
            if (prior->type != ELF_PT_LOAD || prior->memsz == 0) continue;
            uint64_t prior_start = prior->vaddr & ~(uint64_t)(VMM_PAGE_SIZE - 1u);
            uint64_t prior_end = (prior->vaddr + prior->memsz + VMM_PAGE_SIZE - 1u) & ~(uint64_t)(VMM_PAGE_SIZE - 1u);
            if (start < prior_end && prior_start < end) goto malformed;
        }
        for (uint64_t address = start; address < end; address += VMM_PAGE_SIZE) {
            const uint64_t flags = VMM_USER | ((program->flags & ELF_PF_W) ? VMM_WRITABLE : 0) |
                ((program->flags & ELF_PF_X) ? 0 : VMM_NO_EXECUTE);
            if (!allocate_user_page(address, flags)) goto malformed;
        }
        for (uint64_t copied = 0; copied < program->filesz;) {
            const uint64_t virtual_address = program->vaddr + copied;
            uint64_t physical;
            if (!vmm_query(&user_space, virtual_address, &physical, 0)) goto malformed;
            const size_t in_page = (size_t)(virtual_address & (VMM_PAGE_SIZE - 1u));
            size_t amount = VMM_PAGE_SIZE - in_page;
            if (amount > program->filesz - copied) amount = (size_t)(program->filesz - copied);
            kmemcpy((uint8_t *)pmm_phys_to_virt(physical & VMM_PHYS_MASK) + in_page, node->data + program->offset + copied, amount);
            copied += amount;
        }
    }
    uint64_t entry_flags;
    if (!user_range(elf->entry, 1u, false) || !vmm_query(&user_space, elf->entry, 0, &entry_flags) ||
        !(entry_flags & VMM_USER) || (entry_flags & VMM_NO_EXECUTE)) goto malformed;
    if (image_end == 0 || image_end > USER_HEAP_LIMIT) goto malformed;
    *entry_out = elf->entry;
    *heap_start_out = image_end;
    return true;
malformed:
    release_user_pages_to(page_mark);
    return false;
}
static bool process_prepare(struct user_process *process, const char *path, bool register_kva) {
    loading_process = process;
    if (!process->launch_configured) {
        default_launch(&process->launch, path);
        process->launch_configured = true;
    }
    serial_write("[user] preparing process "); serial_write(path); serial_write("\n");
    if (!path || !vmm_create(&user_space)) goto failed;
    struct vmm_space kernel;
    if (!vmm_import_active(&kernel) || !vmm_clone_range(&kernel, &user_space, 0xffff800000000000ULL, 0xfffffffffffff000ULL)) goto failed;
    for (uint64_t i = 0; i < USER_STACK_PAGES; ++i) {
        if (!allocate_user_page(USER_STACK_TOP - (i + 1u) * VMM_PAGE_SIZE,
                VMM_USER | VMM_WRITABLE | VMM_NO_EXECUTE)) goto failed;
    }
    uint64_t entry, heap_start;
    if (!load_image(path, &entry, &heap_start)) goto failed;
    if (!build_initial_stack(process, &process->launch)) goto failed;
    process->entry = entry;
    process->heap_start = process->heap_break = process->heap_mapped_end = heap_start;
    serial_write("[user] image mapped at "); serial_write_hex(entry); serial_write("\n");
    if (register_kva) {
        if (!kva_register_address_space(&process->space)) goto failed;
        process->kva_registered = true;
    }
    process->space_ready = true;
    loading_process = 0;
    return true;
failed:
    process_release_resources(process);
    loading_process = 0;
    return false;
}

static void process_run_user(struct user_process *process) {
    loading_process = process;
    if (!process->space_ready && !process_prepare(process, process->image_path, true)) {
        uintptr_t lock_flags = anix_spin_lock_irqsave(&process_table_lock);
        process->exit_status = 127;
        anix_spin_unlock_irqrestore(&process_table_lock, lock_flags);
        return;
    }
    loading_process = 0;
    scheduler_set_current_address_space(process->space.root_phys);
    arch_enter_user(process->entry, process->initial_stack, process->space.root_phys);
    process_release_resources(process);
}

static void process_reparent_children(struct user_process *process) {
    uintptr_t lock_flags = anix_spin_lock_irqsave(&process_table_lock);
    if (init_process == process) init_process = 0;
    struct user_process *adopter = init_process;
    if (!adopter || adopter == process || adopter->exited) adopter = 0;
    for (struct user_process *child = processes; child;) {
        struct user_process *next = child->next;
        if (child->parent == process) {
            child->parent = adopter;
        }
        child = next;
    }
    anix_spin_unlock_irqrestore(&process_table_lock, lock_flags);
}

static void user_process_task(void *argument) {
    struct user_process *process = argument;
    process_run_user(process);
    process_reparent_children(process);
    uintptr_t lock_flags = anix_spin_lock_irqsave(&process_table_lock);
    process->exited = true;
    struct user_process *parent = process->parent;
    if (parent) {
        wait_queue_wake_all(&parent->child_waiters);
        anix_spin_unlock_irqrestore(&process_table_lock, lock_flags);
    }
    else {
        anix_spin_unlock_irqrestore(&process_table_lock, lock_flags);
        serial_write("[user] orphan process record reaped\n");
        discard_process_record(process);
    }
    scheduler_exit_current();
}

bool user_process_start(const char *path) {
    if (!path || scheduler_current_argument()) return false;
    serial_write("[user] allocating process state\n");
    struct user_process *process = allocate_process_record();
    if (!process) return false;
    serial_write("[user] process state allocated\n");
    process->kernel_root_phys = scheduler_kernel_address_space_root();
    size_t n = kstrlen(path);
    if (n >= sizeof(process->image_path)) { discard_process_record(process); return false; }
    kmemcpy(process->image_path, path, n + 1u);
    uintptr_t lock_flags = anix_spin_lock_irqsave(&process_table_lock);
    process->pid = next_pid++;
    process->process_group_id = process->pid;
    init_process = process;
    process->next = processes;
    processes = process;
    anix_spin_unlock_irqrestore(&process_table_lock, lock_flags);
    scheduler_set_current_argument(process);
    scheduler_set_current_user_context(&process->return_context);
    if (!process_prepare(process, path, true)) {
        lock_flags = anix_spin_lock_irqsave(&process_table_lock);
        process->exited = true;
        process->exit_status = 127;
        if (init_process == process) init_process = 0;
        anix_spin_unlock_irqrestore(&process_table_lock, lock_flags);
        scheduler_set_current_user_context(0);
        scheduler_set_current_argument(0);
        discard_process_record(process);
        return false;
    }
    serial_write("[user] entering process\n");
    process_run_user(process);
    process_reparent_children(process);
    lock_flags = anix_spin_lock_irqsave(&process_table_lock);
    process->exited = true;
    anix_spin_unlock_irqrestore(&process_table_lock, lock_flags);
    serial_write("[user] process returned to kernel scheduler\n");
    scheduler_set_current_user_context(0);
    scheduler_set_current_argument(0);
    discard_process_record(process);
    return true;
}
void user_exception_abort(uint64_t vector, uint64_t rip, uint64_t error) {
    serial_write("[user] process terminated by exception "); serial_write_dec(vector);
    serial_write(" at "); serial_write_hex(rip); serial_write(", error "); serial_write_hex(error); serial_write("\n");
    struct user_process *process = current_process();
    if (process) process->exit_status = 128u + vector;
    arch_user_exception_exit();
    __builtin_unreachable();
}
struct user_process *find_child(struct user_process *parent, uint64_t pid) {
    uintptr_t lock_flags = anix_spin_lock_irqsave(&process_table_lock);
    for (struct user_process *process = processes; process; process = process->next)
        if (process->parent == parent && process->pid == pid) {
            anix_spin_unlock_irqrestore(&process_table_lock, lock_flags);
            return process;
        }
    anix_spin_unlock_irqrestore(&process_table_lock, lock_flags);
    return 0;
}
struct user_process *find_wait_child(struct user_process *parent, int64_t pid,
                                            bool exited_only, bool *has_match) {
    uintptr_t lock_flags = anix_spin_lock_irqsave(&process_table_lock);
    *has_match = false;
    for (struct user_process *child = processes; child; child = child->next) {
        if (child->parent != parent || (pid != -1 && child->pid != (uint64_t)pid)) continue;
        *has_match = true;
        if (!exited_only || child->exited) {
            anix_spin_unlock_irqrestore(&process_table_lock, lock_flags);
            return child;
        }
    }
    anix_spin_unlock_irqrestore(&process_table_lock, lock_flags);
    return 0;
}
void discard_process_record(struct user_process *process) {
    if (!process) return;
    uintptr_t lock_flags = anix_spin_lock_irqsave(&process_table_lock);
    struct user_process **link = &processes;
    while (*link && *link != process) link = &(*link)->next;
    if (*link) *link = process->next;
    struct owned_user_page *pages = process->pages;
    kmemset(process, 0, sizeof(*process));
    anix_spin_unlock_irqrestore(&process_table_lock, lock_flags);
    kfree(pages);
}
bool replace_process_image(struct user_process *process, const char *path,
                                  const struct user_launch *launch, struct interrupt_frame *frame) {
    if (!process || !path || !launch || !frame || kstrlen(path) >= sizeof(process->image_path) ||
        launch->argc > USER_MAX_ARGUMENTS || launch->envc > USER_MAX_ARGUMENTS) return false;
    struct user_process *staging = kmalloc(sizeof(*staging));
    if (!staging) return false;
    kmemset(staging, 0, sizeof(*staging));
    staging->space.root_phys = PMM_INVALID_PHYS;
    staging->kernel_root_phys = process->kernel_root_phys;
    staging->launch = *launch;
    staging->launch_configured = true;
    if (!process_prepare(staging, path, false)) {
        kfree(staging);
        return false;
    }

    struct vmm_space old_space = process->space;
    struct owned_user_page *old_pages = process->pages;
    const size_t old_page_count = process->page_count;
    const uint64_t new_entry = staging->entry;
    const uint64_t new_stack = staging->initial_stack;
    const uint64_t new_heap_start = staging->heap_start;
    uint64_t saved_flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(saved_flags) : : "memory", "cc");
    process->space = staging->space;
    if (!kva_refresh_address_space(&process->space)) {
        process->space = old_space;
        __asm__ volatile("pushq %0; popfq" : : "r"(saved_flags) : "memory", "cc");
        process_release_resources(staging);
        kfree(staging);
        return false;
    }
    process->pages = staging->pages;
    process->page_count = staging->page_count;
    process->page_capacity = staging->page_capacity;
    process->entry = new_entry;
    process->initial_stack = new_stack;
    process->heap_start = process->heap_break = process->heap_mapped_end = new_heap_start;
    process->launch = *launch;
    process->launch_configured = true;
    const size_t path_length = kstrlen(path);
    kmemcpy(process->image_path, path, path_length + 1u);
    process->space_ready = true;
    process->kva_registered = true;
    scheduler_set_current_address_space(process->space.root_phys);
    /* Exit syscalls return through this saved kernel context after exec. */
    process->return_context.kernel_cr3 = process->space.root_phys;
    staging->space.root_phys = PMM_INVALID_PHYS;
    staging->space.owned = false;
    staging->pages = 0;
    staging->page_count = staging->page_capacity = 0;
    staging->space_ready = false;
    __asm__ volatile("pushq %0; popfq" : : "r"(saved_flags) : "memory", "cc");

    for (size_t i = 0; i < old_page_count; ++i) {
        uint64_t physical;
        if (vmm_unmap_page(&old_space, old_pages[i].virtual_address, &physical) &&
            physical == old_pages[i].physical_address) (void)pmm_free_phys_page(physical);
    }
    vmm_destroy(&old_space);
    kfree(old_pages);
    kfree(staging);
    frame->rip = new_entry;
    /* int 0x80 from ring 3 has the hardware-pushed user RSP at word 20. */
    ((uint64_t *)frame)[20] = new_stack;
    return true;
}
uint64_t spawn_process(const struct user_process *parent, const char *path,
                              const struct user_launch *launch) {
    if (!parent || !path || !launch) return (uint64_t)-1;
    struct user_process *child = allocate_process_record();
    if (!child) return (uint64_t)-1;
    child->parent = (struct user_process *)parent;
    uintptr_t lock_flags = anix_spin_lock_irqsave(&process_table_lock);
    child->pid = next_pid++;
    child->process_group_id = parent->process_group_id;
    anix_spin_unlock_irqrestore(&process_table_lock, lock_flags);
    child->kernel_root_phys = scheduler_kernel_address_space_root();
    child->launch = *launch;
    child->launch_configured = true;
    kmemcpy(child->cwd, parent->cwd, sizeof(child->cwd));
    child->cwd_node = parent->cwd_node;
    const bool cwd_ready = !child->cwd_node || vfs_open(child->cwd_node);
    if (!cwd_ready) child->cwd_node = 0;
    bool files_ready = true;
    for (size_t i = 0; i < sizeof(child->files) / sizeof(child->files[0]); ++i) {
        if (!parent->files[i]) continue;
        if (!user_file_retain(parent->files[i])) { files_ready = false; break; }
        child->files[i] = parent->files[i];
    }
    for (size_t i = 0; files_ready && i < 3u; ++i) {
        child->standard_state[i] = parent->standard_state[i];
        if (!parent->standard_files[i]) continue;
        if (!user_file_retain(parent->standard_files[i])) { files_ready = false; break; }
        child->standard_files[i] = parent->standard_files[i];
    }
    const size_t path_length = kstrlen(path);
    if (path_length < sizeof(child->image_path)) kmemcpy(child->image_path, path, path_length + 1u);
    if (cwd_ready && files_ready && path_length < sizeof(child->image_path) && process_prepare(child, path, true)) {
        lock_flags = anix_spin_lock_irqsave(&process_table_lock);
        child->next = processes;
        processes = child;
        anix_spin_unlock_irqrestore(&process_table_lock, lock_flags);
        if (scheduler_create_task_context("user", user_process_task, child, child->space.root_phys,
                                          &child->return_context)) {
            serial_write("[user] spawned pid "); serial_write_dec(child->pid); serial_write(" from pid ");
            serial_write_dec(parent->pid); serial_write("\n");
            return child->pid;
        }
    }
    process_release_resources(child);
    discard_process_record(child);
    return (uint64_t)-1;
}
