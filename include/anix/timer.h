#ifndef ANIX_TIMER_H
#define ANIX_TIMER_H
#include <anix/types.h>

bool timer_init(void);
uint64_t timer_ticks(void);
void timer_sleep_ticks(uint64_t count);

#endif
