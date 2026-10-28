#include <anix/stdio.h>

int anix_main(int argc, char **argv) {
    (void)argv;
    anix_printf("hello from an Anix SDK package (argc=%d)\n", argc);
    return 0;
}
