#ifndef ANIX_HEAP_H
#define ANIX_HEAP_H
#include <anix/types.h>

bool kheap_init(void);
void *kmalloc(size_t size);
void *kcalloc(size_t count, size_t size);
void *krealloc(void *pointer, size_t size);
bool kfree(void *pointer);

#endif
