#include "../userabi.h"

#define COLOR_RESET "\033[0m"
#define COLOR_BOLD_GREEN "\033[1;32m"
#define COLOR_BLUE "\033[34m"
#define COLOR_YELLOW "\033[33m"
#define COLOR_BOLD_RED "\033[1;31m"
#define ARGUMENT_MAX 8u
#define COMMAND_MAX 12u

static const char *const command_names[COMMAND_MAX] = {
    "cat", "cd", "clear", "echo", "exit", "help", "ls", "pwd", "shutdown", "status", "ticks", "uname"
};

static int equal(const char *left, const char *right) {
    size_t i = 0;
    while (left[i] && right[i] && left[i] == right[i]) ++i;
    return left[i] == right[i];
}

static void color_puts(const char *color, const char *text) {
    anix_puts(color);
    anix_puts(text);
    anix_puts(COLOR_RESET);
}

static int starts_with(const char *text, const char *prefix) {
    size_t i = 0;
    while (prefix[i] && text[i] == prefix[i]) ++i;
    return prefix[i] == '\0';
}

static void redraw_input(const char *line, size_t length, size_t old_length) {
    anix_puts("\ranixsh> ");
    (void)anix_write(line, length);
    while (old_length > length) { anix_puts(" "); --old_length; }
    anix_puts("\ranixsh> ");
    (void)anix_write(line, length);
}

static int complete_command(char *line, size_t *length, char prefix[32],
                            int *last_match, int *completion_active) {
    size_t start = 0;
    while (line[start] == ' ' || line[start] == '\t') ++start;
    size_t end = start;
    while (line[end] && line[end] != ' ' && line[end] != '\t') ++end;
    if (!*completion_active) {
        const size_t prefix_length = end - start;
        if (prefix_length >= 32u) return 0;
        for (size_t i = 0; i < prefix_length; ++i) prefix[i] = line[start + i];
        prefix[prefix_length] = '\0';
        *completion_active = 1;
        *last_match = -1;
    }
    int selected = -1;
    for (size_t step = 1; step <= COMMAND_MAX; ++step) {
        const size_t index = *last_match < 0 ? step - 1u : ((size_t)*last_match + step) % COMMAND_MAX;
        if (starts_with(command_names[index], prefix)) { selected = (int)index; break; }
    }
    if (selected < 0) return 0;
    const size_t replacement_length = anix_len(command_names[selected]);
    const size_t suffix_length = *length - end;
    const size_t new_length = start + replacement_length + suffix_length;
    if (new_length >= 128u) return 0;
    char suffix[128];
    for (size_t i = 0; i <= suffix_length; ++i) suffix[i] = line[end + i];
    for (size_t i = 0; i <= suffix_length; ++i)
        line[start + replacement_length + i] = suffix[i];
    for (size_t i = 0; i < replacement_length; ++i) line[start + i] = command_names[selected][i];
    *length = new_length;
    *last_match = selected;
    redraw_input(line, *length, new_length < end + suffix_length ? end + suffix_length : new_length);
    return 1;
}

static long read_command_line(char *line, size_t capacity) {
    static int discard_lf_after_cr;
    if (!line || capacity < 2u) return -1;
    size_t length = 0;
    char completion_prefix[32] = {0};
    int last_match = -1;
    int completion_active = 0;
    line[0] = '\0';
    for (;;) {
        const long input = anix_read_char();
        if (input < 0) return -1;
        const char character = (char)input;
        if (discard_lf_after_cr && character == '\n') { discard_lf_after_cr = 0; continue; }
        discard_lf_after_cr = 0;
        if (character == '\r' || character == '\n') {
            if (character == '\r') discard_lf_after_cr = 1;
            line[length] = '\0';
            anix_puts("\n");
            return (long)length;
        }
        if (character == '\t') {
            if (!complete_command(line, &length, completion_prefix, &last_match, &completion_active))
                anix_puts("\a");
            continue;
        }
        if (character == '\b' || (unsigned char)character == 0x7fu) {
            completion_active = 0;
            if (length) { line[--length] = '\0'; anix_puts("\b \b"); }
            continue;
        }
        if ((unsigned char)character == 0x03u) {
            line[0] = '\0';
            anix_puts("^C\n");
            return 0;
        }
        if ((unsigned char)character == 0x0cu) {
            anix_puts("\033[2J\033[H");
            redraw_input(line, length, length);
            continue;
        }
        if ((unsigned char)character < 0x20u) continue;
        completion_active = 0;
        if (length + 1u >= capacity) { anix_puts("\a"); continue; }
        line[length++] = character;
        line[length] = '\0';
        (void)anix_write(&character, 1u);
    }
}

/* Split a command in place; single/double quotes and backslash escapes are supported. */
static long parse_arguments(char *line, char *arguments[ARGUMENT_MAX]) {
    char *read = line;
    char *write = line;
    long count = 0;
    while (*read) {
        while (*read == ' ' || *read == '\t') ++read;
        if (!*read) break;
        if ((unsigned long)count >= ARGUMENT_MAX) return -1;
        arguments[count++] = write;
        char quote = 0;
        while (*read) {
            char c = *read++;
            if (!quote && (c == ' ' || c == '\t')) break;
            if (c == '\\' && *read) {
                *write++ = *read++;
                continue;
            }
            if (c == '\\' && !*read) return -1;
            if (quote) {
                if (c == quote) quote = 0;
                else *write++ = c;
            } else if (c == '\'' || c == '"') quote = c;
            else *write++ = c;
        }
        if (quote) return -1;
        *write++ = '\0';
    }
    return count;
}

static void print_decimal(uint64_t value) {
    char digits[24];
    size_t count = 0;
    do {
        digits[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value && count < sizeof(digits));
    while (count) {
        char c = digits[--count];
        (void)anix_write(&c, 1u);
    }
}

static void print_ticks(void) {
    color_puts(COLOR_YELLOW, "ticks: ");
    print_decimal((uint64_t)anix_ticks());
    anix_puts("\n");
}

static void print_status(void) {
    anix_puts("pid: ");
    print_decimal((uint64_t)anix_getpid());
    anix_puts("  pgrp: ");
    const long group = anix_getpgid(0);
    if (group < 0) color_puts(COLOR_BOLD_RED, "unavailable");
    else print_decimal((uint64_t)group);
    anix_puts("\n");
}

static void print_listing(const char *buffer, size_t length) {
    size_t start = 0;
    for (size_t i = 0; i < length; ++i) {
        if (buffer[i] != '\n') continue;
        size_t item_length = i - start;
        if (item_length && buffer[i - 1u] == '/') {
            anix_puts(COLOR_BLUE);
            (void)anix_write(buffer + start, item_length);
            anix_puts(COLOR_RESET);
        } else (void)anix_write(buffer + start, item_length);
        anix_puts("\n");
        start = i + 1u;
    }
    if (start < length) (void)anix_write(buffer + start, length - start);
}

static void command_ls(long argc, char *arguments[ARGUMENT_MAX], char *output, size_t capacity) {
    if (argc > 2) { color_puts(COLOR_BOLD_RED, "usage: ls [directory]\n"); return; }
    const char *path = argc == 2 ? arguments[1] : ".";
    const long count = anix_list(path, output, capacity);
    if (count < 0) color_puts(COLOR_BOLD_RED, "ls: directory unavailable\n");
    else print_listing(output, (size_t)count);
}

static void command_cat(long argc, char *arguments[ARGUMENT_MAX], char *output, size_t capacity) {
    if (argc < 2) { color_puts(COLOR_BOLD_RED, "usage: cat file [file ...]\n"); return; }
    for (long i = 1; i < argc; ++i) {
        const long fd = anix_open(arguments[i]);
        if (fd < 0) {
            anix_puts("cat: cannot open "); anix_puts(arguments[i]); anix_puts("\n");
            continue;
        }
        long count;
        while ((count = anix_read_file(fd, output, capacity)) > 0)
            (void)anix_write(output, (size_t)count);
        if (count < 0) { anix_puts("cat: read failed: "); anix_puts(arguments[i]); anix_puts("\n"); }
        (void)anix_close(fd);
    }
}

static int run_external(char *arguments[ARGUMENT_MAX + 1u], long argc) {
    if (!argc) return 0;
    char path[128];
    size_t length = 0;
    int has_slash = 0;
    for (size_t i = 0; arguments[0][i]; ++i) {
        if (arguments[0][i] == '/') has_slash = 1;
        if (length + (has_slash ? 0u : 5u) + 1u >= sizeof(path)) return 0;
    }
    if (!has_slash) {
        path[length++] = '/'; path[length++] = 'b'; path[length++] = 'i';
        path[length++] = 'n'; path[length++] = '/';
    }
    for (size_t i = 0; arguments[0][i]; ++i) path[length++] = arguments[0][i];
    path[length] = '\0';

    arguments[argc] = 0;
    char *const environment[] = { 0 };
    const long child = anix_spawnv(path, arguments, environment);
    if (child < 0) return 0;
    uint64_t status = 0;
    if (anix_waitpid(child, &status) < 0) {
        color_puts(COLOR_BOLD_RED, "anixsh: failed waiting for program\n");
        return 1;
    }
    return 1;
}

static void execute_command(char *arguments[ARGUMENT_MAX + 1u], long argc,
                            char *output, size_t capacity) {
    if (!argc) return;
    const char *command = arguments[0];
    if (equal(command, "help")) {
        color_puts(COLOR_BOLD_GREEN,
            "commands: help, ticks, status, pwd, cd [directory], ls [directory], cat file..., echo [text...], clear, uname, shutdown [--now], exit; run programs by name from /bin or by path (Tab cycles built-ins)\n");
    } else if (equal(command, "ticks") && argc == 1) print_ticks();
    else if (equal(command, "status") && argc == 1) print_status();
    else if (equal(command, "pwd") && argc == 1) {
        char path[256];
        const long length = anix_getcwd(path, sizeof(path));
        if (length < 0) color_puts(COLOR_BOLD_RED, "pwd: failed\n");
        else { anix_puts("cwd: "); (void)anix_write(path, (size_t)length); anix_puts("\n"); }
    } else if (equal(command, "cd")) {
        if (argc > 2) color_puts(COLOR_BOLD_RED, "usage: cd [directory]\n");
        else if (anix_chdir(argc == 2 ? arguments[1] : "/") < 0)
            color_puts(COLOR_BOLD_RED, "cd: directory unavailable\n");
    } else if (equal(command, "ls")) command_ls(argc, arguments, output, capacity);
    else if (equal(command, "cat")) command_cat(argc, arguments, output, capacity);
    else if (equal(command, "echo")) {
        for (long i = 1; i < argc; ++i) {
            if (i > 1) anix_puts(" ");
            anix_puts(arguments[i]);
        }
        anix_puts("\n");
    } else if (equal(command, "clear") && argc == 1) anix_puts("\033[0m\033[2J\033[H");
    else if (equal(command, "uname") && argc == 1) anix_puts("Anix kernel x86_64\n");
    else if (equal(command, "shutdown")) {
        if (argc > 2 || (argc == 2 && !equal(arguments[1], "--now"))) {
            color_puts(COLOR_BOLD_RED, "usage: shutdown [--now]\n");
        } else {
            anix_puts(argc == 2 ? "shutdown: powering off now\n" : "shutdown: powering off\n");
            if (anix_poweroff() < 0)
                color_puts(COLOR_BOLD_RED, "shutdown: ACPI poweroff is unavailable on this platform\n");
        }
    }
    else if (equal(command, "exit") && argc == 1) {
        anix_puts("anixsh: exiting\n");
        anix_exit(0);
    } else if (equal(command, "exit")) color_puts(COLOR_BOLD_RED, "usage: exit\n");
    else if (equal(command, "ticks") || equal(command, "status") || equal(command, "pwd") ||
             equal(command, "clear") || equal(command, "uname"))
        color_puts(COLOR_BOLD_RED, "usage: command takes no arguments\n");
    else if (!run_external(arguments, argc)) {
        anix_puts("command not found: "); anix_puts(command); anix_puts("\n");
    }
}

long user_main(void) {
    char line[128], output[2048];
    char *arguments[ARGUMENT_MAX + 1u];
    anix_puts("anixsh: ring 3 userspace ready\nType help for commands; Tab cycles command names.\n");
    for (;;) {
        anix_puts("anixsh> ");
        if (read_command_line(line, sizeof(line)) < 0) {
            color_puts(COLOR_BOLD_RED, "read failed\n");
            continue;
        }
        const long argc = parse_arguments(line, arguments);
        if (argc < 0) color_puts(COLOR_BOLD_RED, "syntax error: unmatched quote, escape, or too many arguments\n");
        else execute_command(arguments, argc, output, sizeof(output));
    }
}
