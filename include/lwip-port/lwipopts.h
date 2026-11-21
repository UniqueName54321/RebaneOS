#ifndef REBANE_LWIPOPTS_H
#define REBANE_LWIPOPTS_H

/* Cooperative, polling-only stack: all raw callbacks run in net_poll(). */
#define NO_SYS 1
#define SYS_LIGHTWEIGHT_PROT 0
#define LWIP_SOCKET 0
#define LWIP_NETCONN 0
#define LWIP_TIMERS 1
#define MEM_ALIGNMENT 8
#define MEM_SIZE (2 * 1024 * 1024)
#define MEM_LIBC_MALLOC 0
#define MEMP_MEM_MALLOC 0
#define MEMP_NUM_TCP_PCB 40
#define MEMP_NUM_TCP_PCB_LISTEN 1
#define MEMP_NUM_TCP_SEG 512
#define MEMP_NUM_UDP_PCB 32
#define MEMP_NUM_PBUF 256
#define PBUF_POOL_SIZE 256
#define PBUF_POOL_BUFSIZE 1536
#define LWIP_IPV4 1
#define LWIP_IPV6 0
#define LWIP_ETHERNET 1
#define LWIP_ARP 1
#define ARP_QUEUEING 1
#define MEMP_NUM_ARP_QUEUE 32
#define LWIP_ICMP 1
#define LWIP_RAW 0
#define LWIP_UDP 1
#define LWIP_TCP 1
#define LWIP_DHCP 0
#define LWIP_AUTOIP 0
/* DNS is performed by guest libc over real UDP sockets. */
#define LWIP_DNS 0
#define LWIP_IGMP 0
#define IP_REASSEMBLY 1
#define IP_REASS_MAX_PBUFS 32
#define MEMP_NUM_REASSDATA 8
#define IP_FRAG 1
#define TCP_MSS 1460
#define TCP_WND 32768
#define TCP_SND_BUF 32768
#define TCP_SND_QUEUELEN 128
#define TCP_QUEUE_OOSEQ 1
#define TCP_OOSEQ_MAX_BYTES 32768
#define TCP_OOSEQ_MAX_PBUFS 32
#define LWIP_TCP_KEEPALIVE 1
#define LWIP_NETIF_HOSTNAME 0
#define LWIP_NETIF_STATUS_CALLBACK 0
#define LWIP_NETIF_LINK_CALLBACK 0
#define LWIP_NETIF_LOOPBACK 0
#define LWIP_STATS 0
#define LWIP_NETIF_API 0
#define LWIP_CHECKSUM_ON_COPY 1
#define LWIP_SO_RCVBUF 0
#define LWIP_SINGLE_NETIF 1
#define LWIP_NETIF_TX_SINGLE_PBUF 0
#define LWIP_NOASSERT_ON_ERROR 0
#endif
