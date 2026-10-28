#include <anix/libc.h>
#include <anix/syscall.h>

int anix_main(void) {
    const long pid = anix_getpid();
    const long parent_deadline = anix_ticks() + 1000;
    while (anix_getppid() != 1 && anix_ticks() < parent_deadline) (void)anix_yield();
    if (pid <= 1 || anix_getppid() != 1) return 43;
    static const char reparented[] = "ANIX_SDK_ORPHAN_PPID_OK\n";
    (void)anix_write(1, reparented, sizeof(reparented) - 1u);
    const long deadline = anix_ticks() + 200;
    while (anix_ticks() < deadline) (void)anix_yield();
    static const char message[] = "ANIX_SDK_ORPHAN_FINISHED\n";
    (void)anix_write(1, message, sizeof(message) - 1u);
    return 42;
}
