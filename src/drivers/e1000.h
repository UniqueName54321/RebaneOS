#ifndef REBANE_E1000_H
#define REBANE_E1000_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
struct pbuf;
bool e1000_init(uint8_t mac[6]);
bool e1000_link_up(void);
int e1000_transmit(const struct pbuf *packet);
void e1000_poll(void (*receive)(const void *frame, size_t length));
#endif
