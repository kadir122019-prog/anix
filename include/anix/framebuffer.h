#ifndef ANIX_FRAMEBUFFER_H
#define ANIX_FRAMEBUFFER_H
#include <anix/types.h>

struct framebuffer_info {
    uint64_t width, height, pitch;
    uint16_t bpp;
};

bool framebuffer_init(void);
bool framebuffer_ready(void);
bool framebuffer_get_info(struct framebuffer_info *out);
void framebuffer_console_init(void);
bool framebuffer_console_ready(void);
void framebuffer_console_write(const char *text, size_t length);
void framebuffer_console_putc(char character);
void framebuffer_cursor_enable(bool enabled);
void framebuffer_cursor_move(uint64_t x, uint64_t y);
void framebuffer_clear(uint32_t rgb);
void framebuffer_fill_rect(uint64_t x, uint64_t y, uint64_t width, uint64_t height, uint32_t rgb);
void framebuffer_boot_test(void);

#endif
