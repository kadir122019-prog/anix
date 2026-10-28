#ifndef ANIX_TRANSITION_H
#define ANIX_TRANSITION_H
#include <anix/types.h>
#include <anix/vmm.h>
#define TRANSITION_STACK_PAGES 4u
#define TRANSITION_STACK_VIRTUAL_BASE 0xfffffe0000000000ULL
struct transition_stack {
    uint64_t physical[TRANSITION_STACK_PAGES];
    uint64_t virtual_base;
    uint64_t stack_top;
    bool prepared;
};
enum transition_test_result {
    TRANSITION_TEST_CR3 = 1u << 0,
    TRANSITION_TEST_STACK = 1u << 1,
    TRANSITION_TEST_PMM = 1u << 2,
    TRANSITION_TEST_HHDM = 1u << 3,
    TRANSITION_TEST_TABLES = 1u << 4,
    TRANSITION_TEST_ALL = (1u << 5) - 1u
};
struct transition_context {
    uint64_t old_cr3;
    uint64_t old_rsp;
    uint64_t original_rflags;
    uint64_t candidate_cr3;
    uint64_t stack_base;
    uint64_t stack_top;
    uint64_t test_result;
};
bool transition_stack_prepare(struct transition_stack *stack, struct vmm_space *candidate);
void transition_stack_release(struct transition_stack *stack, struct vmm_space *candidate);
bool transition_stack_validate(const struct transition_stack *stack, const struct vmm_space *candidate);
typedef uint64_t (*transition_continuation)(struct transition_context *context);
uint64_t arch_transition_roundtrip(struct transition_context *context, transition_continuation continuation);
uint64_t transition_run_roundtrip(struct transition_context *context, const struct vmm_space *candidate, const struct transition_stack *stack);
#endif
