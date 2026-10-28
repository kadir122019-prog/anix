.section .text
.global _start
.type _start,@function
.extern anix_main
_start:
    xorq %rbp, %rbp
    movq %rsp, %rbx
    movq (%rbx), %rdi
    leaq 8(%rbx), %rsi
    call anix_main
    movl %eax, %edi
    movq $5, %rax
    int $0x80
1:  pause
    jmp 1b
.section .note.GNU-stack,"",@progbits
