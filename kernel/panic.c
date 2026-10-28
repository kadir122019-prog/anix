#include <anix/panic.h>
#include <anix/serial.h>
void panic(const char *message) { __asm__ volatile("cli"); serial_write("\n*** ANIX KERNEL PANIC ***\n"); serial_write(message ? message : "(no message)"); serial_write("\nSystem halted.\n"); for (;;) __asm__ volatile("hlt"); }
