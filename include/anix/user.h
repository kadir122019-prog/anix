#ifndef ANIX_USER_H
#define ANIX_USER_H
#include <anix/types.h>
#include <anix/scheduler.h>
bool user_process_start(const char *path);
void user_exception_abort(uint64_t vector, uint64_t rip, uint64_t error) __attribute__((noreturn));
#endif
