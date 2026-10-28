.section .text
.global _start
.extern user_main
_start:
    xorq %rbp, %rbp
    call user_main
    movq %rax, %rdi
    movq $5, %rax
    int $0x80
1:  jmp 1b
.section .note.GNU-stack,"",@progbits
