#include <anix/libc.h>
#include <anix/stdio.h>
#include <anix/syscall.h>

static int verify_simd_context(uint32_t pattern) {
    for (unsigned i = 0; i < 512; ++i) {
        uint32_t observed;
        __asm__ volatile("movd %0, %%xmm15" : : "r"(pattern) : "xmm15");
        if (anix_yield() < 0) return 0;
        __asm__ volatile("movd %%xmm15, %0" : "=r"(observed));
        if (observed != pattern) return 0;
    }
    return 1;
}

static int parse_descriptor(const char *text) {
    if (!text || text[0] < '0' || text[0] > '9') return -1;
    int value = text[0] - '0';
    if (text[1]) {
        if (text[1] < '0' || text[1] > '9' || text[2]) return -1;
        value = value * 10 + text[1] - '0';
    }
    return value <= 10 ? value : -1;
}

int anix_main(int argc, char **argv) {
    int exit_status = 23;
    int verify_cwd = 1;
    if (argc == 2 && anix_strcmp(argv[1], "exec-check") == 0) {
        char **envp = argv + argc + 1;
        if (!envp[0] || anix_strcmp(envp[0], "ANIX_EXEC_ENV=present") != 0 || envp[1]) return 27;
        static const char exec_ok[] = "ANIX_SDK_EXEC_REPLACEMENT_OK\n";
        (void)anix_write(1, exec_ok, sizeof(exec_ok) - 1u);
        if (anix_exec("/bin/anixsh") < 0) return 28;
    } else if (argc == 4 && anix_strcmp(argv[1], "pgrp-check") == 0) {
        const int ready_fd = parse_descriptor(argv[2]);
        const int release_fd = parse_descriptor(argv[3]);
        char token;
        if (ready_fd < 0 || release_fd < 0 || anix_getpgid(0) != 1 ||
            anix_write(ready_fd, "R", 1) != 1 || anix_close(ready_fd) < 0 ||
            anix_read(release_fd, &token, 1) != 1 || token != 'G' ||
            anix_close(release_fd) < 0 || anix_getpgid(0) != anix_getpid()) return 67;
        exit_status = 0;
        verify_cwd = 0;
    } else if (argc == 3 && anix_strcmp(argv[1], "fd-check") == 0) {
        if (argv[2][0] < '0' || argv[2][0] > '9' || argv[2][1] != '\0') return 60;
        const int inherited = argv[2][0] - '0';
        char first[4];
        if (anix_read(inherited, first, sizeof(first)) != (long)sizeof(first) ||
            anix_memcmp(first, "Anix", sizeof(first)) != 0 || anix_close(inherited) < 0) return 61;
        exit_status = 59;
    } else if (argc == 2) {
        char **envp = argv + argc + 1;
        if (anix_strcmp(argv[1], "stdin-check") == 0) {
            static const char ready[] = "ANIX_SDK_STDIN_READY\n";
            (void)anix_write(1, ready, sizeof(ready) - 1u);
            char first[4], second[10];
            if (anix_read(0, first, sizeof(first)) != (long)sizeof(first) ||
                anix_memcmp(first, "fd0 ", sizeof(first)) != 0 ||
                anix_read(0, second, sizeof(second)) != (long)sizeof(second) ||
                anix_memcmp(second, "line test\n", sizeof(second)) != 0) return 57;
            char line[32];
            static const char expected_line[] = "sdk getline\n";
            if (anix_getline(line, sizeof(line)) != (long)(sizeof(expected_line) - 1u) ||
                anix_memcmp(line, expected_line, sizeof(expected_line)) != 0) return 62;
            int input_alias = (int)anix_dup(0);
            char aliased_line[8];
            if (input_alias < 3 || anix_read(input_alias, aliased_line, sizeof(aliased_line)) !=
                    (long)sizeof(aliased_line) || anix_memcmp(aliased_line, "fd0 dup\n", sizeof(aliased_line)) != 0 ||
                anix_close(input_alias) < 0) return 63;
            exit_status = 58;
        } else if (anix_strcmp(argv[1], "worker-argument") == 0) {
            if (!envp[0] || anix_strcmp(envp[0], "ANIX_WORKER_ENV=present") != 0 || envp[1] ||
                anix_getpid() <= 1 || anix_getppid() != 1) return 25;
            static const char child_ids_ok[] = "ANIX_SDK_CHILD_IDS_OK\n";
            (void)anix_write(1, child_ids_ok, sizeof(child_ids_ok) - 1u);
            static const char launch_ok[] = "ANIX_SDK_SPAWNV_OK\n";
            (void)anix_write(1, launch_ok, sizeof(launch_ok) - 1u);
        } else if (anix_strcmp(argv[1], "waitpid-block") == 0) {
            if (!envp[0] || anix_strcmp(envp[0], "ANIX_WAITPID_ENV=31") != 0 || envp[1]) return 29;
            const long before = anix_ticks();
            if (anix_sleep_ticks(0) != 0 || anix_sleep_ticks(UINT64_MAX) >= 0 ||
                anix_sleep_ticks(60) < 0 || anix_ticks() - before < 60) return 64;
            static const char sleep_ok[] = "ANIX_SDK_SLEEP_OK\n";
            (void)anix_write(1, sleep_ok, sizeof(sleep_ok) - 1u);
            exit_status = 31;
        } else if (anix_strcmp(argv[1], "sleep-short") == 0) {
            const long before = anix_ticks();
            if (anix_sleep_ticks(10) < 0 || anix_ticks() - before < 10) return 65;
            static const char short_sleep_ok[] = "ANIX_SDK_SHORT_SLEEP_OK\n";
            (void)anix_write(1, short_sleep_ok, sizeof(short_sleep_ok) - 1u);
            exit_status = 33;
        } else if (anix_strcmp(argv[1], "orphan-parent") == 0) {
            if (anix_spawn("/bin/sdk-orphan") <= 1) return 66;
            static const char parent_ok[] = "ANIX_SDK_ORPHAN_PARENT_STARTED\n";
            (void)anix_write(1, parent_ok, sizeof(parent_ok) - 1u);
            exit_status = 35;
            verify_cwd = 0;
        } else if (anix_strcmp(argv[1], "waitpid-ready") == 0) {
            if (!envp[0] || anix_strcmp(envp[0], "ANIX_WAITPID_ENV=32") != 0 || envp[1]) return 30;
            exit_status = 32;
        } else if (anix_strcmp(argv[1], "simd-a") == 0 || anix_strcmp(argv[1], "simd-b") == 0) {
            const int is_a = anix_strcmp(argv[1], "simd-a") == 0;
            if (!verify_simd_context(is_a ? 0x11223344u : 0x55667788u)) return 31;
            static const char simd_ok[] = "ANIX_SDK_SIMD_CONTEXT_OK\n";
            (void)anix_write(1, simd_ok, sizeof(simd_ok) - 1u);
            exit_status = is_a ? 51 : 52;
        } else return 25;
    } else if (argc != 1 || !argv || anix_strcmp(argv[0], "/bin/sdk-worker") != 0 ||
               argv[1] || argv[2]) return 26;
    static const char expected_cwd[] = "/tmp/sdk-dir";
    char cwd[64];
    if (verify_cwd && (anix_getcwd(cwd, sizeof(cwd)) != (long)(sizeof(expected_cwd) - 1u) ||
        anix_memcmp(cwd, expected_cwd, sizeof(expected_cwd)) != 0)) return 24;
    static const char message[] = "ANIX_SDK_WORKER_RAN\n";
    (void)anix_write(1, message, sizeof(message) - 1u);
    return exit_status;
}
