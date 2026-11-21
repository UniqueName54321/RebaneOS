#ifndef REBANE_LWIP_STDIO_H
#define REBANE_LWIP_STDIO_H
#include <stddef.h>
/* lwIP includes this unconditionally; formatted diagnostics are disabled. */
int snprintf(char *, size_t, const char *, ...);
#endif
