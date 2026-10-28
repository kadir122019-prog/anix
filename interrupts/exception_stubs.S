.section .text
.extern exception_dispatch
.extern irq_dispatch
.extern syscall_interrupt
.extern scheduler_current_user_context
.global syscall_stub
syscall_stub:
    pushq $0
    pushq $128
    cld
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, %rdi
    movq %rsp, %rbx
    andq $-16, %rsp
    subq $16, %rsp
    movq %rbx, (%rsp)
    call syscall_interrupt
    testb %al, %al
    jnz .Luser_exit
    movq (%rsp), %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    addq $16, %rsp
    iretq
.Luser_exit:
    movq scheduler_current_user_context(%rip), %rax
    testq %rax, %rax
    jz .Lfatal_user_context
    movq 8(%rax), %rax
    movq %rax, %cr3
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movq scheduler_current_user_context(%rip), %rcx
    movq 16(%rcx), %rbx
    movq 24(%rcx), %rbp
    movq 32(%rcx), %r12
    movq 40(%rcx), %r13
    movq 48(%rcx), %r14
    movq 56(%rcx), %r15
    movq 0(%rcx), %rsp
    ret

.global arch_user_exception_exit
arch_user_exception_exit:
    movq scheduler_current_user_context(%rip), %rax
    testq %rax, %rax
    jz .Lfatal_user_context
    movq 8(%rax), %rax
    movq %rax, %cr3
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movq scheduler_current_user_context(%rip), %rcx
    movq 16(%rcx), %rbx
    movq 24(%rcx), %rbp
    movq 32(%rcx), %r12
    movq 40(%rcx), %r13
    movq 48(%rcx), %r14
    movq 56(%rcx), %r15
    movq 0(%rcx), %rsp
    ret
.Lfatal_user_context:
    ud2

.macro EXCEPTION_NO_ERROR vector
.global exception_stub_\vector
exception_stub_\vector:
    pushq $0
    pushq $\vector
    jmp exception_common
.endm
.macro EXCEPTION_ERROR vector
.global exception_stub_\vector
exception_stub_\vector:
    pushq $\vector
    jmp exception_common
.endm
EXCEPTION_NO_ERROR 0
EXCEPTION_NO_ERROR 1
EXCEPTION_NO_ERROR 2
EXCEPTION_NO_ERROR 3
EXCEPTION_NO_ERROR 4
EXCEPTION_NO_ERROR 5
EXCEPTION_NO_ERROR 6
EXCEPTION_NO_ERROR 7
EXCEPTION_ERROR 8
EXCEPTION_NO_ERROR 9
EXCEPTION_ERROR 10
EXCEPTION_ERROR 11
EXCEPTION_ERROR 12
EXCEPTION_ERROR 13
EXCEPTION_ERROR 14
EXCEPTION_NO_ERROR 15
EXCEPTION_NO_ERROR 16
EXCEPTION_ERROR 17
EXCEPTION_NO_ERROR 18
EXCEPTION_NO_ERROR 19
EXCEPTION_NO_ERROR 20
EXCEPTION_ERROR 21
EXCEPTION_NO_ERROR 22
EXCEPTION_NO_ERROR 23
EXCEPTION_NO_ERROR 24
EXCEPTION_NO_ERROR 25
EXCEPTION_NO_ERROR 26
EXCEPTION_NO_ERROR 27
EXCEPTION_NO_ERROR 28
EXCEPTION_ERROR 29
EXCEPTION_ERROR 30
EXCEPTION_NO_ERROR 31
exception_common:
    cld
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, %rdi
    movq %rsp, %rbx
    andq $-16, %rsp
    subq $16, %rsp
    movq %rbx, (%rsp)
    call exception_dispatch
    ud2

.macro IRQ_NO_ERROR vector
.global irq_stub_\vector
irq_stub_\vector:
    pushq $0
    pushq $\vector
    jmp irq_common
.endm
IRQ_NO_ERROR 32
IRQ_NO_ERROR 33
IRQ_NO_ERROR 34
IRQ_NO_ERROR 35
IRQ_NO_ERROR 36
IRQ_NO_ERROR 37
IRQ_NO_ERROR 38
IRQ_NO_ERROR 39
IRQ_NO_ERROR 40
IRQ_NO_ERROR 41
IRQ_NO_ERROR 42
IRQ_NO_ERROR 43
IRQ_NO_ERROR 44
IRQ_NO_ERROR 45
IRQ_NO_ERROR 46
IRQ_NO_ERROR 47

irq_common:
    cld
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, %rdi
    movq %rsp, %rbx
    andq $-16, %rsp
    subq $16, %rsp
    movq %rbx, (%rsp)
    call irq_dispatch
    movq (%rsp), %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    addq $16, %rsp
    iretq
.section .rodata
.global exception_stub_table
.align 8
exception_stub_table:
    .quad exception_stub_0
    .quad exception_stub_1
    .quad exception_stub_2
    .quad exception_stub_3
    .quad exception_stub_4
    .quad exception_stub_5
    .quad exception_stub_6
    .quad exception_stub_7
    .quad exception_stub_8
    .quad exception_stub_9
    .quad exception_stub_10
    .quad exception_stub_11
    .quad exception_stub_12
    .quad exception_stub_13
    .quad exception_stub_14
    .quad exception_stub_15
    .quad exception_stub_16
    .quad exception_stub_17
    .quad exception_stub_18
    .quad exception_stub_19
    .quad exception_stub_20
    .quad exception_stub_21
    .quad exception_stub_22
    .quad exception_stub_23
    .quad exception_stub_24
    .quad exception_stub_25
    .quad exception_stub_26
    .quad exception_stub_27
    .quad exception_stub_28
    .quad exception_stub_29
    .quad exception_stub_30
    .quad exception_stub_31
.section .rodata
.global irq_stub_table
.align 8
irq_stub_table:
    .quad irq_stub_32
    .quad irq_stub_33
    .quad irq_stub_34
    .quad irq_stub_35
    .quad irq_stub_36
    .quad irq_stub_37
    .quad irq_stub_38
    .quad irq_stub_39
    .quad irq_stub_40
    .quad irq_stub_41
    .quad irq_stub_42
    .quad irq_stub_43
    .quad irq_stub_44
    .quad irq_stub_45
    .quad irq_stub_46
    .quad irq_stub_47
.section .note.GNU-stack,"",@progbits
