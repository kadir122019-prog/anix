#ifndef ANIX_UACCESS_H
#define ANIX_UACCESS_H
#include <anix/types.h>
void uaccess_copy_from(void *kernel_destination, const void *user_source, size_t length);
void uaccess_copy_to(void *user_destination, const void *kernel_source, size_t length);
#endif
