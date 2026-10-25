#include <stddef.h>

/* -ffreestanding does not stop clang/gcc from synthesizing calls to these
 * four names for aggregate zero-init, struct copies, and the like -- both
 * compilers document this explicitly: a freestanding environment is
 * expected to provide them itself. Without this file, whether a build
 * links successfully would depend on the optimization level happening to
 * inline every call site instead, which is not a guarantee worth relying
 * on. */

void *memset(void *dst, int value, size_t count)
{
    unsigned char *d = dst;
    for (size_t i = 0; i < count; i++) {
        d[i] = (unsigned char)value;
    }
    return dst;
}

void *memcpy(void *dst, const void *src, size_t count)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    for (size_t i = 0; i < count; i++) {
        d[i] = s[i];
    }
    return dst;
}

void *memmove(void *dst, const void *src, size_t count)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d < s) {
        for (size_t i = 0; i < count; i++) {
            d[i] = s[i];
        }
    } else {
        for (size_t i = count; i > 0; i--) {
            d[i - 1] = s[i - 1];
        }
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t count)
{
    const unsigned char *pa = a;
    const unsigned char *pb = b;
    for (size_t i = 0; i < count; i++) {
        if (pa[i] != pb[i]) {
            return pa[i] - pb[i];
        }
    }
    return 0;
}
