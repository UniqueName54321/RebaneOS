#ifndef REBANE_LWIP_STRING_H
#define REBANE_LWIP_STRING_H
#include <stddef.h>
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
void *memset(void *, int, size_t);
size_t strlen(const char *);
static inline int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
    while (n--) { if (*x != *y) return (int)*x - (int)*y; x++; y++; }
    return 0;
}
static inline int strncmp(const char *a, const char *b, size_t n) {
    while (n--) {
        unsigned char x = (unsigned char)*a++, y = (unsigned char)*b++;
        if (x != y) return (int)x - (int)y;
        if (!x) return 0;
    }
    return 0;
}
#endif
