#ifndef ANIX_WAIT_QUEUE_H
#define ANIX_WAIT_QUEUE_H

#include <anix/types.h>

struct task;

/*
 * Intrusive scheduler queue. A task can wait on at most one queue at a time.
 * wait() preserves the caller's interrupt state and atomically links before
 * blocking. Callers must make the condition check and wait indivisible (for
 * example, disable interrupts around both) to prevent lost wakeups.
 * Wake operations are interrupt-safe and preserve the caller's interrupt state.
 */
struct wait_queue {
    struct task *head;
    struct task *tail;
};

void wait_queue_init(struct wait_queue *queue);
void wait_queue_wait(struct wait_queue *queue);
void wait_queue_wake_one(struct wait_queue *queue);
void wait_queue_wake_all(struct wait_queue *queue);

#endif
