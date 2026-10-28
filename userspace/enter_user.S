.section .text
.global arch_enter_user
.extern scheduler_current_user_context
arch_enter_user:
    movq scheduler_current_user_context(%rip), %rax
    testq %rax, %rax
    jz .Lbad_user_context
    movq %rsp, 0(%rax)
    movq %rbx, 16(%rax)
    movq %rbp, 24(%rax)
    movq %r12, 32(%rax)
    movq %r13, 40(%rax)
    movq %r14, 48(%rax)
    movq %r15, 56(%rax)
    movq %cr3, %rax
    movq scheduler_current_user_context(%rip), %rcx
    movq %rax, 8(%rcx)
    movq %rdx, %cr3
    movq $0x2b, %rax
    movw %ax, %ds
    movw %ax, %es
    pushq $0x2b
    pushq %rsi
    pushq $0x202
    pushq $0x33
    pushq %rdi
    iretq
.Lbad_user_context:
    ud2
.section .note.GNU-stack,"",@progbits
