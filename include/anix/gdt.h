#ifndef ANIX_GDT_H
#define ANIX_GDT_H
#include <anix/types.h>
#include <anix/live_ranges.h>
void gdt_init(void);
void gdt_set_rsp0(uintptr_t stack_top);
size_t gdt_live_ranges(struct live_range *ranges, size_t capacity);
#endif
