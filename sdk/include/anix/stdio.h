#ifndef ANIX_SDK_STDIO_H
#define ANIX_SDK_STDIO_H

#include <stddef.h>

/* Freestanding descriptor-based I/O helpers. Supported formats are documented in sdk/README.md. */
long anix_fprintf(int descriptor, const char *format, ...);
long anix_printf(const char *format, ...);
long anix_getline(char *buffer, size_t capacity);

#endif
