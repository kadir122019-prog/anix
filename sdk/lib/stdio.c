#include <anix/stdio.h>
#include <anix/syscall.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

struct output_stream {
    int descriptor;
    char buffer[128];
    size_t used;
    long written;
};

static bool flush_output(struct output_stream *stream) {
    size_t offset = 0;
    while (offset < stream->used) {
        long count = anix_write(stream->descriptor, stream->buffer + offset, stream->used - offset);
        if (count <= 0) return false;
        offset += (size_t)count;
        stream->written += count;
    }
    stream->used = 0;
    return true;
}

static bool output_char(struct output_stream *stream, char character) {
    if (stream->used == sizeof(stream->buffer) && !flush_output(stream)) return false;
    stream->buffer[stream->used++] = character;
    return true;
}

static bool output_repeat(struct output_stream *stream, char character, size_t count) {
    while (count--) if (!output_char(stream, character)) return false;
    return true;
}

static bool output_string(struct output_stream *stream, const char *string, size_t width, bool left) {
    size_t length = 0;
    while (string[length]) ++length;
    if (!left && width > length && !output_repeat(stream, ' ', width - length)) return false;
    for (size_t i = 0; i < length; ++i) if (!output_char(stream, string[i])) return false;
    return left && width > length ? output_repeat(stream, ' ', width - length) : true;
}

static bool output_number(struct output_stream *stream, uint64_t value, bool negative,
                          unsigned base, bool uppercase, bool pointer, size_t width,
                          bool zero_pad, bool left) {
    char digits[32];
    const char *alphabet = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    size_t count = 0;
    do {
        digits[count++] = alphabet[value % base];
        value /= base;
    } while (value);
    const size_t prefix = (negative ? 1u : 0u) + (pointer ? 2u : 0u);
    const size_t padding = width > count + prefix ? width - count - prefix : 0;
    if (!left && !zero_pad && !output_repeat(stream, ' ', padding)) return false;
    if (negative && !output_char(stream, '-')) return false;
    if (pointer && (!output_char(stream, '0') || !output_char(stream, 'x'))) return false;
    if (!left && zero_pad && !output_repeat(stream, '0', padding)) return false;
    while (count) if (!output_char(stream, digits[--count])) return false;
    return !left || output_repeat(stream, ' ', padding);
}

static uint64_t unsigned_argument(va_list arguments, unsigned length) {
    if (length == 3u) return (uint64_t)va_arg(arguments, size_t);
    if (length == 2u) return (uint64_t)va_arg(arguments, unsigned long long);
    if (length == 1u) return (uint64_t)va_arg(arguments, unsigned long);
    return (uint64_t)va_arg(arguments, unsigned int);
}

static int64_t signed_argument(va_list arguments, unsigned length) {
    if (length == 3u) return (int64_t)va_arg(arguments, ptrdiff_t);
    if (length == 2u) return (int64_t)va_arg(arguments, long long);
    if (length == 1u) return (int64_t)va_arg(arguments, long);
    return (int64_t)va_arg(arguments, int);
}

static long format_output(int descriptor, const char *format, va_list arguments) {
    if (!format) return -1;
    struct output_stream stream = { .descriptor = descriptor };
    while (*format) {
        if (*format++ != '%') {
            if (!output_char(&stream, format[-1])) return -1;
            continue;
        }
        if (*format == '%') {
            ++format;
            if (!output_char(&stream, '%')) return -1;
            continue;
        }
        bool left = false, zero_pad = false;
        while (*format == '-' || *format == '0') {
            if (*format == '-') left = true;
            if (*format == '0') zero_pad = true;
            ++format;
        }
        size_t width = 0;
        while (*format >= '0' && *format <= '9') {
            if (width < 256u) width = width * 10u + (size_t)(*format - '0');
            if (width > 256u) width = 256u;
            ++format;
        }
        unsigned length = 0;
        if (*format == 'l') {
            length = 1;
            ++format;
            if (*format == 'l') { length = 2; ++format; }
        } else if (*format == 'z') {
            length = 3;
            ++format;
        }
        const char conversion = *format ? *format++ : '\0';
        if (conversion == 's') {
            const char *string = va_arg(arguments, const char *);
            if (!string) string = "(null)";
            if (!output_string(&stream, string, width, left)) return -1;
        } else if (conversion == 'c') {
            const char character = (char)va_arg(arguments, int);
            if (!left && width > 1u && !output_repeat(&stream, ' ', width - 1u)) return -1;
            if (!output_char(&stream, character)) return -1;
            if (left && width > 1u && !output_repeat(&stream, ' ', width - 1u)) return -1;
        } else if (conversion == 'd' || conversion == 'i') {
            const int64_t signed_value = signed_argument(arguments, length);
            const bool negative = signed_value < 0;
            const uint64_t magnitude = negative ? 0u - (uint64_t)signed_value : (uint64_t)signed_value;
            if (!output_number(&stream, magnitude, negative, 10u, false, false, width, zero_pad, left)) return -1;
        } else if (conversion == 'u' || conversion == 'x' || conversion == 'X') {
            const unsigned base = conversion == 'u' ? 10u : 16u;
            if (!output_number(&stream, unsigned_argument(arguments, length), false, base,
                               conversion == 'X', false, width, zero_pad, left)) return -1;
        } else if (conversion == 'p') {
            const uint64_t address = (uint64_t)(uintptr_t)va_arg(arguments, void *);
            if (!output_number(&stream, address, false, 16u, false, true, width, zero_pad, left)) return -1;
        } else if (conversion == '\0') {
            if (!output_char(&stream, '%')) return -1;
            break;
        } else {
            if (!output_char(&stream, '%') || !output_char(&stream, conversion)) return -1;
        }
    }
    if (!flush_output(&stream)) return -1;
    return stream.written;
}

long anix_fprintf(int descriptor, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    long written = format_output(descriptor, format, arguments);
    va_end(arguments);
    return written;
}

long anix_printf(const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    long written = format_output(1, format, arguments);
    va_end(arguments);
    return written;
}

long anix_getline(char *buffer, size_t capacity) {
    if (!buffer || capacity == 0) return -1;
    if (capacity == 1u) { buffer[0] = '\0'; return 0; }
    const long count = anix_read(0, buffer, capacity - 1u);
    if (count < 0) return -1;
    buffer[count] = '\0';
    return count;
}
