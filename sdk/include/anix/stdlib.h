#ifndef ANIX_SDK_STDLIB_H
#define ANIX_SDK_STDLIB_H

#include <stddef.h>

/* Small SDK allocator backed by the process sbrk syscall; not POSIX libc. */
void *anix_malloc(size_t size);
void anix_free(void *pointer);
void *anix_calloc(size_t count, size_t size);
void *anix_realloc(void *pointer, size_t size);

#endif
