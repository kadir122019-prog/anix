#include <anix/pmm.h>
#include <anix/serial.h>
#include <anix/shell.h>
#include <anix/terminal.h>
#include <anix/timer.h>
#include <anix/string.h>
#include <anix/scheduler.h>
#include <anix/vfs.h>

static bool equals(const char *left, const char *right) {
    const size_t length = kstrlen(right);
    return kstrlen(left) == length && kmemcmp(left, right, length) == 0;
}

static bool print_node(const struct vfs_node *node, void *context) {
    (void)context;
    serial_write(node->name);
    if (node->type == VFS_DIRECTORY) serial_write("/");
    serial_write("\n");
    return true;
}

static void print_file(const char *path) {
    const struct vfs_node *node = vfs_lookup(path);
    if (!node || node->type != VFS_REGULAR_FILE) {
        serial_write("cat: file not found: "); serial_write(path); serial_write("\n");
        return;
    }
    char chunk[128];
    uint64_t offset = 0;
    while (offset < node->size) {
        const size_t amount = vfs_read(node, offset, chunk, sizeof(chunk) - 1u);
        if (!amount) break;
        chunk[amount] = '\0';
        serial_write(chunk);
        offset += amount;
    }
}

void shell_run(void) {
    char line[128];
    for (;;) {
        serial_write("anix> ");
        if (!terminal_read_line(line, sizeof(line))) continue;
        if (equals(line, "help")) {
            terminal_writeln("help  ticks  memory  tasks  ls  cat PATH  clear  uname  halt");
        } else if (equals(line, "ticks")) {
            serial_write("timer ticks: "); serial_write_dec(timer_ticks()); serial_write("\n");
        } else if (equals(line, "tasks")) {
            serial_write("kernel tasks: "); serial_write_dec(scheduler_task_count());
            serial_write(", demo cycles: "); serial_write_dec(scheduler_demo_count()); serial_write("\n");
        } else if (equals(line, "memory")) {
            serial_write("physical pages free: "); serial_write_dec(pmm_free_page_count());
            serial_write(" / "); serial_write_dec(pmm_total_page_count()); serial_write("\n");
        } else if (equals(line, "ls")) {
            if (!vfs_list(vfs_root(), print_node, 0)) terminal_writeln("ls: not a directory");
        } else if (kstrlen(line) > 4u && kmemcmp(line, "cat ", 4u) == 0) {
            print_file(line + 4u);
        } else if (equals(line, "clear")) {
            terminal_writeln("\n\n\n\n\n\n\n\n\n\n\n\n");
        } else if (equals(line, "uname")) {
            terminal_writeln("Anix 0.13.0 x86_64");
        } else if (equals(line, "halt")) {
            terminal_writeln("CPU halted.");
            for (;;) __asm__ volatile("cli; hlt");
        } else if (line[0]) {
            serial_write("unknown command: "); serial_write(line); serial_write("\n");
        }
    }
}
