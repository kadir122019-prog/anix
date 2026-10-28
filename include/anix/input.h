#ifndef ANIX_INPUT_H
#define ANIX_INPUT_H
#include <anix/types.h>

enum input_event_type {
    INPUT_EVENT_KEY = 1,
    INPUT_EVENT_POINTER = 2
};

enum input_modifier {
    INPUT_MOD_SHIFT = 1u << 0,
    INPUT_MOD_CAPS_LOCK = 1u << 1
};

struct input_event {
    uint8_t type;
    uint8_t code;
    uint8_t modifiers;
    uint8_t pressed;
    uint8_t buttons;
    uint8_t reserved;
    uint16_t x, y;
    int16_t dx, dy;
};

/* Blocking event interface for user programs and nonblocking kernel polling. */
bool input_event_wait(struct input_event *event);
bool input_event_poll(struct input_event *event);
uint64_t input_event_dropped(void);
void input_event_push_key(uint8_t scan_code, bool pressed, uint8_t modifiers);
void input_event_push_pointer(int16_t dx, int16_t dy, uint16_t x, uint16_t y, uint8_t buttons);

#endif
