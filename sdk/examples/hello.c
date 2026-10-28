#include <anix/libc.h>
#include <anix/stdlib.h>
#include <anix/stdio.h>
#include <anix/syscall.h>
#include <stdbool.h>

static void descriptor_text(int descriptor, char text[3]) {
    if (descriptor < 10) {
        text[0] = (char)('0' + descriptor);
        text[1] = '\0';
    } else {
        text[0] = (char)('0' + descriptor / 10);
        text[1] = (char)('0' + descriptor % 10);
        text[2] = '\0';
    }
}

int anix_main(int argc, char **argv) {
    static const char argv_ok[] = "ANIX_SDK_ARGV_OK\n";
    static const char expected_argv0[] = "/bin/sdk-hello";
    static const char expected_argument[] = "from-init";
    static const char expected_environment[] = "ANIX_INIT_ENV=present";
    char **envp = argv ? argv + argc + 1 : 0;
    int argv0_matches = argc == 2 && argv && argv[0] && argv[1] && envp && envp[0];
    for (size_t i = 0; argv0_matches && expected_argv0[i]; ++i)
        if (argv[0][i] != expected_argv0[i]) argv0_matches = 0;
    int environment_ok = envp && anix_strcmp(envp[0], expected_environment) == 0;
#if defined(ANIX_ENABLE_STDIN_TEST)
    environment_ok = environment_ok && envp[1] && anix_strcmp(envp[1], "ANIX_TEST_STDIN=1") == 0 && !envp[2];
#else
    environment_ok = environment_ok && !envp[1];
#endif
    if (argv0_matches && argv[0][sizeof(expected_argv0) - 1u] == '\0' &&
        anix_strcmp(argv[1], expected_argument) == 0 && environment_ok)
        (void)anix_write(1, argv_ok, sizeof(argv_ok) - 1u);
    else {
        static const char failed[] = "ANIX_SDK_ARGV_FAILED\n";
        (void)anix_write(1, failed, sizeof(failed) - 1u);
    }
    static const char greeting[] = "anix-sdk: hello from a separately built ring-3 program\n";
    (void)anix_write(1, greeting, sizeof(greeting) - 1u);
    if (anix_getpid() != 1 || anix_getppid() != 0) return 72;
    static const char process_id_ok[] = "ANIX_SDK_PROCESS_ID_OK\n";
    (void)anix_write(1, process_id_ok, sizeof(process_id_ok) - 1u);
    const long initial_group = anix_getpgid(0);
    const long self_group = anix_getpgid(anix_getpid());
    const long group_set = anix_setpgid(0, 0);
    const long group_after_set = anix_getpgid(0);
    const long invalid_group_set = anix_setpgid(-1, 1);
    if (initial_group != 1 || self_group != 1 || group_set != 1 || group_after_set != 1 ||
        invalid_group_set >= 0) return 84;
    int ready_pipe[2], release_pipe[2];
    char ready_writer[3], release_reader[3];
    if (anix_pipe(ready_pipe) < 0 || anix_pipe(release_pipe) < 0) return 85;
    descriptor_text(ready_pipe[1], ready_writer);
    descriptor_text(release_pipe[0], release_reader);
    char *const pgrp_argv[] = { "/bin/sdk-worker", "pgrp-check", ready_writer, release_reader, 0 };
    const long pgrp_child = anix_spawnv("/bin/sdk-worker", pgrp_argv, 0);
    uint64_t pgrp_status = 255;
    char token;
    if (pgrp_child <= 1 || anix_close(ready_pipe[1]) < 0 || anix_close(release_pipe[0]) < 0 ||
        anix_read(ready_pipe[0], &token, 1) != 1 || token != 'R' ||
        anix_setpgid(pgrp_child, pgrp_child) != pgrp_child || anix_getpgid(pgrp_child) != pgrp_child ||
        anix_write(release_pipe[1], "G", 1) != 1 || anix_close(ready_pipe[0]) < 0 ||
        anix_close(release_pipe[1]) < 0 || anix_waitpid(pgrp_child, &pgrp_status) != pgrp_child ||
        pgrp_status != 0) return 85;
    static const char process_group_ok[] = "ANIX_SDK_PROCESS_GROUP_OK\n";
    (void)anix_write(1, process_group_ok, sizeof(process_group_ok) - 1u);
    static const char formatted[] = "ANIX_SDK_STDIO_OK text=hello signed=-42 hex=2a width=0007 char=Z %\n";
    if (anix_printf("ANIX_SDK_STDIO_OK text=%s signed=%d hex=%x width=%04u char=%c %%\n",
                    "hello", -42, 42u, 7u, 'Z') != (long)(sizeof(formatted) - 1u)) return 62;
    static const char duplicate_stream_message[] = "ANIX_SDK_DUP_STDOUT_OK\n";
    int duplicate_stream = (int)anix_dup(1);
    if (duplicate_stream < 3 || anix_write(duplicate_stream, duplicate_stream_message,
            sizeof(duplicate_stream_message) - 1u) != (long)(sizeof(duplicate_stream_message) - 1u) ||
        anix_close(duplicate_stream) < 0) return 71;

    uint8_t *heap = anix_sbrk(0);
    if (heap == (void *)-1 || anix_sbrk(-1) != (void *)-1 || anix_sbrk(0) != heap ||
        anix_sbrk(8193) != heap || anix_sbrk(0) != heap + 8193 ||
        anix_sbrk(-8194) != (void *)-1 || anix_sbrk(0) != heap + 8193 ||
        anix_sbrk(0x7fffffffffffffffL) != (void *)-1 || anix_sbrk(0) != heap + 8193) return 45;
    heap[0] = 0x5a;
    heap[8192] = 0xa5;
    if (anix_yield() < 0 || heap[0] != 0x5a || heap[8192] != 0xa5 || anix_sbrk(-8193) != heap + 8193) return 46;
    if (anix_sbrk(8193) != heap || heap[0] != 0 || heap[8192] != 0 ||
        anix_sbrk(-8193) != heap + 8193) return 47;
    static const char heap_ok[] = "ANIX_SDK_SBRK_OK\n";
    (void)anix_write(1, heap_ok, sizeof(heap_ok) - 1u);

    void *allocator_base = anix_sbrk(0);
    uint8_t *a = anix_malloc(33);
    uint8_t *b = anix_malloc(33);
    uint8_t *c = anix_malloc(33);
    if (allocator_base == (void *)-1 || !a || !b || !c ||
        ((uintptr_t)a & 15u) || ((uintptr_t)b & 15u) || ((uintptr_t)c & 15u) ||
        a == b || b == c || anix_malloc(SIZE_MAX) != 0 ||
        anix_calloc(SIZE_MAX, 2) != 0) return 48;
    a[0] = 0x13;
    b[0] = 0x24;
    c[0] = 0x35;
    anix_free((void *)(uintptr_t)1u);
    anix_free(b);
    uint8_t *reused = anix_malloc(16);
    if (reused != b) return 49;
    reused[0] = 0x46;
    anix_free(reused);
    anix_free(a);
    uint8_t *coalesced = anix_malloc(100);
    if (coalesced != a || coalesced[0] != 0x13) return 50;
    for (size_t i = 0; i < 100; ++i) coalesced[i] = (uint8_t)(i ^ 0x5au);
    uint8_t *resized = anix_realloc(coalesced, 300);
    if (!resized) return 51;
    for (size_t i = 0; i < 100; ++i) if (resized[i] != (uint8_t)(i ^ 0x5au)) return 52;
    uint8_t *shrunk = anix_realloc(resized, 48);
    if (shrunk != resized) return 56;
    uint8_t *zeroed = anix_calloc(64, 1);
    if (!zeroed) return 53;
    for (size_t i = 0; i < 64; ++i) if (zeroed[i] != 0) return 54;
    anix_free(zeroed);
    anix_free(shrunk);
    anix_free(c);
    if (anix_sbrk(0) != allocator_base) return 55;
    static const char allocator_ok[] = "ANIX_SDK_ALLOCATOR_OK\n";
    (void)anix_write(1, allocator_ok, sizeof(allocator_ok) - 1u);

    int fd = (int)anix_open("/etc/motd");
    if (fd < 0) return 1;
    static const char expected_motd[] = "Anix: a small x86-64 kernel under active development.\n";
    struct anix_stat metadata;
    if (anix_stat("/etc/motd", &metadata) < 0 || metadata.type != ANIX_FILE_REGULAR ||
        metadata.size != sizeof(expected_motd) - 1u || (metadata.flags & ANIX_STAT_WRITABLE) ||
        anix_stat("/etc", &metadata) < 0 || metadata.type != ANIX_FILE_DIRECTORY ||
        (metadata.flags & ANIX_STAT_WRITABLE) || anix_stat("/missing", &metadata) >= 0) return 63;
    static const char stat_ok[] = "ANIX_SDK_STAT_OK\n";
    (void)anix_write(1, stat_ok, sizeof(stat_ok) - 1u);
    char buffer[128];
    long count = anix_read(fd, buffer, sizeof(buffer));
    if (count <= 0 || anix_write(1, buffer, (size_t)count) != count) return 2;
    if (anix_close(fd) < 0) return 3;
    static const char file_ok[] = "ANIX_SDK_FILE_IO_OK\n";
    (void)anix_write(1, file_ok, sizeof(file_ok) - 1u);

    if (anix_yield() < 0) return 4;
    static const char yield_ok[] = "ANIX_SDK_YIELD_OK\n";
    (void)anix_write(1, yield_ok, sizeof(yield_ok) - 1u);

    if (anix_create("/etc/sdk-test.txt") >= 0) return 9;
    static const char test_path[] = "/tmp/sdk-test.txt";
    static const char raw_test_data[] = "Anix RAM filesystem write path works.\n";
    static const char test_data[] = "Anix RAM filesystem write path works.\nFormatted descriptor output works.\n";
    static const char formatted_file_data[] = "Formatted descriptor output works.\n";
    int output = (int)anix_create(test_path);
    if (output < 0 || anix_stat(test_path, &metadata) < 0 || metadata.type != ANIX_FILE_REGULAR ||
        metadata.size != 0 || !(metadata.flags & ANIX_STAT_WRITABLE)) return 64;
    if (anix_write(output, raw_test_data, sizeof(raw_test_data) - 1u) !=
        (long)(sizeof(raw_test_data) - 1u) ||
        anix_fprintf(output, "%s", formatted_file_data) != (long)(sizeof(formatted_file_data) - 1u)) return 5;
    if (anix_stat(test_path, &metadata) < 0 || metadata.size != sizeof(test_data) - 1u) return 65;
    int duplicate = (int)anix_dup(output);
    char first_duplicate[4], next_original[4];
    if (duplicate < 0 || duplicate != 4 || anix_seek(output, 0) != 0 ||
        anix_read(duplicate, first_duplicate, sizeof(first_duplicate)) != (long)sizeof(first_duplicate) ||
        anix_memcmp(first_duplicate, "Anix", sizeof(first_duplicate)) != 0 ||
        anix_read(output, next_original, sizeof(next_original)) != (long)sizeof(next_original) ||
        anix_memcmp(next_original, " RAM", sizeof(next_original)) != 0 || anix_close(duplicate) < 0 ||
        anix_read(duplicate, next_original, sizeof(next_original)) >= 0) return 70;
    static const char dup_ok[] = "ANIX_SDK_DUP_OK\n";
    (void)anix_write(1, dup_ok, sizeof(dup_ok) - 1u);
    if (anix_seek(output, 0) != 0) return 6;
    char verify[sizeof(test_data)];
    long verified = anix_read(output, verify, sizeof(verify));
    if (verified != (long)(sizeof(test_data) - 1u) ||
        anix_memcmp(verify, test_data, sizeof(test_data) - 1u) != 0) return 7;
    static const char ramfs_ok[] = "ANIX_SDK_RAMFS_OK\n";
    (void)anix_write(1, ramfs_ok, sizeof(ramfs_ok) - 1u);

    static const char directory_path[] = "/tmp/sdk-dir";
    if (anix_mkdir(directory_path) < 0 || anix_mkdir(directory_path) >= 0 ||
        anix_mkdir("/etc/sdk-dir") >= 0) return 22;
    if (anix_mkdir("/tmp/sdk-dir/empty") < 0 || anix_rmdir("/tmp/sdk-dir/empty") < 0 ||
        anix_mkdir("/tmp/sdk-dir/occupied") < 0 || anix_rmdir("/etc") >= 0) return 35;
    int directory_file = (int)anix_create("/tmp/sdk-dir/occupied/file");
    if (directory_file < 0 || anix_close(directory_file) < 0 ||
        anix_rmdir("/tmp/sdk-dir/occupied") >= 0 ||
        anix_unlink("/tmp/sdk-dir/occupied/file") < 0 ||
        anix_rmdir("/tmp/sdk-dir/occupied") < 0) return 36;
    if (anix_chdir(directory_path) < 0 || anix_chdir("missing") >= 0) return 27;
    if (anix_mkdir("held") < 0 || anix_chdir("held") < 0 || anix_rmdir(".") >= 0 ||
        anix_chdir("..") < 0 || anix_rmdir("held") < 0) return 39;
    static const char expected_cwd[] = "/tmp/sdk-dir";
    char cwd[64];
    if (anix_getcwd(cwd, sizeof(cwd)) != (long)(sizeof(expected_cwd) - 1u) ||
        anix_memcmp(cwd, expected_cwd, sizeof(expected_cwd)) != 0) return 28;
    char small_cwd[2];
    if (anix_getcwd(small_cwd, sizeof(small_cwd)) >= 0) return 29;
    static const char cwd_ok[] = "ANIX_SDK_CWD_OK\n";
    (void)anix_write(1, cwd_ok, sizeof(cwd_ok) - 1u);
    if (output != 3 || anix_seek(output, 0) != 0) return 58;
    char inherited_fd[] = "3";
    char *const inherit_argv[] = { "/bin/sdk-worker", "fd-check", inherited_fd, 0 };
    long inherit_child = anix_spawnv("/bin/sdk-worker", inherit_argv, 0);
    uint64_t inherit_status = 0;
    if (inherit_child < 0 || anix_waitpid(inherit_child, &inherit_status) != inherit_child || inherit_status != 59u)
        return 60;
    char shared_offset[4];
    if (anix_read(output, shared_offset, sizeof(shared_offset)) != (long)sizeof(shared_offset) ||
        anix_memcmp(shared_offset, " RAM", sizeof(shared_offset)) != 0) return 61;
    if (anix_unlink(test_path) < 0 || anix_open(test_path) >= 0) return 10;
    if (anix_seek(output, 0) != 0 ||
        anix_read(output, verify, sizeof(verify)) != (long)(sizeof(test_data) - 1u) ||
        anix_memcmp(verify, test_data, sizeof(test_data) - 1u) != 0) return 11;
    if (anix_close(output) < 0) return 8;
    static const char inherited_ok[] = "ANIX_SDK_FD_INHERIT_OK\n";
    (void)anix_write(1, inherited_ok, sizeof(inherited_ok) - 1u);
#if defined(ANIX_ENABLE_STDIN_TEST)
    char *const stdin_argv[] = { "/bin/sdk-worker", "stdin-check", 0 };
    long stdin_child = anix_spawnv("/bin/sdk-worker", stdin_argv, 0);
    uint64_t stdin_status = 0;
    if (stdin_child < 0 || anix_waitpid(stdin_child, &stdin_status) != stdin_child || stdin_status != 58u)
        return 57;
    static const char stdin_ok[] = "ANIX_SDK_STDIN_OK\n";
    (void)anix_write(1, stdin_ok, sizeof(stdin_ok) - 1u);
#endif
    static const char nested_path[] = "nested.txt";
    static const char nested_data[] = "Nested VFS paths work.\n";
    int nested = (int)anix_create(nested_path);
    if (nested < 0 || anix_write_file(nested, nested_data, sizeof(nested_data) - 1u) !=
        (long)(sizeof(nested_data) - 1u) || anix_seek(nested, 0) != 0) return 23;
    if (anix_close(nested) < 0 ||
        anix_open_mode(nested_path, ANIX_OPEN_APPEND) >= 0) return 24;
    int readonly = (int)anix_open(nested_path);
    if (readonly < 0 || anix_write_file(readonly, "x", 1u) >= 0 || anix_close(readonly) < 0) return 30;
    nested = (int)anix_open_mode(nested_path, ANIX_OPEN_WRITE | ANIX_OPEN_APPEND);
    static const char appended_data[] = "Append works.\n";
    static const char combined_data[] = "Nested VFS paths work.\nAppend works.\n";
    if (nested < 0 || anix_write_file(nested, appended_data, sizeof(appended_data) - 1u) !=
        (long)(sizeof(appended_data) - 1u) || anix_seek(nested, 0) != 0) return 31;
    char nested_verify[sizeof(combined_data)];
    if (anix_read(nested, nested_verify, sizeof(nested_verify)) != (long)(sizeof(combined_data) - 1u) ||
        anix_memcmp(nested_verify, combined_data, sizeof(combined_data) - 1u) != 0) return 32;
    char directory_listing[32];
    static const char expected_listing[] = "nested.txt\n";
    if (anix_list(".", directory_listing, sizeof(directory_listing)) !=
        (long)(sizeof(expected_listing) - 1u) ||
        anix_memcmp(directory_listing, expected_listing, sizeof(expected_listing) - 1u) != 0) return 26;
    int parent_relative = (int)anix_open("../sdk-dir/nested.txt");
    if (parent_relative < 0 || anix_close(parent_relative) < 0 ||
        anix_close(nested) < 0 || anix_unlink(nested_path) < 0) return 25;
    if (anix_rmdir(".") >= 0) return 37;
    static const char nested_ok[] = "ANIX_SDK_NESTED_VFS_OK\n";
    (void)anix_write(1, nested_ok, sizeof(nested_ok) - 1u);

    static const char persistent_path[] = "/persist/sdk-persistent.txt";
    static const char persistent_data[] = "Anix disk-backed VFS survived a restart.\n";
    static const char updated_persistent_data[] = "Anix persistent files can be reopened and rewritten.\n";
    const bool persistent_mounted = anix_stat("/persist", &metadata) == 0;
    if (persistent_mounted) {
        if (metadata.type != ANIX_FILE_DIRECTORY || !(metadata.flags & ANIX_STAT_PERSISTENT) ||
            !(metadata.flags & ANIX_STAT_WRITABLE)) return 66;
        static const char persistent_stat_ok[] = "ANIX_SDK_PERSIST_STAT_OK\n";
        (void)anix_write(1, persistent_stat_ok, sizeof(persistent_stat_ok) - 1u);
    }
    int persistent = (int)anix_open(persistent_path);
    if (persistent >= 0) {
        char saved[sizeof(updated_persistent_data)];
        long saved_size = anix_read(persistent, saved, sizeof(saved));
        if (!persistent_mounted || anix_stat(persistent_path, &metadata) < 0 ||
            metadata.type != ANIX_FILE_REGULAR || !(metadata.flags & ANIX_STAT_PERSISTENT) ||
            !(metadata.flags & ANIX_STAT_WRITABLE) || metadata.size != (uint64_t)saved_size) return 67;
        if (anix_close(persistent) < 0) return 13;
        if (saved_size == (long)(sizeof(persistent_data) - 1u) &&
            anix_memcmp(saved, persistent_data, sizeof(persistent_data) - 1u) == 0) {
            persistent = (int)anix_open_mode(persistent_path, ANIX_OPEN_WRITE | ANIX_OPEN_TRUNCATE);
            if (persistent < 0 || anix_write_file(persistent, updated_persistent_data,
                    sizeof(updated_persistent_data) - 1u) != (long)(sizeof(updated_persistent_data) - 1u) ||
                anix_seek(persistent, 0) != 0) return 33;
            char verify_updated[sizeof(updated_persistent_data)];
            if (anix_read(persistent, verify_updated, sizeof(verify_updated)) !=
                    (long)(sizeof(updated_persistent_data) - 1u) ||
                anix_memcmp(verify_updated, updated_persistent_data, sizeof(updated_persistent_data) - 1u) != 0 ||
                anix_close(persistent) < 0) return 34;
            static const char persistent_directory[] = "/persist/sdk-dir";
            static const char persistent_file[] = "/persist/sdk-dir/nested.txt";
            static const char persistent_nested_data[] = "Persistent directories survive reboot.\n";
            char persistent_listing[32];
            static const char persistent_expected_listing[] = "nested.txt\n";
            if (anix_list(persistent_directory, persistent_listing, sizeof(persistent_listing)) !=
                    (long)(sizeof(persistent_expected_listing) - 1u) ||
                anix_memcmp(persistent_listing, persistent_expected_listing,
                    sizeof(persistent_expected_listing) - 1u) != 0) return 44;
            int nested_persistent = (int)anix_open(persistent_file);
            char nested_persistent_verify[sizeof(persistent_nested_data)];
            if (nested_persistent < 0 || anix_read(nested_persistent, nested_persistent_verify,
                    sizeof(nested_persistent_verify)) != (long)(sizeof(persistent_nested_data) - 1u) ||
                anix_memcmp(nested_persistent_verify, persistent_nested_data,
                    sizeof(persistent_nested_data) - 1u) != 0 || anix_close(nested_persistent) < 0 ||
                anix_rmdir(persistent_directory) >= 0 || anix_unlink(persistent_file) < 0 ||
                anix_rmdir(persistent_directory) < 0) return 42;
            static const char persistent_directory_ok[] = "ANIX_SDK_PERSIST_DIR_OK\n";
            (void)anix_write(1, persistent_directory_ok, sizeof(persistent_directory_ok) - 1u);
            static const char rewritten[] = "ANIX_SDK_PERSIST_REWRITTEN\n";
            (void)anix_write(1, rewritten, sizeof(rewritten) - 1u);
        } else if (saved_size == (long)(sizeof(updated_persistent_data) - 1u) &&
                   anix_memcmp(saved, updated_persistent_data, sizeof(updated_persistent_data) - 1u) == 0) {
            static const char persistent_directory[] = "/persist/sdk-dir";
            char missing_directory[32];
            if (anix_list(persistent_directory, missing_directory, sizeof(missing_directory)) >= 0) return 43;
            static const char restored[] = "ANIX_SDK_PERSIST_UPDATED_RESTORED\n";
            (void)anix_write(1, restored, sizeof(restored) - 1u);
        } else return 12;
    } else {
        if (persistent_mounted && anix_stat(persistent_path, &metadata) == 0) return 68;
        persistent = (int)anix_create(persistent_path);
        if (persistent >= 0) {
            if (anix_stat(persistent_path, &metadata) < 0 || metadata.type != ANIX_FILE_REGULAR ||
                metadata.size != 0 || !(metadata.flags & ANIX_STAT_PERSISTENT) ||
                !(metadata.flags & ANIX_STAT_WRITABLE)) return 69;
            if (anix_write_file(persistent, persistent_data, sizeof(persistent_data) - 1u) !=
                (long)(sizeof(persistent_data) - 1u) || anix_close(persistent) < 0) return 14;
            static const char persistent_directory[] = "/persist/sdk-dir";
            static const char persistent_file[] = "/persist/sdk-dir/nested.txt";
            static const char persistent_nested_data[] = "Persistent directories survive reboot.\n";
            if (anix_mkdir(persistent_directory) < 0) return 40;
            int nested_persistent = (int)anix_create(persistent_file);
            if (nested_persistent < 0 || anix_write_file(nested_persistent, persistent_nested_data,
                    sizeof(persistent_nested_data) - 1u) != (long)(sizeof(persistent_nested_data) - 1u) ||
                anix_close(nested_persistent) < 0) return 41;
            static const char created[] = "ANIX_SDK_PERSIST_CREATED\n";
            (void)anix_write(1, created, sizeof(created) - 1u);
        } else {
            static const char unavailable[] = "ANIX_SDK_PERSIST_UNAVAILABLE\n";
            (void)anix_write(1, unavailable, sizeof(unavailable) - 1u);
        }
    }
    if (anix_spawn("/bin/no-such-program") >= 0) return 16;
    static const char spawn_failure_ok[] = "ANIX_SDK_SPAWN_FAILURE_OK\n";
    (void)anix_write(1, spawn_failure_ok, sizeof(spawn_failure_ok) - 1u);
    char *const invalid_exec_argv[] = { "/bin/no-such-program", 0 };
    if (anix_execv("/bin/no-such-program", invalid_exec_argv, 0) >= 0) return 29;
    static const char exec_failure_ok[] = "ANIX_SDK_EXEC_FAILURE_OK\n";
    (void)anix_write(1, exec_failure_ok, sizeof(exec_failure_ok) - 1u);
    if (anix_spawn("/etc/not-an-elf") >= 0) return 47;
    char *const malformed_exec_argv[] = { "/etc/not-an-elf", 0 };
    if (anix_execv("/etc/not-an-elf", malformed_exec_argv, 0) >= 0) return 48;
    static const char malformed_elf_ok[] = "ANIX_SDK_MALFORMED_ELF_REJECTED\n";
    (void)anix_write(1, malformed_elf_ok, sizeof(malformed_elf_ok) - 1u);
    for (unsigned i = 0; i < 8; ++i) {
        char *const worker_argv[] = { "/bin/sdk-worker", "worker-argument", 0 };
        char *const worker_envp[] = { "ANIX_WORKER_ENV=present", 0 };
        long child = i == 0 ? anix_spawnv("/bin/sdk-worker", worker_argv, worker_envp) :
                              anix_spawn("/bin/sdk-worker");
        if (child <= 0) return 15;
        /* Alternate blocking waits with waits after the child has exited. */
        if ((i & 1u) && anix_yield() < 0) return 18;
        if (anix_wait(child) != 23) return 19;
    }
    char *const blocking_wait_argv[] = { "/bin/sdk-worker", "waitpid-block", 0 };
    char *const blocking_wait_envp[] = { "ANIX_WAITPID_ENV=31", 0 };
    long blocking_child = anix_spawnv("/bin/sdk-worker", blocking_wait_argv, blocking_wait_envp);
    char *const short_sleep_argv[] = { "/bin/sdk-worker", "sleep-short", 0 };
    long short_sleep_child = anix_spawnv("/bin/sdk-worker", short_sleep_argv, 0);
    uint64_t child_status = 0;
    if (blocking_child <= 0 || short_sleep_child <= 0 ||
        anix_waitpid(-1, &child_status) != short_sleep_child || child_status != 33 ||
        anix_waitpid(blocking_child, &child_status) != blocking_child || child_status != 31) return 42;
    char *const ready_wait_argv[] = { "/bin/sdk-worker", "waitpid-ready", 0 };
    char *const ready_wait_envp[] = { "ANIX_WAITPID_ENV=32", 0 };
    long ready_child = anix_spawnv("/bin/sdk-worker", ready_wait_argv, ready_wait_envp);
    if (ready_child <= 0 || anix_yield() < 0 ||
        anix_waitpid(ready_child, &child_status) != ready_child || child_status != 32) return 43;
    static const char waitpid_ok[] = "ANIX_SDK_WAITPID_OK\n";
    (void)anix_write(1, waitpid_ok, sizeof(waitpid_ok) - 1u);
    int pipe_descriptors[2];
    char pipe_reader_fd[3], pipe_writer_fd[3];
    if (anix_pipe(pipe_descriptors) < 0 || pipe_descriptors[0] < 3 || pipe_descriptors[1] < 3) return 73;
    descriptor_text(pipe_descriptors[0], pipe_reader_fd);
    descriptor_text(pipe_descriptors[1], pipe_writer_fd);
    char *const pipe_argv[] = { "/bin/sdk-pipe-reader", "stream", pipe_reader_fd, pipe_writer_fd, 0 };
    long pipe_child = anix_spawnv("/bin/sdk-pipe-reader", pipe_argv, 0);
    if (pipe_child <= 1 || anix_close(pipe_descriptors[0]) < 0 || anix_yield() < 0 ||
        anix_write(pipe_descriptors[1], "ping", 4u) != 4 || anix_close(pipe_descriptors[1]) < 0 ||
        anix_waitpid(pipe_child, &child_status) != pipe_child || child_status != 0) return 74;
    static const char pipe_ok[] = "ANIX_SDK_PIPE_BLOCKING_EOF_OK\n";
    (void)anix_write(1, pipe_ok, sizeof(pipe_ok) - 1u);

    if (anix_pipe(pipe_descriptors) < 0) return 75;
    descriptor_text(pipe_descriptors[0], pipe_reader_fd);
    descriptor_text(pipe_descriptors[1], pipe_writer_fd);
    char *const bulk_pipe_argv[] = { "/bin/sdk-pipe-reader", "bulk", pipe_reader_fd, pipe_writer_fd, 0 };
    pipe_child = anix_spawnv("/bin/sdk-pipe-reader", bulk_pipe_argv, 0);
    static char pipe_payload[4096];
    for (size_t index = 0; index < sizeof(pipe_payload); ++index) pipe_payload[index] = 'A';
    if (pipe_child <= 1 || anix_write(pipe_descriptors[1], pipe_payload, sizeof(pipe_payload)) !=
            (long)sizeof(pipe_payload) || anix_close(pipe_descriptors[0]) < 0 ||
        anix_write(pipe_descriptors[1], "B", 1u) != 1 || anix_close(pipe_descriptors[1]) < 0 ||
        anix_waitpid(pipe_child, &child_status) != pipe_child || child_status != 0) return 76;
    static const char backpressure_ok[] = "ANIX_SDK_PIPE_BACKPRESSURE_OK\n";
    (void)anix_write(1, backpressure_ok, sizeof(backpressure_ok) - 1u);

    if (anix_pipe(pipe_descriptors) < 0) return 77;
    descriptor_text(pipe_descriptors[0], pipe_reader_fd);
    descriptor_text(pipe_descriptors[1], pipe_writer_fd);
    char *const drop_pipe_argv[] = { "/bin/sdk-pipe-reader", "drop", pipe_reader_fd, pipe_writer_fd, 0 };
    pipe_child = anix_spawnv("/bin/sdk-pipe-reader", drop_pipe_argv, 0);
    if (pipe_child <= 1 || anix_close(pipe_descriptors[0]) < 0 ||
        anix_waitpid(pipe_child, &child_status) != pipe_child || child_status != 0 ||
        anix_write(pipe_descriptors[1], "x", 1u) >= 0 || anix_close(pipe_descriptors[1]) < 0) return 78;
    static const char broken_pipe_ok[] = "ANIX_SDK_PIPE_BROKEN_WRITE_OK\n";
    (void)anix_write(1, broken_pipe_ok, sizeof(broken_pipe_ok) - 1u);
    const int saved_stdin = (int)anix_dup(0);
    if (saved_stdin < 3 || anix_pipe(pipe_descriptors) < 0 || anix_dup2(pipe_descriptors[0], 0) != 0)
        return 79;
    descriptor_text(pipe_descriptors[1], pipe_writer_fd);
    char *const stdin_pipe_argv[] = { "/bin/sdk-pipe-reader", "stdin", pipe_writer_fd, 0 };
    pipe_child = anix_spawnv("/bin/sdk-pipe-reader", stdin_pipe_argv, 0);
    if (pipe_child <= 1 || anix_dup2(saved_stdin, 0) != 0 || anix_close(saved_stdin) < 0 ||
        anix_close(pipe_descriptors[0]) < 0 || anix_write(pipe_descriptors[1], "ping", 4u) != 4 ||
        anix_close(pipe_descriptors[1]) < 0 || anix_waitpid(pipe_child, &child_status) != pipe_child ||
        child_status != 0) return 80;
    static const char stdin_redirect_ok[] = "ANIX_SDK_DUP2_STDIN_PIPE_OK\n";
    (void)anix_write(1, stdin_redirect_ok, sizeof(stdin_redirect_ok) - 1u);

    const int saved_stdout = (int)anix_dup(1);
    if (saved_stdout < 3 || anix_pipe(pipe_descriptors) < 0 || anix_dup2(pipe_descriptors[1], 1) != 1)
        return 81;
    char *const stdout_pipe_argv[] = { "/bin/sdk-pipe-reader", "stdout", 0 };
    pipe_child = anix_spawnv("/bin/sdk-pipe-reader", stdout_pipe_argv, 0);
    if (pipe_child <= 1 || anix_dup2(saved_stdout, 1) != 1 || anix_close(saved_stdout) < 0 ||
        anix_close(pipe_descriptors[1]) < 0) return 82;
    char redirected[4];
    if (anix_read(pipe_descriptors[0], redirected, sizeof(redirected)) != 4 ||
        anix_memcmp(redirected, "pong", 4u) != 0 || anix_read(pipe_descriptors[0], redirected, sizeof(redirected)) != 0 ||
        anix_close(pipe_descriptors[0]) < 0 || anix_waitpid(pipe_child, &child_status) != pipe_child ||
        child_status != 0) return 83;
    static const char stdout_redirect_ok[] = "ANIX_SDK_DUP2_STDOUT_PIPE_OK\n";
    (void)anix_write(1, stdout_redirect_ok, sizeof(stdout_redirect_ok) - 1u);
    char *const simd_a_argv[] = { "/bin/sdk-worker", "simd-a", 0 };
    char *const simd_b_argv[] = { "/bin/sdk-worker", "simd-b", 0 };
    long simd_a = anix_spawnv("/bin/sdk-worker", simd_a_argv, 0);
    long simd_b = anix_spawnv("/bin/sdk-worker", simd_b_argv, 0);
    if (simd_a <= 0 || simd_b <= 0 || anix_waitpid(simd_a, &child_status) != simd_a || child_status != 51 ||
        anix_waitpid(simd_b, &child_status) != simd_b || child_status != 52) return 44;
    static const char simd_ok[] = "ANIX_SDK_SIMD_SCHEDULING_OK\n";
    (void)anix_write(1, simd_ok, sizeof(simd_ok) - 1u);
    if (anix_chdir("/tmp") < 0 || anix_rmdir("sdk-dir") < 0 || anix_chdir("/") < 0) return 38;
    static const char rmdir_ok[] = "ANIX_SDK_RMDIR_OK\n";
    (void)anix_write(1, rmdir_ok, sizeof(rmdir_ok) - 1u);
    static const char process_ok[] = "ANIX_SDK_PROCESS_WAIT_OK\n";
    (void)anix_write(1, process_ok, sizeof(process_ok) - 1u);
    char *const orphan_parent_argv[] = { "/bin/sdk-worker", "orphan-parent", 0 };
    long orphan_parent = anix_spawnv("/bin/sdk-worker", orphan_parent_argv, 0);
    uint64_t orphan_parent_status = 0;
    if (orphan_parent <= 1 || anix_waitpid(orphan_parent, &orphan_parent_status) != orphan_parent ||
        orphan_parent_status != 35u) return 17;
    uint64_t adopted_status = 0;
    long adopted_child = anix_waitpid(-1, &adopted_status);
    if (adopted_child <= 1 || adopted_child == orphan_parent || adopted_status != 42u) return 18;
    static const char reparent_ok[] = "ANIX_SDK_REPARENT_OK\n";
    (void)anix_write(1, reparent_ok, sizeof(reparent_ok) - 1u);
    /* Exec a second image at the same link address, then launch the preserved shell. */
    char *const exec_argv[] = { "/bin/sdk-worker", "exec-check", 0 };
    char *const exec_envp[] = { "ANIX_EXEC_ENV=present", 0 };
    if (anix_execv("/bin/sdk-worker", exec_argv, exec_envp) < 0) return 1;
    return 0;
}
