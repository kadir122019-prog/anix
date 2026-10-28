#ifndef ANIX_COMPILER_H
#define ANIX_COMPILER_H
#define ANIX_NORETURN __attribute__((noreturn))
#define ANIX_PACKED __attribute__((packed))
#define ANIX_USED __attribute__((used))
#define ANIX_SECTION(x) __attribute__((section(x)))
#endif
