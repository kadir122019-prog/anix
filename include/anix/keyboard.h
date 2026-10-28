#ifndef ANIX_KEYBOARD_H
#define ANIX_KEYBOARD_H
#include <anix/types.h>

bool keyboard_init(void);
bool keyboard_available(void);
bool keyboard_try_read(char *character);

#endif
