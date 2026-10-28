#ifndef ANIX_LIVE_RANGES_H
#define ANIX_LIVE_RANGES_H
#include <anix/types.h>
struct live_range { const char *name; uint64_t start, end; };
size_t arch_live_ranges(struct live_range *ranges, size_t capacity);
#endif
