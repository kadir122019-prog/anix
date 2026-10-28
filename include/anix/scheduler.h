#ifndef ANIX_SCHEDULER_H
#define ANIX_SCHEDULER_H
#include <anix/types.h>
#include <anix/wait_queue.h>

typedef void (*task_entry)(void *argument);
struct task_user_context {
    uintptr_t kernel_rsp;
    uint64_t kernel_cr3;
    uintptr_t kernel_rbx, kernel_rbp, kernel_r12, kernel_r13, kernel_r14, kernel_r15;
};
bool scheduler_init(void);
bool scheduler_create_task(const char *name, task_entry entry, void *argument);
bool scheduler_create_task_context(const char *name, task_entry entry, void *argument,
                                   uint64_t address_space_root, struct task_user_context *user_context);
void *scheduler_current_argument(void);
void scheduler_set_current_argument(void *argument);
void scheduler_set_current_address_space(uint64_t root_phys);
void scheduler_set_current_user_context(struct task_user_context *context);
uint64_t scheduler_kernel_address_space_root(void);
extern struct task_user_context *scheduler_current_user_context;
void scheduler_yield(void);
bool scheduler_sleep_ticks(uint64_t ticks);
void scheduler_on_timer_tick(void);
uint64_t scheduler_preemption_count(void);
void scheduler_idle_task(void *argument);
void scheduler_exit_current(void) __attribute__((noreturn));
void scheduler_run(void);
uint64_t scheduler_task_count(void);
uint64_t scheduler_demo_count(void);
void scheduler_demo_task(void *argument);

#endif
