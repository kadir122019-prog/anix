#include <anix/libc.h>
#include <anix/syscall.h>

static int parse_descriptor(const char *text) {
    if (!text || !text[0]) return -1;
    int value = 0;
    for (unsigned index = 0; text[index]; ++index) {
        if (index == 2u || text[index] < '0' || text[index] > '9') return -1;
        value = value * 10 + text[index] - '0';
    }
    return value;
}

int anix_main(int argc, char **argv) {
    if (!argv) return 80;
    if (argc == 2 && anix_strcmp(argv[1], "stdout") == 0)
        return anix_write(1, "pong", 4u) == 4 ? 0 : 88;
    if (argc == 3 && anix_strcmp(argv[1], "stdin") == 0) {
        const int writer = parse_descriptor(argv[2]);
        char bytes[4];
        if (writer < 3 || anix_close(writer) < 0 || anix_read(0, bytes, sizeof(bytes)) != 4 ||
            anix_memcmp(bytes, "ping", sizeof(bytes)) != 0 || anix_read(0, bytes, sizeof(bytes)) != 0)
            return 89;
        return 0;
    }
    if (argc != 4) return 80;
    const int reader = parse_descriptor(argv[2]);
    const int writer = parse_descriptor(argv[3]);
    if (reader < 3 || writer < 3 || anix_close(writer) < 0) return 81;
    if (anix_strcmp(argv[1], "drop") == 0)
        return anix_close(reader) < 0 ? 83 : 0;
    if (anix_strcmp(argv[1], "stream") == 0) {
        char bytes[4];
        if (anix_read(reader, bytes, sizeof(bytes)) != (long)sizeof(bytes) ||
            anix_memcmp(bytes, "ping", sizeof(bytes)) != 0 ||
            anix_read(reader, bytes, sizeof(bytes)) != 0 || anix_close(reader) < 0) return 82;
        return 0;
    }
    if (anix_strcmp(argv[1], "bulk") == 0) {
        char bytes[256];
        size_t total = 0;
        while (total < 4097u) {
            size_t request = 4097u - total;
            if (request > sizeof(bytes)) request = sizeof(bytes);
            const long count = anix_read(reader, bytes, request);
            if (count <= 0) return 84;
            for (long index = 0; index < count; ++index) {
                const char expected = total + (size_t)index == 4096u ? 'B' : 'A';
                if (bytes[index] != expected) return 85;
            }
            total += (size_t)count;
        }
        if (anix_read(reader, bytes, sizeof(bytes)) != 0 || anix_close(reader) < 0) return 86;
        return 0;
    }
    return 87;
}
