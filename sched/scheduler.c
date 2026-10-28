#include <anix/heap.h>
#include <anix/cpu.h>
#include <anix/gdt.h>
#include <anix/serial.h>
#include <anix/kva.h>
#include <anix/scheduler.h>
#include <anix/string.h>
#include <anix/timer.h>
#include <anix/vmm.h>

#define TASK_STACK_PAGES 4u
#define TASK_INTERRUPT_STACK_PAGES 4u
#define TASK_NAME_LENGTH 24u
enum task_state { TASK_READY, TASK_RUNNING, TASK_BLOCKED, TASK_SLEEPING, TASK_EXITED };
struct task {
    uintptr_t stack_pointer;
    void *stack_base;
    void *interrupt_stack_base;
    task_entry entry;
    void *argument;
    uint64_t address_space_root;
    struct task_user_context *user_context;
    enum task_state state;
    uint64_t wake_deadline;
    struct wait_queue *waiting_on;
    struct task *wait_next;
    char name[TASK_NAME_LENGTH];
    struct task *next;
    uint8_t fpu_state[512] __attribute__((aligned(16)));
};
static struct task *tasks;
static struct task *current;
static uintptr_t bootstrap_stack;
static bool initialized;
static bool scheduler_started;
static volatile uint64_t demo_counter;
static volatile uint64_t preemption_counter;
static bool preemption_reported;
static uint64_t kernel_address_space_root;
struct task_user_context *scheduler_current_user_context;
extern void arch_context_switch(uintptr_t *old_stack, uintptr_t new_stack);
static void task_bootstrap(void);

bool scheduler_create_task_context(const char *name, task_entry entry, void *argument,
                                   uint64_t address_space_root, struct task_user_context *user_context) {
    if (!initialized || !entry) return false;
    struct task *task = kmalloc(sizeof(*task));
    if (!task) return false;
    void *stack = kva_alloc_guarded_pages(TASK_STACK_PAGES, VMM_WRITABLE | VMM_NO_EXECUTE);
    if (!stack) { kfree(task); return false; }
    void *interrupt_stack = kva_alloc_guarded_pages(TASK_INTERRUPT_STACK_PAGES, VMM_WRITABLE | VMM_NO_EXECUTE);
    if (!interrupt_stack) { (void)kva_free_guarded_pages(stack, TASK_STACK_PAGES); kfree(task); return false; }
    if (address_space_root && address_space_root != vmm_active_root()) {
        struct vmm_space source, destination = { .root_phys = address_space_root, .owned = true };
        if (!vmm_import_active(&source) || !vmm_sync_missing_range(&source, &destination,
                0xffff800000000000ULL, 0xfffffffffffff000ULL)) {
            (void)kva_free_guarded_pages(stack, TASK_STACK_PAGES);
            (void)kva_free_guarded_pages(interrupt_stack, TASK_INTERRUPT_STACK_PAGES);
            kfree(task);
            return false;
        }
    }
    *task = (struct task){ .stack_base = stack, .interrupt_stack_base = interrupt_stack,
        .entry = entry, .argument = argument,
        .address_space_root = address_space_root ? address_space_root : kernel_address_space_root,
        .user_context = user_context, .state = TASK_READY };
    cpu_fpu_state_init(task->fpu_state);
    if (name) {
        size_t i = 0;
        while (i + 1u < sizeof(task->name) && name[i]) { task->name[i] = name[i]; ++i; }
        task->name[i] = '\0';
    }
    uintptr_t *frame = (uintptr_t *)((uint8_t *)stack + TASK_STACK_PAGES * VMM_PAGE_SIZE - 64u);
    for (size_t i = 0; i < 6; ++i) frame[i] = 0;
    frame[6] = (uintptr_t)task_bootstrap;
    task->stack_pointer = (uintptr_t)frame;
    if (!tasks) tasks = task;
    else {
        struct task *tail = tasks;
        while (tail->next) tail = tail->next;
        tail->next = task;
    }
    return true;
}

bool scheduler_create_task(const char *name, task_entry entry, void *argument) {
    return scheduler_create_task_context(name, entry, argument, kernel_address_space_root, 0);
}

bool scheduler_init(void) {
    if (initialized) return false;
    tasks = 0;
    current = 0;
    kernel_address_space_root = vmm_active_root();
    scheduler_current_user_context = 0;
    initialized = true;
    return true;
}

static struct task *next_task(void) {
    if (!tasks) return 0;
    struct task *start = current ? current->next : tasks;
    if (!start) start = tasks;
    struct task *task = start;
    do {
        if (task->state == TASK_READY) return task;
        task = task->next ? task->next : tasks;
    } while (task != start);
    return current && current->state == TASK_READY ? current : 0;
}

static void reap_exited_tasks(void) {
    struct task **link = &tasks;
    while (*link) {
        struct task *task = *link;
        if (task != current && task->state == TASK_EXITED) {
            *link = task->next;
            serial_write("[sched] reaped task "); serial_write(task->name); serial_write("\n");
            (void)kva_free_guarded_pages(task->stack_base, TASK_STACK_PAGES);
            (void)kva_free_guarded_pages(task->interrupt_stack_base, TASK_INTERRUPT_STACK_PAGES);
            (void)kfree(task);
        } else link = &task->next;
    }
}

static void scheduler_schedule(bool timer_preempt, enum task_state old_state) {
    if (!initialized) return;
    uintptr_t interrupt_flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(interrupt_flags) : : "memory", "cc");
    struct task *old = current;
    if (old) __asm__ volatile("fxsave64 %0" : "=m"(old->fpu_state) : : "memory");
    if (old) old->state = old_state;
    reap_exited_tasks();
    struct task *next = next_task();
    if (!next) {
        __asm__ volatile("pushq %0; popfq" : : "r"(interrupt_flags) : "memory", "cc");
        for (;;) __asm__ volatile("cli; hlt" : : : "memory");
    }
    next->state = TASK_RUNNING;
    current = next;
    scheduler_current_user_context = next->user_context;
    if (next->address_space_root && vmm_active_root() != next->address_space_root) {
        struct vmm_space next_space = { .root_phys = next->address_space_root };
        vmm_activate(&next_space);
    }
    gdt_set_rsp0((uintptr_t)next->interrupt_stack_base + TASK_INTERRUPT_STACK_PAGES * VMM_PAGE_SIZE);
    if (timer_preempt && old && old != next) {
        __atomic_add_fetch(&preemption_counter, 1u, __ATOMIC_RELAXED);
        if (!preemption_reported) {
            preemption_reported = true;
            serial_write("[sched] APIC timer preempted a running task\n");
        }
    }
    uintptr_t *old_stack = old ? &old->stack_pointer : &bootstrap_stack;
    __asm__ volatile("fxrstor64 %0" : : "m"(next->fpu_state) : "memory");
    arch_context_switch(old_stack, next->stack_pointer);
    __asm__ volatile("pushq %0; popfq" : : "r"(interrupt_flags) : "memory", "cc");
}

void *scheduler_current_argument(void) { return current ? current->argument : 0; }
void scheduler_set_current_argument(void *argument) { if (current) current->argument = argument; }
void scheduler_set_current_address_space(uint64_t root_phys) {
    if (!current || !root_phys) return;
    current->address_space_root = root_phys;
    struct vmm_space space = { .root_phys = root_phys };
    vmm_activate(&space);
}
void scheduler_set_current_user_context(struct task_user_context *context) {
    if (!current) return;
    current->user_context = context;
    scheduler_current_user_context = context;
}
uint64_t scheduler_kernel_address_space_root(void) { return kernel_address_space_root; }

void scheduler_yield(void) { scheduler_schedule(false, TASK_READY); }

void wait_queue_init(struct wait_queue *queue) {
    if (!queue) return;
    queue->head = 0;
    queue->tail = 0;
}

void wait_queue_wait(struct wait_queue *queue) {
    if (!initialized || !current || !queue) return;
    uintptr_t interrupt_flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(interrupt_flags) : : "memory", "cc");
    current->waiting_on = queue;
    current->wait_next = 0;
    if (queue->tail) queue->tail->wait_next = current;
    else queue->head = current;
    queue->tail = current;
    scheduler_schedule(false, TASK_BLOCKED);
    current->waiting_on = 0;
    current->wait_next = 0;
    __asm__ volatile("pushq %0; popfq" : : "r"(interrupt_flags) : "memory", "cc");
}

void wait_queue_wake_one(struct wait_queue *queue) {
    if (!initialized || !queue) return;
    uintptr_t interrupt_flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(interrupt_flags) : : "memory", "cc");
    struct task *task = queue->head;
    if (task) {
        queue->head = task->wait_next;
        if (!queue->head) queue->tail = 0;
        task->waiting_on = 0;
        task->wait_next = 0;
        if (task->state == TASK_BLOCKED) task->state = TASK_READY;
    }
    __asm__ volatile("pushq %0; popfq" : : "r"(interrupt_flags) : "memory", "cc");
}

void wait_queue_wake_all(struct wait_queue *queue) {
    if (!initialized || !queue) return;
    uintptr_t interrupt_flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(interrupt_flags) : : "memory", "cc");
    while (queue->head) {
        struct task *task = queue->head;
        queue->head = task->wait_next;
        task->waiting_on = 0;
        task->wait_next = 0;
        if (task->state == TASK_BLOCKED) task->state = TASK_READY;
    }
    queue->tail = 0;
    __asm__ volatile("pushq %0; popfq" : : "r"(interrupt_flags) : "memory", "cc");
}

bool scheduler_sleep_ticks(uint64_t ticks) {
    if (!initialized || !current || ticks > INT64_MAX) return false;
    if (ticks == 0) return true;
    const uint64_t deadline = timer_ticks() + ticks;
    for (;;) {
        uintptr_t interrupt_flags;
        __asm__ volatile("pushfq; popq %0; cli" : "=r"(interrupt_flags) : : "memory", "cc");
        if ((int64_t)(timer_ticks() - deadline) >= 0) {
            __asm__ volatile("pushq %0; popfq" : : "r"(interrupt_flags) : "memory", "cc");
            return true;
        }
        current->wake_deadline = deadline;
        scheduler_schedule(false, TASK_SLEEPING);
        __asm__ volatile("pushq %0; popfq" : : "r"(interrupt_flags) : "memory", "cc");
    }
}
static void task_bootstrap(void) {
    __asm__ volatile("sti" : : : "memory");
    struct task *task = current;
    if (task && task->entry) task->entry(task->argument);
    scheduler_exit_current();
}

void scheduler_run(void) {
    scheduler_started = true;
    for (;;) scheduler_yield();
}

void scheduler_on_timer_tick(void) {
    if (!scheduler_started) return;
    const uint64_t now = timer_ticks();
    for (struct task *task = tasks; task; task = task->next) {
        if (task->state == TASK_SLEEPING && (int64_t)(now - task->wake_deadline) >= 0) {
            task->wake_deadline = 0;
            task->state = TASK_READY;
        }
    }
    if (current && tasks && tasks->next) scheduler_schedule(true, TASK_READY);
}

void scheduler_exit_current(void) {
    scheduler_schedule(false, TASK_EXITED);
    __builtin_unreachable();
}

void scheduler_idle_task(void *argument) {
    (void)argument;
    for (;;) __asm__ volatile("sti; hlt" : : : "memory");
}

uint64_t scheduler_preemption_count(void) {
    return __atomic_load_n(&preemption_counter, __ATOMIC_RELAXED);
}

uint64_t scheduler_task_count(void) {
    uint64_t count = 0;
    for (const struct task *task = tasks; task; task = task->next) ++count;
    return count;
}

uint64_t scheduler_demo_count(void) { return __atomic_load_n(&demo_counter, __ATOMIC_RELAXED); }

void scheduler_demo_task(void *argument) {
    (void)argument;
    for (;;) {
        __atomic_add_fetch(&demo_counter, 1u, __ATOMIC_RELAXED);
    }
}
