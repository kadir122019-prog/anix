#include <anix/cpu.h>
#include <anix/string.h>
#include <anix/uaccess.h>

static uint64_t access_begin(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory", "cc");
    if (cpu_smap_active()) __asm__ volatile("stac" : : : "memory");
    return flags;
}
static void access_end(uint64_t flags) {
    if (cpu_smap_active()) __asm__ volatile("clac" : : : "memory");
    __asm__ volatile("pushq %0; popfq" : : "r"(flags) : "memory", "cc");
}
void uaccess_copy_from(void *kernel_destination, const void *user_source, size_t length) {
    uint64_t flags = access_begin();
    kmemcpy(kernel_destination, user_source, length);
    access_end(flags);
}
void uaccess_copy_to(void *user_destination, const void *kernel_source, size_t length) {
    uint64_t flags = access_begin();
    kmemcpy(user_destination, kernel_source, length);
    access_end(flags);
}
