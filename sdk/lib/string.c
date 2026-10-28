#include <anix/libc.h>

size_t anix_strlen(const char *text) {
    size_t length = 0;
    if (text) while (text[length]) ++length;
    return length;
}

void *anix_memcpy(void *destination, const void *source, size_t length) {
    unsigned char *out = destination;
    const unsigned char *in = source;
    for (size_t i = 0; i < length; ++i) out[i] = in[i];
    return destination;
}

int anix_memcmp(const void *left, const void *right, size_t length) {
    const unsigned char *a = left;
    const unsigned char *b = right;
    for (size_t i = 0; i < length; ++i) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

int anix_strcmp(const char *left, const char *right) {
    if (!left || !right) return left == right ? 0 : (left ? 1 : -1);
    while (*left && *left == *right) { ++left; ++right; }
    return (unsigned char)*left == (unsigned char)*right ? 0 :
        ((unsigned char)*left < (unsigned char)*right ? -1 : 1);
}
