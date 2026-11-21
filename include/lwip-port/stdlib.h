#ifndef REBANE_LWIP_STDLIB_H
#define REBANE_LWIP_STDLIB_H
/* Only netif_find() uses atoi; lwIP's heap never uses the host allocator. */
static inline int atoi(const char *s) {
    int sign = 1, value = 0;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    if (*s == '-' || *s == '+') { if (*s == '-') sign = -1; s++; }
    while (*s >= '0' && *s <= '9') { value = value * 10 + (*s++ - '0'); }
    return value * sign;
}
#endif
