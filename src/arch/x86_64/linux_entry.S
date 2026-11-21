.code64
.section .text
.extern linux_kernel_cr3
.extern linux_user_cr3
.extern linux_kernel_stack
.extern linux_user_stack
.extern interrupt_dispatch
.extern linux_compat_syscall
.extern task_interrupt_exit

/* SYSCALL does not switch stacks. IF and DF are masked by IA32_FMASK. */
.global linux_syscall_entry
.type linux_syscall_entry,@function
linux_syscall_entry:
    movq %rsp, linux_user_stack(%rip)
    movq linux_kernel_stack(%rip), %rsp
    pushq $0x23
    pushq linux_user_stack(%rip)
    pushq %r11
    pushq $0x1b
    pushq %rcx
    pushq $0
    pushq $256
    jmp linux_interrupt_common
.size linux_syscall_entry,.-linux_syscall_entry

.macro LINUX_NOERR num
.section .text
linux_vector_\num:
    pushq $0
    pushq $\num
    jmp linux_interrupt_common
.section .rodata
    .quad linux_vector_\num
.endm
.macro LINUX_ERR num
.section .text
linux_vector_\num:
    pushq $\num
    jmp linux_interrupt_common
.section .rodata
    .quad linux_vector_\num
.endm
.section .rodata
.balign 8
.global linux_interrupt_stub_table
linux_interrupt_stub_table:
LINUX_NOERR 0
LINUX_NOERR 1
LINUX_NOERR 2
LINUX_NOERR 3
LINUX_NOERR 4
LINUX_NOERR 5
LINUX_NOERR 6
LINUX_NOERR 7
LINUX_ERR 8
LINUX_NOERR 9
LINUX_ERR 10
LINUX_ERR 11
LINUX_ERR 12
LINUX_ERR 13
LINUX_ERR 14
LINUX_NOERR 15
LINUX_NOERR 16
LINUX_ERR 17
LINUX_NOERR 18
LINUX_NOERR 19
LINUX_NOERR 20
LINUX_ERR 21
LINUX_NOERR 22
LINUX_NOERR 23
LINUX_NOERR 24
LINUX_NOERR 25
LINUX_NOERR 26
LINUX_NOERR 27
LINUX_NOERR 28
LINUX_ERR 29
LINUX_ERR 30
LINUX_NOERR 31
LINUX_NOERR 32
LINUX_NOERR 33
LINUX_NOERR 34
LINUX_NOERR 35
LINUX_NOERR 36
LINUX_NOERR 37
LINUX_NOERR 38
LINUX_NOERR 39
LINUX_NOERR 40
LINUX_NOERR 41
LINUX_NOERR 42
LINUX_NOERR 43
LINUX_NOERR 44
LINUX_NOERR 45
LINUX_NOERR 46
LINUX_NOERR 47

.section .text
linux_interrupt_common:
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
    cld
    movq %rsp, %rdi
    movq %cr3, %rdx
    cmpq linux_user_cr3(%rip), %rdx
    jne 1f
    movq linux_kernel_cr3(%rip), %rax
    movq %rax, %cr3
1:
    movq %rsp, %rax
    andq $-16, %rsp
    subq $16, %rsp
    movq %rax, (%rsp)
    movq %rdx, 8(%rsp)
    cmpq $256, 120(%rdi)
    je 2f
    call interrupt_dispatch
    call task_interrupt_exit
    jmp 3f
2:
    call linux_compat_syscall
3:
    movq 8(%rsp), %rax
    movq %rax, %cr3
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

.global linux_enter
.type linux_enter,@function
/* rdi=entry rsi=stack rdx=CR3. All kernel storage remains supervisor-only. */
linux_enter:
    cli
    movw $0x23, %ax
    movw %ax, %ds
    movw %ax, %es
    xorw %ax, %ax
    movw %ax, %fs
    movw %ax, %gs
    movq %rdx, %cr3
    pushq $0x23
    pushq %rsi
    pushq $0x202
    pushq $0x1b
    pushq %rdi
    xorq %rax, %rax
    xorq %rbx, %rbx
    xorq %rcx, %rcx
    xorq %rdx, %rdx
    xorq %rsi, %rsi
    xorq %rdi, %rdi
    xorq %rbp, %rbp
    xorq %r8, %r8
    xorq %r9, %r9
    xorq %r10, %r10
    xorq %r11, %r11
    xorq %r12, %r12
    xorq %r13, %r13
    xorq %r14, %r14
    xorq %r15, %r15
    iretq
.size linux_enter,.-linux_enter
.section .note.GNU-stack,"",@progbits
