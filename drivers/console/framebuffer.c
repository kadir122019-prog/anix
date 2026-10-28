#include <anix/boot.h>
#include <anix/framebuffer.h>
#include "font8x8_basic.h"

/* Limine's RGB framebuffer memory model is 1. Keep the device state private so
 * callers can draw without depending on the boot protocol's structure layout. */
#define FRAMEBUFFER_RGB_MEMORY_MODEL 1u
#define FRAMEBUFFER_MAX_DIMENSION 16384u
#define FRAMEBUFFER_MAX_BYTES (512u * 1024u * 1024u)
#define CURSOR_WIDTH 8u
#define CURSOR_HEIGHT 12u

struct framebuffer_state {
    volatile uint8_t *address;
    uint64_t width, height, pitch;
    uint16_t bpp;
    uint8_t bytes_per_pixel;
    uint8_t red_size, red_shift;
    uint8_t green_size, green_shift;
    uint8_t blue_size, blue_shift;
    bool ready;
};

static struct framebuffer_state device;
static bool text_console_ready;
static uint64_t cursor_column, cursor_row;
static uint32_t foreground_color = 0xe5e7ebu;
static uint32_t background_color = 0x000000u;
static uint8_t escape_state;
static uint16_t escape_parameters[4];
static uint8_t escape_parameter_count;
static bool cursor_enabled, cursor_drawn;
static uint64_t cursor_x, cursor_y;
static uint32_t cursor_saved[CURSOR_HEIGHT][CURSOR_WIDTH];
static const uint8_t cursor_shape[CURSOR_HEIGHT] = {
    0x80u, 0xc0u, 0xe0u, 0xf0u, 0xf8u, 0xfcu,
    0xfeu, 0xffu, 0xfeu, 0xd8u, 0x8cu, 0x04u,
};
static const uint8_t cursor_inner[CURSOR_HEIGHT] = {
    0x00u, 0x00u, 0x40u, 0x60u, 0x70u, 0x78u,
    0x7cu, 0x78u, 0x70u, 0x30u, 0x08u, 0x00u,
};

static const uint32_t ansi_colors[16] = {
    0x000000u, 0xaa0000u, 0x00aa00u, 0xaaaa00u,
    0x0000aau, 0xaa00aau, 0x00aaaau, 0xaaaaaau,
    0x555555u, 0xff5555u, 0x55ff55u, 0xffff55u,
    0x5555ffu, 0xff55ffu, 0x55ffffu, 0xffffffu,
};

static uint32_t packed_rgb(uint32_t rgb);

static bool valid_channel(uint8_t size, uint8_t shift, uint16_t bpp) {
    return size != 0u && size <= 8u && shift < bpp && (uint16_t)size + shift <= bpp;
}

static uint64_t channel_mask(uint8_t size, uint8_t shift) {
    return ((1ull << size) - 1ull) << shift;
}

bool framebuffer_init(void) {
    struct boot_framebuffer boot;
    if (device.ready || !boot_framebuffer_get(&boot) || !boot.address ||
        boot.memory_model != FRAMEBUFFER_RGB_MEMORY_MODEL ||
        (boot.bpp != 24u && boot.bpp != 32u) ||
        boot.width == 0u || boot.height == 0u ||
        boot.width > FRAMEBUFFER_MAX_DIMENSION || boot.height > FRAMEBUFFER_MAX_DIMENSION ||
        boot.width > UINT64_MAX / (boot.bpp / 8u) ||
        boot.pitch < boot.width * (boot.bpp / 8u) ||
        boot.height > UINT64_MAX / boot.pitch ||
        boot.height * boot.pitch > FRAMEBUFFER_MAX_BYTES ||
        boot.height * boot.pitch > UINT64_MAX - (uint64_t)(uintptr_t)boot.address ||
        !valid_channel(boot.red_mask_size, boot.red_mask_shift, boot.bpp) ||
        !valid_channel(boot.green_mask_size, boot.green_mask_shift, boot.bpp) ||
        !valid_channel(boot.blue_mask_size, boot.blue_mask_shift, boot.bpp) ||
        (channel_mask(boot.red_mask_size, boot.red_mask_shift) & channel_mask(boot.green_mask_size, boot.green_mask_shift)) ||
        (channel_mask(boot.red_mask_size, boot.red_mask_shift) & channel_mask(boot.blue_mask_size, boot.blue_mask_shift)) ||
        (channel_mask(boot.green_mask_size, boot.green_mask_shift) & channel_mask(boot.blue_mask_size, boot.blue_mask_shift))) return false;

    device = (struct framebuffer_state){
        .address = boot.address,
        .width = boot.width,
        .height = boot.height,
        .pitch = boot.pitch,
        .bpp = boot.bpp,
        .bytes_per_pixel = (uint8_t)(boot.bpp / 8u),
        .red_size = boot.red_mask_size,
        .red_shift = boot.red_mask_shift,
        .green_size = boot.green_mask_size,
        .green_shift = boot.green_mask_shift,
        .blue_size = boot.blue_mask_size,
        .blue_shift = boot.blue_mask_shift,
        .ready = true,
    };
    return true;
}

bool framebuffer_ready(void) { return device.ready; }

bool framebuffer_get_info(struct framebuffer_info *out) {
    if (!device.ready || !out) return false;
    *out = (struct framebuffer_info){
        .width = device.width, .height = device.height,
        .pitch = device.pitch, .bpp = device.bpp,
    };
    return true;
}

static void put_pixel_packed(uint64_t x, uint64_t y, uint32_t pixel) {
    volatile uint8_t *destination = device.address + y * device.pitch + x * device.bytes_per_pixel;
    for (uint8_t byte = 0; byte < device.bytes_per_pixel; ++byte)
        destination[byte] = (uint8_t)(pixel >> (byte * 8u));
}

static uint32_t get_pixel_packed(uint64_t x, uint64_t y) {
    const volatile uint8_t *source = device.address + y * device.pitch + x * device.bytes_per_pixel;
    uint32_t pixel = 0;
    for (uint8_t byte = 0; byte < device.bytes_per_pixel; ++byte)
        pixel |= (uint32_t)source[byte] << (byte * 8u);
    return pixel;
}

static void cursor_hide_unlocked(void) {
    if (!cursor_drawn) return;
    for (uint64_t y = 0; y < CURSOR_HEIGHT && cursor_y + y < device.height; ++y)
        for (uint64_t x = 0; x < CURSOR_WIDTH && cursor_x + x < device.width; ++x)
            put_pixel_packed(cursor_x + x, cursor_y + y, cursor_saved[y][x]);
    cursor_drawn = false;
}

static void cursor_draw_unlocked(void) {
    if (!cursor_enabled || cursor_drawn || !device.ready) return;
    const uint32_t outline = packed_rgb(0x000000u);
    const uint32_t fill = packed_rgb(0xffffffu);
    for (uint64_t y = 0; y < CURSOR_HEIGHT && cursor_y + y < device.height; ++y) {
        for (uint64_t x = 0; x < CURSOR_WIDTH && cursor_x + x < device.width; ++x) {
            cursor_saved[y][x] = get_pixel_packed(cursor_x + x, cursor_y + y);
            const uint8_t mask = (uint8_t)(0x80u >> x);
            if (cursor_shape[y] & mask)
                put_pixel_packed(cursor_x + x, cursor_y + y,
                                 (cursor_inner[y] & mask) ? fill : outline);
        }
    }
    cursor_drawn = true;
}

static uint32_t pack_channel(uint8_t value, uint8_t size, uint8_t shift) {
    const uint32_t scaled = size == 8u ? value : (uint32_t)(value >> (8u - size));
    return scaled << shift;
}

static void put_pixel(uint64_t x, uint64_t y, uint32_t rgb) {
    const uint8_t red = (uint8_t)(rgb >> 16);
    const uint8_t green = (uint8_t)(rgb >> 8);
    const uint8_t blue = (uint8_t)rgb;
    const uint32_t pixel = pack_channel(red, device.red_size, device.red_shift) |
                           pack_channel(green, device.green_size, device.green_shift) |
                           pack_channel(blue, device.blue_size, device.blue_shift);
    put_pixel_packed(x, y, pixel);
}

static uint32_t packed_rgb(uint32_t rgb) {
    return pack_channel((uint8_t)(rgb >> 16), device.red_size, device.red_shift) |
           pack_channel((uint8_t)(rgb >> 8), device.green_size, device.green_shift) |
           pack_channel((uint8_t)rgb, device.blue_size, device.blue_shift);
}

__attribute__((target("sse2")))
void framebuffer_fill_rect(uint64_t x, uint64_t y, uint64_t width, uint64_t height, uint32_t rgb) {
    if (!device.ready || x >= device.width || y >= device.height || width == 0u || height == 0u) return;
    if (width > device.width - x) width = device.width - x;
    if (height > device.height - y) height = device.height - y;
    if (device.bytes_per_pixel == 4u) {
        uint8_t saved_fpu_state[512] __attribute__((aligned(16)));
        const uint32_t pixel = pack_channel((uint8_t)(rgb >> 16), device.red_size, device.red_shift) |
                               pack_channel((uint8_t)(rgb >> 8), device.green_size, device.green_shift) |
                               pack_channel((uint8_t)rgb, device.blue_size, device.blue_shift);
        __asm__ volatile("fxsave64 %0" : "=m"(saved_fpu_state) : : "memory");
        for (uint64_t row = y; row < y + height; ++row) {
            volatile uint8_t *destination = device.address + row * device.pitch + x * 4u;
            uint64_t batches = width / 4u;
            uint8_t *batch = (uint8_t *)(uintptr_t)destination;
            __asm__ volatile(
                "movd %k2, %%xmm0\n\t"
                "pshufd $0, %%xmm0, %%xmm0\n\t"
                "1:\n\t"
                "test %0, %0\n\t"
                "jz 2f\n\t"
                "movdqu %%xmm0, (%1)\n\t"
                "add $16, %1\n\t"
                "dec %0\n\t"
                "jmp 1b\n\t"
                "2:"
                : "+r"(batches), "+r"(batch)
                : "r"(pixel)
                : "xmm0", "memory");
            uint64_t column = width - width % 4u;
            for (; column < width; ++column) {
                volatile uint8_t *pixel_destination = destination + column * 4u;
                for (uint8_t byte = 0; byte < 4u; ++byte)
                    pixel_destination[byte] = (uint8_t)(pixel >> (byte * 8u));
            }
        }
        __asm__ volatile("fxrstor64 %0" : : "m"(saved_fpu_state) : "memory");
        return;
    }
    for (uint64_t row = y; row < y + height; ++row)
        for (uint64_t column = x; column < x + width; ++column)
            put_pixel(column, row, rgb);
}

void framebuffer_clear(uint32_t rgb) {
    framebuffer_fill_rect(0, 0, device.width, device.height, rgb);
}

static uint64_t console_columns(void) { return device.width / 8u; }
static uint64_t console_rows(void) { return device.height / 8u; }

static void console_scroll(void) {
    cursor_hide_unlocked();
    const uint64_t bytes_per_row = device.width * device.bytes_per_pixel;
    if (device.height <= 8u) {
        framebuffer_clear(background_color);
        cursor_row = 0;
        return;
    }
    for (uint64_t y = 0; y < device.height - 8u; ++y) {
        volatile uint8_t *destination = device.address + y * device.pitch;
        volatile uint8_t *source = device.address + (y + 8u) * device.pitch;
        for (uint64_t byte = 0; byte < bytes_per_row; ++byte) destination[byte] = source[byte];
    }
    framebuffer_fill_rect(0, device.height - 8u, device.width, 8u, background_color);
    cursor_row = console_rows() - 1u;
}

static void console_advance_line(void) {
    cursor_column = 0;
    if (++cursor_row >= console_rows()) console_scroll();
}

static void draw_character(uint8_t character) {
    if (cursor_column >= console_columns()) console_advance_line();
    if (cursor_row >= console_rows()) console_scroll();
    const uint8_t glyph_index = character < 128u ? character : (uint8_t)'?';
    const uint64_t left = cursor_column * 8u;
    const uint64_t top = cursor_row * 8u;
    if (cursor_drawn && left < cursor_x + CURSOR_WIDTH && cursor_x < left + 8u &&
        top < cursor_y + CURSOR_HEIGHT && cursor_y < top + 8u)
        cursor_hide_unlocked();
    const uint32_t foreground = packed_rgb(foreground_color);
    const uint32_t background = packed_rgb(background_color);
    for (uint64_t y = 0; y < 8u; ++y) {
        const uint8_t bits = font8x8_basic[glyph_index][y];
        if (device.bytes_per_pixel == 4u) {
            volatile uint32_t *row = (volatile uint32_t *)(void *)(device.address + (top + y) * device.pitch + left * 4u);
            for (uint64_t x = 0; x < 8u; ++x) row[x] = (bits & (1u << x)) ? foreground : background;
        } else {
            for (uint64_t x = 0; x < 8u; ++x)
                put_pixel_packed(left + x, top + y, (bits & (1u << x)) ? foreground : background);
        }
    }
    ++cursor_column;
}

static void console_clear(void) {
    cursor_hide_unlocked();
    foreground_color = 0xe5e7ebu;
    background_color = 0x000000u;
    framebuffer_clear(background_color);
    cursor_column = 0;
    cursor_row = 0;
}

static void apply_escape(char final_character) {
    if (final_character == 'm') {
        for (uint8_t index = 0; index < escape_parameter_count; ++index) {
            const uint16_t code = escape_parameters[index];
            if (code == 0u) {
                foreground_color = 0xe5e7ebu;
                background_color = 0x000000u;
            } else if (code >= 30u && code <= 37u) foreground_color = ansi_colors[code - 30u];
            else if (code >= 90u && code <= 97u) foreground_color = ansi_colors[code - 90u + 8u];
            else if (code >= 40u && code <= 47u) background_color = ansi_colors[code - 40u];
            else if (code >= 100u && code <= 107u) background_color = ansi_colors[code - 100u + 8u];
        }
    } else if (final_character == 'J' && escape_parameters[0] == 2u) console_clear();
    else if (final_character == 'H') {
        const uint64_t row = escape_parameter_count > 0u && escape_parameters[0] ? escape_parameters[0] - 1u : 0u;
        const uint64_t column = escape_parameter_count > 1u && escape_parameters[1] ? escape_parameters[1] - 1u : 0u;
        cursor_row = row < console_rows() ? row : console_rows() - 1u;
        cursor_column = column < console_columns() ? column : console_columns() - 1u;
    }
    escape_state = 0;
}

void framebuffer_console_init(void) {
    text_console_ready = device.ready && device.width >= 8u && device.height >= 8u;
    cursor_column = 0;
    cursor_row = console_rows() > 12u ? 10u : 0u;
    foreground_color = 0xe5e7ebu;
    background_color = 0x000000u;
    escape_state = 0;
}

bool framebuffer_console_ready(void) { return text_console_ready; }

static void console_putc_unlocked(char character) {
    if (!text_console_ready) return;
    const uint8_t byte = (uint8_t)character;
    if (escape_state == 1u) {
        escape_state = byte == '[' ? 2u : 0u;
        if (escape_state == 2u) {
            escape_parameter_count = 1u;
            for (uint8_t i = 0; i < 4u; ++i) escape_parameters[i] = 0;
        }
        return;
    }
    if (escape_state == 2u) {
        if (byte >= '0' && byte <= '9') {
            uint16_t *value = &escape_parameters[escape_parameter_count - 1u];
            if (*value < 1000u) *value = (uint16_t)(*value * 10u + (byte - '0'));
        } else if (byte == ';') {
            if (escape_parameter_count < 4u) ++escape_parameter_count;
        } else apply_escape((char)byte);
        return;
    }
    if (byte == 0x1bu) { escape_state = 1u; return; }
    if (byte == '\n') { console_advance_line(); return; }
    if (byte == '\r') { cursor_column = 0; return; }
    if (byte == '\t') {
        do { draw_character(' '); } while (cursor_column % 4u != 0u);
        return;
    }
    if (byte == '\b' || byte == 0x7fu) {
        if (cursor_column == 0u && cursor_row == 0u) return;
        if (cursor_column) --cursor_column;
        else if (cursor_row) { --cursor_row; cursor_column = console_columns() - 1u; }
        draw_character(' ');
        --cursor_column;
        return;
    }
    if (byte >= 0x20u) draw_character(byte);
}

void framebuffer_console_write(const char *text, size_t length) {
    if (!text_console_ready || !text || length == 0u) return;
    uintptr_t interrupt_flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(interrupt_flags) : : "memory", "cc");
    for (size_t i = 0; i < length; ++i) console_putc_unlocked(text[i]);
    cursor_draw_unlocked();
    __asm__ volatile("pushq %0; popfq" : : "r"(interrupt_flags) : "memory", "cc");
}

void framebuffer_console_putc(char character) { framebuffer_console_write(&character, 1u); }

void framebuffer_cursor_enable(bool enabled) {
    if (!device.ready) return;
    uintptr_t interrupt_flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(interrupt_flags) : : "memory", "cc");
    cursor_hide_unlocked();
    cursor_enabled = enabled;
    cursor_draw_unlocked();
    __asm__ volatile("pushq %0; popfq" : : "r"(interrupt_flags) : "memory", "cc");
}

void framebuffer_cursor_move(uint64_t x, uint64_t y) {
    if (!device.ready) return;
    uintptr_t interrupt_flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(interrupt_flags) : : "memory", "cc");
    cursor_hide_unlocked();
    cursor_x = x < device.width ? x : device.width - 1u;
    cursor_y = y < device.height ? y : device.height - 1u;
    cursor_draw_unlocked();
    __asm__ volatile("pushq %0; popfq" : : "r"(interrupt_flags) : "memory", "cc");
}

void framebuffer_boot_test(void) {
    if (!device.ready) return;
    framebuffer_clear(0x000000u);
    const uint32_t colors[] = {
        0x000000u, 0xaa0000u, 0x00aa00u, 0xaaaa00u,
        0x0000aau, 0xaa00aau, 0x00aaaau, 0xaaaaaau,
        0x555555u, 0xff5555u, 0x55ff55u, 0xffff55u,
        0x5555ffu, 0xff55ffu, 0x55ffffu, 0xffffffu,
    };
    const uint64_t margin = device.width < 32u ? 0u : 16u;
    const uint64_t available_width = device.width - margin * 2u;
    const uint64_t columns = 8u;
    const uint64_t gap = available_width >= columns * 4u ? 4u : 0u;
    const uint64_t bar_width = (available_width - gap * (columns - 1u)) / columns;
    const uint64_t bar_height = device.height < 48u ? 1u : 24u;
    const uint64_t top = device.height > bar_height + 32u ? 16u : 0u;
    for (uint64_t index = 0; index < 16u; ++index) {
        const uint64_t column = index % columns;
        const uint64_t row = index / columns;
        framebuffer_fill_rect(margin + column * (bar_width + gap), top + row * (bar_height + 8u),
                              bar_width, bar_height, colors[index]);
    }
}
