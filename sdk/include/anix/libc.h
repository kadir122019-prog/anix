#ifndef ANIX_SDK_LIBC_H
#define ANIX_SDK_LIBC_H
#include <stddef.h>

size_t anix_strlen(const char *text);
void *anix_memcpy(void *destination, const void *source, size_t length);
int anix_memcmp(const void *left, const void *right, size_t length);
int anix_strcmp(const char *left, const char *right);

#endif
