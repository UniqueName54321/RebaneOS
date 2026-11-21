#ifndef REBANE_NET_SOCKET_H
#define REBANE_NET_SOCKET_H
#include <stdbool.h>
#include <stdint.h>

/* Cooperative IPv4 sockets. Addresses/ports are raw network-byte-order values.
 * All operations are nonblocking and return negative Linux errno on failure.
 * net_poll must run regularly (including while a guest syscall waits).
 * Handles are kernel transport handles, not Linux guest descriptors. */
bool net_init(void);
bool net_is_ready(void); /* attached driver plus current physical link */
void net_poll(void);
int net_socket(int type, int protocol);
int net_connect(int handle, uint32_t ipv4_network_order, uint16_t port_network_order);
long net_send(int handle, const void *data, unsigned long length);
long net_recv(int handle, void *data, unsigned long length);
long net_peek(int handle, void *data, unsigned long length);
int net_sendto(int handle, const void *data, unsigned long length,
    uint32_t ipv4_network_order, uint16_t port_network_order);
long net_recvfrom(int handle, void *data, unsigned long length,
    uint32_t *ipv4_network_order, uint16_t *port_network_order);
unsigned net_events(int handle); /* POLLIN=1, POLLOUT=4, POLLERR=8, POLLHUP=16 */
int net_error(int handle); /* positive SO_ERROR, clears pending error */
long net_available(int handle); /* pending TCP bytes / first UDP datagram bytes */
int net_sockname(int handle, uint32_t *address, uint16_t *port);
int net_peername(int handle, uint32_t *address, uint16_t *port);
int net_setsockopt(int handle, int level, int option, const void *value, unsigned length);
int net_getsockopt(int handle, int level, int option, void *value, unsigned *length);
int net_shutdown(int handle, int how);
void net_close(int handle);
#endif
