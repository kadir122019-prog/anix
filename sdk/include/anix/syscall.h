#ifndef ANIX_SDK_SYSCALL_H
#define ANIX_SDK_SYSCALL_H

/* Anix userspace ABI v1. All calls return a nonnegative result or -1. */
#include <stddef.h>
#include <stdint.h>
#include <anix/syscall_abi.h>
#include <anix/stat.h>
#include <anix/input.h>

static inline long anix_syscall0(long number) {
    long result;
    __asm__ volatile("int $0x80" : "=a"(result) : "0"(number) : "memory", "cc");
    return result;
}

static inline long anix_syscall1(long number, long a) {
    long result;
    __asm__ volatile("int $0x80" : "=a"(result) : "0"(number), "D"(a) : "memory", "cc");
    return result;
}

static inline long anix_syscall2(long number, long a, long b) {
    long result;
    __asm__ volatile("int $0x80" : "=a"(result) : "0"(number), "D"(a), "S"(b) : "memory", "cc");
    return result;
}

static inline long anix_syscall3(long number, long a, long b, long c) {
    long result;
    __asm__ volatile("int $0x80" : "=a"(result) : "0"(number), "D"(a), "S"(b), "d"(c) : "memory", "cc");
    return result;
}

static inline long anix_write(int fd, const void *buffer, size_t length) {
    return anix_syscall3(ANIX_SYS_WRITE, fd, (long)buffer, (long)length);
}
static inline long anix_readline(char *buffer, size_t capacity) {
    return anix_syscall2(ANIX_SYS_READLINE, (long)buffer, (long)capacity);
}
static inline long anix_read_char(void) { return anix_syscall0(ANIX_SYS_READ_CHAR); }
static inline long anix_input_event(struct input_event *event) {
    return anix_syscall1(ANIX_SYS_INPUT_EVENT, (long)event);
}
static inline long anix_poweroff(void) { return anix_syscall0(ANIX_SYS_POWEROFF); }
static inline long anix_ticks(void) { return anix_syscall0(ANIX_SYS_TICKS); }
static inline long anix_getpid(void) { return anix_syscall0(ANIX_SYS_GETPID); }
static inline long anix_getppid(void) { return anix_syscall0(ANIX_SYS_GETPPID); }
static inline long anix_getpgid(long pid) { return anix_syscall1(ANIX_SYS_GETPGID, pid); }
static inline long anix_setpgid(long pid, long pgid) { return anix_syscall2(ANIX_SYS_SETPGID, pid, pgid); }
static inline long anix_sleep_ticks(uint64_t ticks) {
    return anix_syscall1(ANIX_SYS_SLEEP_TICKS, (long)ticks);
}
static inline long anix_exec(const char *path) { return anix_syscall1(ANIX_SYS_EXEC, (long)path); }
static inline long anix_execv(const char *path, char *const argv[], char *const envp[]) {
    return anix_syscall3(ANIX_SYS_EXECV, (long)path, (long)argv, (long)envp);
}
static inline long anix_open(const char *path) { return anix_syscall1(ANIX_SYS_OPEN, (long)path); }
static inline long anix_open_mode(const char *path, unsigned flags) {
    return anix_syscall2(ANIX_SYS_OPEN_MODE, (long)path, (long)flags);
}
static inline long anix_read(int fd, void *buffer, size_t capacity) {
    return anix_syscall3(ANIX_SYS_READ, fd, (long)buffer, (long)capacity);
}
static inline long anix_list(const char *path, char *buffer, size_t capacity) {
    return anix_syscall3(ANIX_SYS_LIST, (long)path, (long)buffer, (long)capacity);
}
static inline long anix_stat(const char *path, struct anix_stat *status) {
    return anix_syscall2(ANIX_SYS_STAT, (long)path, (long)status);
}
static inline long anix_pipe(int descriptors[2]) { return anix_syscall1(ANIX_SYS_PIPE, (long)descriptors); }
static inline long anix_close(int fd) { return anix_syscall1(ANIX_SYS_CLOSE, fd); }
static inline long anix_dup(int fd) { return anix_syscall1(ANIX_SYS_DUP, fd); }
static inline long anix_dup2(int source, int target) { return anix_syscall2(ANIX_SYS_DUP2, source, target); }
static inline long anix_yield(void) { return anix_syscall0(ANIX_SYS_YIELD); }
static inline long anix_create(const char *path) { return anix_syscall1(ANIX_SYS_CREATE, (long)path); }
static inline long anix_write_file(int fd, const void *buffer, size_t length) {
    return anix_syscall3(ANIX_SYS_WRITE_FILE, fd, (long)buffer, (long)length);
}
static inline long anix_seek(int fd, size_t absolute_offset) {
    return anix_syscall2(ANIX_SYS_SEEK, fd, (long)absolute_offset);
}
static inline long anix_unlink(const char *path) { return anix_syscall1(ANIX_SYS_UNLINK, (long)path); }
static inline long anix_spawn(const char *path) { return anix_syscall1(ANIX_SYS_SPAWN, (long)path); }
static inline long anix_spawnv(const char *path, char *const argv[], char *const envp[]) {
    return anix_syscall3(ANIX_SYS_SPAWNV, (long)path, (long)argv, (long)envp);
}
static inline long anix_wait(long pid) { return anix_syscall1(ANIX_SYS_WAIT, pid); }
static inline long anix_waitpid(long pid, uint64_t *status) {
    return anix_syscall2(ANIX_SYS_WAITPID, pid, (long)status);
}
static inline void *anix_sbrk(long increment) {
    const long result = anix_syscall1(ANIX_SYS_SBRK, increment);
    return result < 0 ? (void *)-1 : (void *)(uintptr_t)result;
}
static inline long anix_mkdir(const char *path) { return anix_syscall1(ANIX_SYS_MKDIR, (long)path); }
static inline long anix_rmdir(const char *path) { return anix_syscall1(ANIX_SYS_RMDIR, (long)path); }
static inline long anix_chdir(const char *path) { return anix_syscall1(ANIX_SYS_CHDIR, (long)path); }
/* Returns the path length excluding NUL, and writes a terminated path on success. */
static inline long anix_getcwd(char *buffer, size_t capacity) {
    return anix_syscall2(ANIX_SYS_GETCWD, (long)buffer, (long)capacity);
}
static inline void anix_exit(int status) {
    (void)anix_syscall1(ANIX_SYS_EXIT, status);
    for (;;) __asm__ volatile("pause");
}

#endif
