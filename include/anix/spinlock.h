#ifndef ANIX_SPINLOCK_H
#define ANIX_SPINLOCK_H
#include <anix/types.h>

/* Interrupt-saving lock for kernel critical sections on the current CPU. */
struct anix_spinlock { uint8_t held; };

static inline uintptr_t anix_spin_lock_irqsave(struct anix_spinlock *lock) {
#if defined(ANIX_HOST_TEST)
    while (__atomic_test_and_set(&lock->held, __ATOMIC_ACQUIRE)) __asm__ volatile("pause");
    return 0;
#else
    uintptr_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory", "cc");
    while (__atomic_test_and_set(&lock->held, __ATOMIC_ACQUIRE)) __asm__ volatile("pause");
    return flags;
#endif
}

static inline void anix_spin_unlock_irqrestore(struct anix_spinlock *lock, uintptr_t flags) {
#if defined(ANIX_HOST_TEST)
    (void)flags;
    __atomic_clear(&lock->held, __ATOMIC_RELEASE);
#else
    __atomic_clear(&lock->held, __ATOMIC_RELEASE);
    __asm__ volatile("pushq %0; popfq" : : "r"(flags) : "memory", "cc");
#endif
}

#endif
