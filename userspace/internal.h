#ifndef ANIX_USERSPACE_INTERNAL_H
#define ANIX_USERSPACE_INTERNAL_H

#include <anix/interrupts.h>
#include <anix/scheduler.h>
#include <anix/vfs.h>
#include <anix/vmm.h>

#define USER_PATH_MAX 256u
#define USER_MAX_PROCESSES 32u
#define USER_MAX_ARGUMENTS 16u
#define USER_MAX_ARGUMENT_LENGTH 128u

struct user_launch {
    uint64_t argc, envc;
    char argv[USER_MAX_ARGUMENTS][USER_MAX_ARGUMENT_LENGTH];
    char envp[USER_MAX_ARGUMENTS][USER_MAX_ARGUMENT_LENGTH];
};
struct owned_user_page { uint64_t virtual_address, physical_address; };
enum user_file_kind { USER_FILE_NODE, USER_FILE_STDIN, USER_FILE_SERIAL, USER_FILE_PIPE_READ, USER_FILE_PIPE_WRITE };
#define USER_PIPE_CAPACITY 4096u
struct user_pipe {
    uint8_t bytes[USER_PIPE_CAPACITY];
    size_t head, used;
    uint32_t readers, writers;
    struct wait_queue read_waiters, write_waiters;
};
struct user_file {
    struct vfs_node *node;
    struct user_pipe *pipe;
    uint64_t offset;
    uint32_t references;
    enum user_file_kind kind;
    bool writable, append;
};
struct user_process {
    struct vmm_space space;
    struct vfs_node *cwd_node;
    struct owned_user_page *pages;
    size_t page_count, page_capacity;
    struct user_file *files[8];
    struct user_file *standard_files[3];
    uint8_t standard_state[3]; /* 0=implicit default, 1=redirected, 2=closed */
    struct task_user_context return_context;
    struct user_process *parent;
    struct user_process *next;
    struct wait_queue child_waiters;
    uint64_t pid;
    uint64_t process_group_id;
    uint64_t kernel_root_phys;
    uint64_t exit_status;
    uint64_t entry;
    uint64_t initial_stack;
    uint64_t heap_start;
    uint64_t heap_break;
    uint64_t heap_mapped_end;
    struct user_launch launch;
    char image_path[USER_PATH_MAX];
    char cwd[USER_PATH_MAX];
    bool exited;
    bool space_ready;
    bool kva_registered;
    bool allocated;
    bool launch_configured;
};

struct user_process *current_process(void);
uint64_t user_process_parent_pid(const struct user_process *process);
uint64_t user_process_getpgid(const struct user_process *process, uint64_t pid);
bool user_process_setpgid(struct user_process *process, uint64_t pid, uint64_t pgid);
bool user_process_exit_status(const struct user_process *process, uint64_t *status);
bool user_range(uint64_t address, uint64_t length, bool writable);
bool user_string(uint64_t address, char *out, size_t capacity);
bool copy_user_launch(uint64_t user_argv, uint64_t user_envp, struct user_launch *launch);
void default_launch(struct user_launch *launch, const char *path);
bool resolve_process_path(const struct user_process *process, const char *path,
                          char *output, size_t output_capacity);
struct user_process *find_child(struct user_process *parent, uint64_t pid);
struct user_process *find_wait_child(struct user_process *parent, int64_t pid,
                                     bool exited_only, bool *has_match);
void discard_process_record(struct user_process *process);
bool replace_process_image(struct user_process *process, const char *path,
                           const struct user_launch *launch, struct interrupt_frame *frame);
uint64_t spawn_process(const struct user_process *parent, const char *path,
                       const struct user_launch *launch);
uint64_t user_heap_sbrk(struct user_process *process, int64_t increment);
struct user_file *user_file_create(struct vfs_node *node, uint64_t offset, bool writable, bool append);
struct user_file *user_file_create_standard(uint64_t descriptor);
bool user_file_retain(struct user_file *file);
void user_file_release(struct user_file *file);
bool user_pipe_create(struct user_process *process, int descriptors[2]);
uint64_t user_pipe_read(struct user_file *file, void *buffer, size_t length);
uint64_t user_pipe_write(struct user_file *file, const void *buffer, size_t length);

#endif
