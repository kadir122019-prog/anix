#ifndef ANIX_USER_ABI_H
#define ANIX_USER_ABI_H
#include <stddef.h>
#include <stdint.h>
static inline long anix_call0(long number) {
    long result; __asm__ volatile("int $0x80" : "=a"(result) : "0"(number) : "memory", "cc"); return result;
}
static inline long anix_call1(long number, long a) {
    long result; __asm__ volatile("int $0x80" : "=a"(result) : "0"(number), "D"(a) : "memory", "cc"); return result;
}
static inline long anix_call2(long number, long a, long b) {
    long result; __asm__ volatile("int $0x80" : "=a"(result) : "0"(number), "D"(a), "S"(b) : "memory", "cc"); return result;
}
static inline long anix_call3(long number, long a, long b, long c) {
    long result; __asm__ volatile("int $0x80" : "=a"(result) : "0"(number), "D"(a), "S"(b), "d"(c) : "memory", "cc"); return result;
}
static inline long anix_spawn(const char *path) { return anix_call1(15, (long)path); }
static inline long anix_spawnv(const char *path, char *const argv[], char *const envp[]) {
    return anix_call3(23, (long)path, (long)argv, (long)envp);
}
static inline long anix_wait(long pid) { return anix_call1(16, pid); }
static inline long anix_waitpid(long pid, uint64_t *status) { return anix_call2(24, pid, (long)status); }
static inline long anix_write(const char *text, size_t length) { return anix_call3(1, 1, (long)text, (long)length); }
static inline long anix_read_line(char *buffer, size_t capacity) { return anix_call2(2, (long)buffer, (long)capacity); }
static inline long anix_read_char(void) { return anix_call0(35); }
static inline long anix_poweroff(void) { return anix_call0(36); }
static inline long anix_ticks(void) { return anix_call0(3); }
static inline long anix_execve(const char *path) { return anix_call1(4, (long)path); }
static inline long anix_execv(const char *path, char *const argv[], char *const envp[]) {
    return anix_call3(22, (long)path, (long)argv, (long)envp);
}
static inline long anix_open(const char *path) { return anix_call1(6, (long)path); }
static inline long anix_read_file(long fd, char *buffer, size_t capacity) { return anix_call3(7, fd, (long)buffer, (long)capacity); }
static inline long anix_close(long fd) { return anix_call1(9, fd); }
static inline long anix_list(const char *path, char *buffer, size_t capacity) { return anix_call3(8, (long)path, (long)buffer, (long)capacity); }
static inline long anix_chdir(const char *path) { return anix_call1(18, (long)path); }
static inline long anix_getcwd(char *buffer, size_t capacity) { return anix_call2(19, (long)buffer, (long)capacity); }
static inline long anix_getpid(void) { return anix_call0(28); }
static inline long anix_getpgid(long pid) { return anix_call1(33, pid); }
static inline void anix_exit(long status) { (void)anix_call1(5, status); for (;;) { } }
static inline size_t anix_len(const char *text) { size_t n=0; while(text[n]) ++n; return n; }
static inline void anix_puts(const char *text) { (void)anix_write(text, anix_len(text)); }
#endif
