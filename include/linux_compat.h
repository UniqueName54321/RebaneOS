#ifndef ROCKOS_LINUX_COMPAT_H
#define ROCKOS_LINUX_COMPAT_H

#include <stdbool.h>
#include <stdint.h>

/* Read-only ustar Multiboot module named REBANE_LINUX_ROOT. */
void linux_compat_init(uintptr_t boot_info_address);
bool linux_compat_launch(const char* path, const char* arguments);
void linux_compat_task(void);
bool linux_compat_active(void);
bool linux_compat_current(void);
/* Full-screen graphical browser owns display and input until its guest exits. */
bool linux_browser_launch(const char* url);
bool linux_browser_active(void);
void linux_compat_fault(uint64_t vector, uint64_t error, uint64_t rip,
    uint64_t address) __attribute__((noreturn));

/* Matches the x86-64 interrupt register frame. */
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error;
    uint64_t rip, cs, rflags, rsp, ss;
} linux_registers_t;
void linux_compat_syscall(linux_registers_t* registers);

#endif
