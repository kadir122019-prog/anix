#ifndef ANIX_PANIC_H
#define ANIX_PANIC_H
#include <anix/compiler.h>
ANIX_NORETURN void panic(const char *message);
#endif
