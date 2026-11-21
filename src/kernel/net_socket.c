/* lwIP NO_SYS raw sockets for the cooperative Linux application runtime. */
#include "net_socket.h"
#include "pci.h"
#include "pmm.h"
#include "timer.h"
#include "serial.h"
#include "../drivers/e1000.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/timeouts.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "netif/ethernet.h"
#include <string.h>

#define SOCKET_COUNT 32
#define RX_SIZE 65536u
#define UDP_QUEUE 16u
#define RX_PAGES (RX_SIZE / PMM_PAGE_SIZE)
#define STATE_CONNECTING 1
#define STATE_CONNECTED 2
#define STATE_DEAD 3

struct datagram {
    struct pbuf *packet;
    uint32_t address;
    uint16_t port;
};
struct socket {
    bool used, eof, read_shutdown, write_shutdown;
    int type, state, error, failure;
    struct tcp_pcb *tcp;
    struct udp_pcb *udp;
    uint32_t local_address, remote_address;
    uint16_t local_port, remote_port;
    uint8_t *rx;
    unsigned rx_head, rx_length;
    struct datagram datagrams[UDP_QUEUE];
    unsigned udp_head, udp_count;
};
static struct socket sockets[SOCKET_COUNT];
static struct netif interface;
static bool initialized, polling;

/* kernel.c programs the PIT at 100 Hz. Unsigned wrapping is lwIP's timebase. */
u32_t sys_now(void) {
    return (u32_t)(timer_get_ticks() * 10u);
}

void rebane_net_assert(const char *message) {
    serial_write_string("lwIP assertion: ");
    serial_write_string(message);
    serial_write_string("\n");
    for (;;) __asm__ volatile ("cli; hlt");
}

uint32_t rebane_net_random(void) {
    static uint32_t state;
    static int has_rdrand = -1;
    if (has_rdrand < 0) {
        uint32_t a = 1, b, c, d;
        __asm__ volatile ("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
        has_rdrand = (c >> 30) & 1;
    }
    if (has_rdrand) {
        for (unsigned i = 0; i < 10; i++) {
            uint32_t value;
            unsigned char good;
            __asm__ volatile ("rdrand %0; setc %1" : "=r"(value), "=qm"(good));
            if (good) return value;
        }
    }
    /* TCP port/sequence jitter only, never used as a TLS entropy source. */
    uint32_t low, high;
    __asm__ volatile ("rdtsc" : "=a"(low), "=d"(high));
    state ^= low ^ high ^ sys_now() ^ 0x9e3779b9u;
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

static int linux_error(err_t error) {
    switch (error) {
    case ERR_OK: return 0;
    case ERR_MEM: case ERR_BUF: return 11;
    case ERR_TIMEOUT: return 110;
    case ERR_RTE: return 113;
    case ERR_INPROGRESS: return 115;
    case ERR_VAL: case ERR_ARG: return 22;
    case ERR_USE: return 98;
    case ERR_ALREADY: return 114;
    case ERR_ISCONN: return 106;
    case ERR_CONN: return 107;
    case ERR_IF: return 100;
    case ERR_ABRT: return 103;
    case ERR_RST: return 104;
    case ERR_CLSD: return 32;
    default: return 5;
    }
}
static struct socket *lookup(int handle) {
    if (handle < 0 || handle >= SOCKET_COUNT || !sockets[handle].used) return NULL;
    return &sockets[handle];
}
static void save_local(struct socket *socket) {
    if (socket->tcp) {
        socket->local_address = ip4_addr_get_u32(ip_2_ip4(&socket->tcp->local_ip));
        socket->local_port = lwip_htons(socket->tcp->local_port);
    } else if (socket->udp) {
        socket->local_address = ip4_addr_get_u32(ip_2_ip4(&socket->udp->local_ip));
        socket->local_port = lwip_htons(socket->udp->local_port);
    }
}
static err_t output(struct netif *netif, struct pbuf *packet) {
    (void)netif;
    int result = e1000_transmit(packet);
    if (result == -11) return ERR_MEM;
    return result ? ERR_IF : ERR_OK;
}
static err_t interface_init(struct netif *netif) {
    netif->name[0] = 'e';
    netif->name[1] = 'n';
    netif->hwaddr_len = 6;
    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    netif->output = etharp_output;
    netif->linkoutput = output;
    return ERR_OK;
}
static void receive_frame(const void *frame, size_t length) {
    struct pbuf *packet = pbuf_alloc(PBUF_RAW, (u16_t)length, PBUF_POOL);
    if (!packet) return;
    if (pbuf_take(packet, frame, (u16_t)length) != ERR_OK ||
            interface.input(packet, &interface) != ERR_OK)
        pbuf_free(packet);
}

bool net_is_ready(void) {
    return initialized && e1000_link_up();
}
bool net_init(void) {
    if (initialized) return net_is_ready();
    if (!pci_was_scanned()) { pci_init(); pci_scan(); }
    uint8_t mac[6];
    if (!e1000_init(mac)) return false;
    lwip_init();
    ip4_addr_t address, mask, gateway;
    IP4_ADDR(&address, 10, 0, 2, 15);
    IP4_ADDR(&mask, 255, 255, 255, 0);
    IP4_ADDR(&gateway, 10, 0, 2, 2);
    if (!netif_add(&interface, &address, &mask, &gateway, NULL,
            interface_init, ethernet_input)) return false;
    memcpy(interface.hwaddr, mac, 6);
    netif_set_default(&interface);
    netif_set_up(&interface);
    if (e1000_link_up()) netif_set_link_up(&interface);
    initialized = true;
    serial_write_string("Network: e1000 IPv4 10.0.2.15/24 via 10.0.2.2\n");
    return net_is_ready();
}
void net_poll(void) {
    if (!initialized || polling) return;
    polling = true;
    if (e1000_link_up()) {
        if (!netif_is_link_up(&interface)) netif_set_link_up(&interface);
    } else if (netif_is_link_up(&interface)) {
        netif_set_link_down(&interface);
    }
    e1000_poll(receive_frame);
    /* lwIP owns TIME_WAIT PCBs and frees them without an error callback. */
    for (unsigned i = 0; i < SOCKET_COUNT; i++) {
        struct socket *socket = &sockets[i];
        if (socket->used && socket->tcp && socket->tcp->state == TIME_WAIT) {
            tcp_arg(socket->tcp, NULL);
            tcp_err(socket->tcp, NULL);
            socket->tcp = NULL;
            socket->eof = true;
        }
    }
    sys_check_timeouts();
    polling = false;
}

static err_t tcp_received(void *argument, struct tcp_pcb *pcb,
        struct pbuf *packet, err_t error) {
    struct socket *socket = argument;
    if (!packet) { socket->eof = true; return ERR_OK; }
    if (error != ERR_OK) return error;
    if (socket->read_shutdown) {
        tcp_recved(pcb, packet->tot_len);
        pbuf_free(packet);
        return ERR_OK;
    }
    if (packet->tot_len > RX_SIZE - socket->rx_length) return ERR_MEM;
    unsigned tail = (socket->rx_head + socket->rx_length) % RX_SIZE;
    unsigned first = packet->tot_len;
    if (first > RX_SIZE - tail) first = RX_SIZE - tail;
    pbuf_copy_partial(packet, socket->rx + tail, (u16_t)first, 0);
    if (first < packet->tot_len)
        pbuf_copy_partial(packet, socket->rx, (u16_t)(packet->tot_len - first), (u16_t)first);
    socket->rx_length += packet->tot_len;
    pbuf_free(packet);
    /* Advertise space again only when the application actually reads it. */
    return ERR_OK;
}
static void tcp_failed(void *argument, err_t error) {
    struct socket *socket = argument;
    socket->tcp = NULL; /* lwIP already released the PCB before this callback. */
    if (error == ERR_CLSD) {
        socket->eof = true;
        return;
    }
    socket->failure = error == ERR_RST && socket->state == STATE_CONNECTING ?
        111 : linux_error(error);
    socket->state = STATE_DEAD;
    socket->error = socket->failure;
}
static err_t tcp_connected(void *argument, struct tcp_pcb *pcb, err_t error) {
    struct socket *socket = argument;
    if (error != ERR_OK) return error;
    socket->tcp = pcb;
    socket->state = STATE_CONNECTED;
    save_local(socket);
    return ERR_OK;
}
static void udp_received(void *argument, struct udp_pcb *pcb, struct pbuf *packet,
        const ip_addr_t *address, u16_t port) {
    (void)pcb;
    struct socket *socket = argument;
    if (socket->read_shutdown || socket->udp_count == UDP_QUEUE) {
        pbuf_free(packet);
        return;
    }
    unsigned tail = (socket->udp_head + socket->udp_count) % UDP_QUEUE;
    socket->datagrams[tail].packet = packet;
    socket->datagrams[tail].address = ip4_addr_get_u32(ip_2_ip4(address));
    socket->datagrams[tail].port = lwip_htons(port);
    socket->udp_count++;
}

int net_socket(int type, int protocol) {
    if (type != 1 && type != 2) return -94;
    if (protocol && protocol != (type == 1 ? 6 : 17)) return -93;
    if (!net_is_ready()) return -100;
    unsigned index;
    for (index = 0; index < SOCKET_COUNT; index++) if (!sockets[index].used) break;
    if (index == SOCKET_COUNT) return -24;
    struct socket *socket = &sockets[index];
    memset(socket, 0, sizeof(*socket));
    socket->type = type;
    if (type == 1) {
        socket->rx = pmm_alloc(RX_PAGES);
        socket->tcp = tcp_new_ip_type(IPADDR_TYPE_V4);
        if (!socket->rx || !socket->tcp) {
            if (socket->rx) pmm_free(socket->rx, RX_PAGES);
            if (socket->tcp) tcp_abort(socket->tcp);
            memset(socket, 0, sizeof(*socket));
            return -12;
        }
        tcp_arg(socket->tcp, socket);
        tcp_recv(socket->tcp, tcp_received);
        tcp_err(socket->tcp, tcp_failed);
    } else {
        socket->udp = udp_new_ip_type(IPADDR_TYPE_V4);
        if (!socket->udp) return -12;
        udp_recv(socket->udp, udp_received, socket);
    }
    socket->used = true;
    return (int)index;
}
int net_connect(int handle, uint32_t address, uint16_t port) {
    struct socket *socket = lookup(handle);
    if (!socket) return -9;
    if (socket->state == STATE_CONNECTED) return -106;
    if (socket->state == STATE_CONNECTING) return -114;
    if (socket->state == STATE_DEAD) return -(socket->failure ? socket->failure : 107);
    if (!port || !address) return -22;
    ip_addr_t destination;
    ip_addr_set_ip4_u32(&destination, address);
    err_t result;
    if (socket->type == 1) {
        result = tcp_connect(socket->tcp, &destination, lwip_ntohs(port), tcp_connected);
    } else {
        result = udp_connect(socket->udp, &destination, lwip_ntohs(port));
    }
    if (result != ERR_OK) return -linux_error(result);
    socket->remote_address = address;
    socket->remote_port = port;
    save_local(socket);
    socket->state = socket->type == 1 ? STATE_CONNECTING : STATE_CONNECTED;
    return socket->type == 1 ? -115 : 0;
}

long net_send(int handle, const void *data, unsigned long length) {
    struct socket *socket = lookup(handle);
    if (!socket) return -9;
    if (socket->write_shutdown) return -32;
    if (socket->type == 2) {
        if (socket->state != STATE_CONNECTED) return -89;
        return net_sendto(handle, data, length, socket->remote_address, socket->remote_port);
    }
    if (socket->state == STATE_DEAD) return -(socket->failure ? socket->failure : 32);
    if (socket->state != STATE_CONNECTED || !socket->tcp) return -107;
    if (!length) return 0;
    if (!data) return -14;
    unsigned count = tcp_sndbuf(socket->tcp);
    if (length < count) count = (unsigned)length;
    if (!count) return -11;
    err_t result = tcp_write(socket->tcp, data, (u16_t)count, TCP_WRITE_FLAG_COPY);
    if (result != ERR_OK) return -linux_error(result);
    /* Queued bytes remain owned by TCP even when ARP/NIC output is busy. */
    (void)tcp_output(socket->tcp);
    return count;
}
int net_sendto(int handle, const void *data, unsigned long length,
        uint32_t address, uint16_t port) {
    struct socket *socket = lookup(handle);
    if (!socket) return -9;
    if (socket->type != 2) return -95;
    if (socket->write_shutdown) return -32;
    if (length > 65507) return -90;
    if (length && !data) return -14;
    if (!address || !port) return -22;
    struct pbuf *packet = pbuf_alloc(PBUF_TRANSPORT, (u16_t)length, PBUF_RAM);
    if (!packet) return -11;
    if (length) pbuf_take(packet, data, (u16_t)length);
    ip_addr_t destination;
    ip_addr_set_ip4_u32(&destination, address);
    err_t result = udp_sendto(socket->udp, packet, &destination, lwip_ntohs(port));
    pbuf_free(packet);
    save_local(socket);
    return result == ERR_OK ? (int)length : -linux_error(result);
}

static long read_socket(struct socket *socket, void *data, unsigned long length,
        uint32_t *address, uint16_t *port, bool peek) {
    if (socket->read_shutdown) return 0;
    if (!length && socket->type == 1) return 0;
    if (length && !data) return -14;
    if (socket->type == 2) {
        if (!socket->udp_count) return -11;
        struct datagram *datagram = &socket->datagrams[socket->udp_head];
        unsigned count = datagram->packet->tot_len;
        if (length < count) count = (unsigned)length;
        if (count) pbuf_copy_partial(datagram->packet, data, (u16_t)count, 0);
        if (address) *address = datagram->address;
        if (port) *port = datagram->port;
        if (!peek) {
            pbuf_free(datagram->packet);
            datagram->packet = NULL;
            socket->udp_head = (socket->udp_head + 1) % UDP_QUEUE;
            socket->udp_count--;
        }
        return count;
    }
    if (!socket->rx_length) {
        if (socket->failure) return -socket->failure;
        if (socket->eof) return 0;
        return socket->state == STATE_CONNECTED ? -11 : -107;
    }
    unsigned count = socket->rx_length;
    if (length < count) count = (unsigned)length;
    unsigned first = count;
    if (first > RX_SIZE - socket->rx_head) first = RX_SIZE - socket->rx_head;
    memcpy(data, socket->rx + socket->rx_head, first);
    if (first < count) memcpy((uint8_t *)data + first, socket->rx, count - first);
    if (address) *address = socket->remote_address;
    if (port) *port = socket->remote_port;
    if (!peek) {
        socket->rx_head = (socket->rx_head + count) % RX_SIZE;
        socket->rx_length -= count;
        if (socket->tcp) {
            /* TCP_WND is below 65536, so count always fits tcp_recved(). */
            tcp_recved(socket->tcp, (u16_t)count);
        }
    }
    return count;
}
long net_recvfrom(int handle, void *data, unsigned long length,
        uint32_t *address, uint16_t *port) {
    struct socket *socket = lookup(handle);
    if (!socket) return -9;
    return read_socket(socket, data, length, address, port, false);
}
long net_recv(int handle, void *data, unsigned long length) {
    return net_recvfrom(handle, data, length, NULL, NULL);
}
long net_peek(int handle, void *data, unsigned long length) {
    struct socket *socket = lookup(handle);
    if (!socket) return -9;
    return read_socket(socket, data, length, NULL, NULL, true);
}
long net_available(int handle) {
    struct socket *socket = lookup(handle);
    if (!socket) return -9;
    if (socket->type == 1) return socket->rx_length;
    return socket->udp_count ? socket->datagrams[socket->udp_head].packet->tot_len : 0;
}
unsigned net_events(int handle) {
    struct socket *socket = lookup(handle);
    if (!socket) return 0x20; /* POLLNVAL */
    unsigned events = 0;
    if (socket->error) events |= 8;
    if (socket->state == STATE_DEAD) events |= 16 | 1 | 4;
    if (socket->write_shutdown) events |= 4;
    if (socket->read_shutdown && socket->write_shutdown) events |= 16;
    if (socket->read_shutdown || socket->eof || socket->rx_length || socket->udp_count)
        events |= 1;
    if (net_is_ready() && !socket->write_shutdown &&
            (socket->type == 2 || (socket->state == STATE_CONNECTED &&
            socket->tcp && tcp_sndbuf(socket->tcp) &&
            tcp_sndqueuelen(socket->tcp) < TCP_SND_QUEUELEN))) events |= 4;
    return events;
}
int net_error(int handle) {
    struct socket *socket = lookup(handle);
    if (!socket) return 9;
    int error = socket->error;
    socket->error = 0;
    return error;
}
int net_sockname(int handle, uint32_t *address, uint16_t *port) {
    struct socket *socket = lookup(handle);
    if (!socket) return -9;
    save_local(socket);
    if (address) *address = socket->local_address;
    if (port) *port = socket->local_port;
    return 0;
}
int net_peername(int handle, uint32_t *address, uint16_t *port) {
    struct socket *socket = lookup(handle);
    if (!socket) return -9;
    if (socket->state != STATE_CONNECTED) return -107;
    if (address) *address = socket->remote_address;
    if (port) *port = socket->remote_port;
    return 0;
}

int net_setsockopt(int handle, int level, int option, const void *value, unsigned length) {
    struct socket *socket = lookup(handle);
    if (!socket) return -9;
    if (!value || length < sizeof(int)) return -22;
    int setting;
    memcpy(&setting, value, sizeof(setting));
    if (!socket->tcp) return -92;
    if (level == 1 && option == 9) { /* SOL_SOCKET / SO_KEEPALIVE */
        if (setting) ip_set_option(socket->tcp, SOF_KEEPALIVE);
        else ip_reset_option(socket->tcp, SOF_KEEPALIVE);
        return 0;
    }
    if (level != 6) return -92;
    switch (option) {
    case 1: /* TCP_NODELAY */
        if (setting) tcp_nagle_disable(socket->tcp);
        else tcp_nagle_enable(socket->tcp);
        return 0;
    case 4: /* TCP_KEEPIDLE, seconds */
        if (setting < 1 || setting > 32767) return -22;
        socket->tcp->keep_idle = (u32_t)setting * 1000;
        return 0;
    case 5: /* TCP_KEEPINTVL, seconds */
        if (setting < 1 || setting > 32767) return -22;
        socket->tcp->keep_intvl = (u32_t)setting * 1000;
        return 0;
    case 6: /* TCP_KEEPCNT */
        if (setting < 1 || setting > 127) return -22;
        socket->tcp->keep_cnt = (u32_t)setting;
        return 0;
    default: return -92;
    }
}
int net_getsockopt(int handle, int level, int option, void *value, unsigned *length) {
    struct socket *socket = lookup(handle);
    if (!socket) return -9;
    if (!value || !length) return -14;
    int setting;
    if (level == 1) {
        switch (option) {
        case 3: setting = socket->type; break; /* SO_TYPE */
        case 4: setting = net_error(handle); break;
        case 7: setting = socket->type == 1 ? TCP_SND_BUF : 65507; break;
        case 8: setting = socket->type == 1 ? TCP_WND : 65535; break;
        case 9: setting = socket->tcp && ip_get_option(socket->tcp, SOF_KEEPALIVE) ? 1 : 0; break;
        default: return -92;
        }
    } else if (level == 6 && socket->tcp) {
        switch (option) {
        case 1: setting = tcp_nagle_disabled(socket->tcp) ? 1 : 0; break;
        case 2: setting = socket->tcp->mss; break;
        case 4: setting = (int)(socket->tcp->keep_idle / 1000); break;
        case 5: setting = (int)(socket->tcp->keep_intvl / 1000); break;
        case 6: setting = (int)socket->tcp->keep_cnt; break;
        default: return -92;
        }
    } else return -92;
    unsigned count = *length < sizeof(setting) ? *length : sizeof(setting);
    memcpy(value, &setting, count);
    *length = count;
    return 0;
}
int net_shutdown(int handle, int how) {
    struct socket *socket = lookup(handle);
    if (!socket) return -9;
    if (how < 0 || how > 2) return -22;
    if (socket->state != STATE_CONNECTED) return -107;
    /* Keep the PCB's receive side alive to ACK and discard after SHUT_RD.
     * lwIP's raw RX shutdown instead resets on subsequent received data. */
    if (socket->tcp && how != 0 && !socket->write_shutdown) {
        err_t result = tcp_shutdown(socket->tcp, 0, 1);
        if (result != ERR_OK) return -linux_error(result);
    }
    if (how != 1) {
        socket->read_shutdown = true;
        if (socket->tcp && socket->rx_length)
            tcp_recved(socket->tcp, (u16_t)socket->rx_length);
        socket->rx_length = 0;
        while (socket->udp_count) {
            pbuf_free(socket->datagrams[socket->udp_head].packet);
            socket->udp_head = (socket->udp_head + 1) % UDP_QUEUE;
            socket->udp_count--;
        }
    }
    if (how != 0) socket->write_shutdown = true;
    return 0;
}
void net_close(int handle) {
    struct socket *socket = lookup(handle);
    if (!socket) return;
    if (socket->tcp) {
        struct tcp_pcb *pcb = socket->tcp;
        tcp_arg(pcb, NULL);
        tcp_err(pcb, NULL);
        tcp_recv(pcb, NULL);
        tcp_sent(pcb, NULL);
        tcp_poll(pcb, NULL, 0);
        /* lwIP internally retries FIN allocation with TF_CLOSEPEND. */
        if (tcp_close(pcb) != ERR_OK) tcp_abort(pcb);
    }
    if (socket->udp) udp_remove(socket->udp);
    while (socket->udp_count) {
        pbuf_free(socket->datagrams[socket->udp_head].packet);
        socket->udp_head = (socket->udp_head + 1) % UDP_QUEUE;
        socket->udp_count--;
    }
    if (socket->rx) pmm_free(socket->rx, RX_PAGES);
    memset(socket, 0, sizeof(*socket));
}
