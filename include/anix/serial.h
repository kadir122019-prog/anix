#ifndef ANIX_SERIAL_H
#define ANIX_SERIAL_H
#include <anix/types.h>
bool serial_init(void);
bool serial_available(void);
bool serial_irq_init(void);
bool serial_rx_irq_active(void);
void serial_putc(char c);
void serial_write(const char *s);
void serial_write_n(const char *s, size_t length);
void serial_write_hex(uint64_t value);
void serial_write_dec(uint64_t value);
bool serial_try_read(char *character);
void serial_write_ansi_color(uint8_t code);
void serial_write_ansi_reset(void);
bool serial_color_test(void);
void serial_write_service_ok(const char *name);
void serial_write_service_skip(const char *name);
#endif
