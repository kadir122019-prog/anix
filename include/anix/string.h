#ifndef ANIX_STRING_H
#define ANIX_STRING_H
#include <anix/types.h>
size_t kstrlen(const char *s);
void *kmemset(void *dst, int value, size_t n);
void *kmemcpy(void *dst, const void *src, size_t n);
int kmemcmp(const void *a, const void *b, size_t n);
#endif
