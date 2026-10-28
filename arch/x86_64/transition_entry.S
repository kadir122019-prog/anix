.section .text
.global arch_transition_roundtrip
.type arch_transition_roundtrip, @function
arch_transition_roundtrip:
    /* Preserve the caller's callee-saved registers on its original stack. */
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rdi, %r12
    movq %rsi, %r13
    movq %rsp, 8(%r12)       /* old_rsp */
    pushfq
    popq %rax
    movq %rax, 16(%r12)     /* original_rflags */
    movq %cr3, %rax
    movq %rax, 0(%r12)      /* old_cr3 */
    cli
    movq 24(%r12), %rax     /* candidate_cr3 */
    movq %rax, %cr3
    movq 40(%r12), %rsp     /* stack_top; page-aligned for SysV call ABI */
    movq %r12, %rdi
    call *%r13
    movq %r12, %rdi
    movq %rax, %rsi          /* test result from continuation */
    jmp arch_transition_rollback
.size arch_transition_roundtrip, .-arch_transition_roundtrip

.global arch_transition_rollback
.type arch_transition_rollback, @function
arch_transition_rollback:
    cli
    movq %rsi, 48(%rdi)     /* test_result */
    movq 0(%rdi), %rax      /* restore old_cr3 before old_rsp */
    movq %rax, %cr3
    movq 8(%rdi), %rsp
    movq 16(%rdi), %rax
    pushq %rax
    popfq
    movq 48(%rdi), %rax
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret
.size arch_transition_rollback, .-arch_transition_rollback
.section .note.GNU-stack,"",@progbits
