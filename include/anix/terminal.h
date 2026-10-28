#ifndef ANIX_TERMINAL_H
#define ANIX_TERMINAL_H
#include <anix/types.h>

void terminal_write(const char *text);
void terminal_writeln(const char *text);
bool terminal_read_line(char *buffer, size_t capacity);
bool terminal_read_character(char *character);
size_t terminal_read(char *buffer, size_t capacity);
void terminal_input_notify(void);

#endif
