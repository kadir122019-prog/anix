.section .text
.global gdt_load
.type gdt_load, @function
gdt_load:
    lgdt (%rdi)
    mov %si, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    xor %eax, %eax
    mov %ax, %fs
    mov %ax, %gs
    movzwq %dx, %rdx
    pushq %rdx
    leaq .Lreload_cs(%rip), %rax
    pushq %rax
    lretq
.Lreload_cs:
    ret
.size gdt_load, .-gdt_load
.section .note.GNU-stack,"",@progbits
