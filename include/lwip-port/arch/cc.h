#ifndef REBANE_LWIP_CC_H
#define REBANE_LWIP_CC_H
#include <stddef.h>
#include <stdint.h>
#include <limits.h>
typedef ptrdiff_t ssize_t;
#ifndef SSIZE_MAX
#define SSIZE_MAX PTRDIFF_MAX
#endif
#define BYTE_ORDER LITTLE_ENDIAN
#define LWIP_NO_CTYPE_H 1
#define LWIP_NO_UNISTD_H 1
#define LWIP_NO_INTTYPES_H 1
#define X8_F "02x"
#define U16_F "u"
#define S16_F "d"
#define X16_F "x"
#define U32_F "u"
#define S32_F "d"
#define X32_F "x"
#define SZT_F "zu"
#define PACK_STRUCT_STRUCT __attribute__((packed))
#define LWIP_PLATFORM_DIAG(x) do { } while (0)
void rebane_net_assert(const char *message) __attribute__((noreturn));
#define LWIP_PLATFORM_ASSERT(x) rebane_net_assert(x)
uint32_t rebane_net_random(void);
#define LWIP_RAND() rebane_net_random()
#endif
