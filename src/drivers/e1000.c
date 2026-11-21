/* Intel 82540EM legacy DMA descriptors, polling (no interrupt handler). */
#include "e1000.h"
#include "pci.h"
#include "paging.h"
#include "pmm.h"
#include "io.h"
#include "lwip/pbuf.h"
#include <string.h>

#define RX_COUNT 64u
#define TX_COUNT 32u
#define BUFFER_SIZE 2048u
#define REG_CTRL 0x0000u
#define REG_STATUS 0x0008u
#define REG_ICR 0x00c0u
#define REG_IMC 0x00d8u
#define REG_RCTL 0x0100u
#define REG_TCTL 0x0400u
#define REG_TIPG 0x0410u
#define REG_RDBAL 0x2800u
#define REG_RDBAH 0x2804u
#define REG_RDLEN 0x2808u
#define REG_RDH 0x2810u
#define REG_RDT 0x2818u
#define REG_TDBAL 0x3800u
#define REG_TDBAH 0x3804u
#define REG_TDLEN 0x3808u
#define REG_TDH 0x3810u
#define REG_TDT 0x3818u
#define REG_MTA 0x5200u
#define REG_RAL 0x5400u
#define REG_RAH 0x5404u

struct rx_descriptor {
    uint64_t address;
    uint16_t length, checksum;
    uint8_t status, errors;
    uint16_t special;
} __attribute__((packed));
struct tx_descriptor {
    uint64_t address;
    uint16_t length;
    uint8_t checksum_offset, command, status, checksum_start;
    uint16_t special;
} __attribute__((packed));

static volatile uint8_t *registers;
static volatile struct rx_descriptor *rx_ring;
static volatile struct tx_descriptor *tx_ring;
static uint8_t *rx_buffers, *tx_buffers;
static unsigned rx_next, tx_next;
static bool initialized;

static uint32_t reg_read(unsigned offset) {
    return *(volatile uint32_t *)(registers + offset);
}
static void reg_write(unsigned offset, uint32_t value) {
    *(volatile uint32_t *)(registers + offset) = value;
}
static void dma_barrier(void) {
    /* x86 coherent DMA; prevent compiler reordering around MMIO doorbells. */
    __asm__ volatile ("" ::: "memory");
}

bool e1000_link_up(void) {
    return initialized && (reg_read(REG_STATUS) & 2u) != 0;
}

bool e1000_init(uint8_t mac[6]) {
    if (initialized) {
        uint32_t low = reg_read(REG_RAL), high = reg_read(REG_RAH);
        for (unsigned i = 0; i < 4; i++) mac[i] = (uint8_t)(low >> (i * 8));
        mac[4] = (uint8_t)high;
        mac[5] = (uint8_t)(high >> 8);
        return true;
    }
    const pci_device_t *device = pci_find_by_id(0x8086, 0x100e);
    if (!device || !device->bars[0].present || device->bars[0].is_io)
        return false;
    uintptr_t mapping;
    if (!paging_map_kernel_mmio((uintptr_t)device->bars[0].addr, 0x20000, &mapping))
        return false;
    registers = (volatile uint8_t *)mapping;
    uint16_t command = pci_config_read16(device->bus, device->device,
        device->function, PCI_OFF_COMMAND);
    pci_config_write16(device->bus, device->device, device->function,
        PCI_OFF_COMMAND, (uint16_t)(command | 0x406u)); /* memory + DMA, no INTx */
    reg_write(REG_IMC, 0xffffffffu);
    reg_write(REG_RCTL, 0);
    reg_write(REG_TCTL, 0);
    reg_write(REG_CTRL, reg_read(REG_CTRL) | (1u << 26));
    unsigned spins = 1000000;
    while ((reg_read(REG_CTRL) & (1u << 26)) && --spins)
        __asm__ volatile ("pause");
    if (!spins) return false;
    reg_write(REG_IMC, 0xffffffffu);
    (void)reg_read(REG_ICR);
    reg_write(REG_CTRL, reg_read(REG_CTRL) | (1u << 6)); /* set link up */

    void *rings = pmm_alloc(1);
    rx_buffers = pmm_alloc(RX_COUNT * BUFFER_SIZE / PMM_PAGE_SIZE);
    tx_buffers = pmm_alloc(TX_COUNT * BUFFER_SIZE / PMM_PAGE_SIZE);
    if (!rings || !rx_buffers || !tx_buffers) {
        if (rings) pmm_free(rings, 1);
        if (rx_buffers) pmm_free(rx_buffers, RX_COUNT * BUFFER_SIZE / PMM_PAGE_SIZE);
        if (tx_buffers) pmm_free(tx_buffers, TX_COUNT * BUFFER_SIZE / PMM_PAGE_SIZE);
        return false;
    }
    memset(rings, 0, PMM_PAGE_SIZE);
    rx_ring = rings;
    tx_ring = (volatile struct tx_descriptor *)((uint8_t *)rings + 2048);
    for (unsigned i = 0; i < RX_COUNT; i++)
        rx_ring[i].address = (uintptr_t)(rx_buffers + i * BUFFER_SIZE);
    for (unsigned i = 0; i < TX_COUNT; i++) {
        tx_ring[i].address = (uintptr_t)(tx_buffers + i * BUFFER_SIZE);
        tx_ring[i].status = 1; /* DD: initially owned by software */
    }
    uint32_t low = reg_read(REG_RAL), high = reg_read(REG_RAH);
    for (unsigned i = 0; i < 4; i++) mac[i] = (uint8_t)(low >> (i * 8));
    mac[4] = (uint8_t)high;
    mac[5] = (uint8_t)(high >> 8);
    bool nonzero = false;
    for (unsigned i = 0; i < 6; i++) nonzero |= mac[i] != 0;
    if (!nonzero || (mac[0] & 1)) {
        pmm_free(rings, 1);
        pmm_free(rx_buffers, RX_COUNT * BUFFER_SIZE / PMM_PAGE_SIZE);
        pmm_free(tx_buffers, TX_COUNT * BUFFER_SIZE / PMM_PAGE_SIZE);
        return false;
    }
    reg_write(REG_RAH, high | 0x80000000u); /* receive address valid */
    for (unsigned i = 0; i < 128; i++) reg_write(REG_MTA + i * 4, 0);
    reg_write(REG_RDBAL, (uint32_t)(uintptr_t)rx_ring);
    reg_write(REG_RDBAH, 0); /* PMM DMA frames are below 1 GiB */
    reg_write(REG_RDLEN, RX_COUNT * sizeof(*rx_ring));
    reg_write(REG_RDH, 0);
    reg_write(REG_RDT, RX_COUNT - 1);
    reg_write(REG_TDBAL, (uint32_t)(uintptr_t)tx_ring);
    reg_write(REG_TDBAH, 0);
    reg_write(REG_TDLEN, TX_COUNT * sizeof(*tx_ring));
    reg_write(REG_TDH, 0);
    reg_write(REG_TDT, 0);
    rx_next = tx_next = 0;
    reg_write(REG_TIPG, 10u | (8u << 10) | (6u << 20));
    reg_write(REG_TCTL, (1u << 1) | (1u << 3) | (15u << 4) | (64u << 12));
    /* Enable RX, broadcast acceptance, 2048-byte buffers and CRC stripping. */
    reg_write(REG_RCTL, (1u << 1) | (1u << 15) | (1u << 26));
    initialized = true;
    return true;
}

int e1000_transmit(const struct pbuf *packet) {
    if (!e1000_link_up()) return -100;
    if (!packet || packet->tot_len > 1514 || packet->tot_len < 14) return -90;
    volatile struct tx_descriptor *descriptor = &tx_ring[tx_next];
    if (!(descriptor->status & 1)) return -11;
    dma_barrier();
    if (pbuf_copy_partial(packet, tx_buffers + tx_next * BUFFER_SIZE,
            packet->tot_len, 0) != packet->tot_len)
        return -22;
    descriptor->length = packet->tot_len;
    descriptor->checksum_offset = 0;
    descriptor->checksum_start = 0;
    descriptor->special = 0;
    descriptor->status = 0;
    descriptor->command = 1u | 2u | 8u; /* EOP | insert CRC | report status */
    tx_next = (tx_next + 1) % TX_COUNT;
    dma_barrier();
    reg_write(REG_TDT, tx_next);
    return 0;
}

void e1000_poll(void (*receive)(const void *, size_t)) {
    if (!initialized) return;
    /* Bounded work prevents a busy NIC starving the cooperative desktop. */
    for (unsigned budget = 0; budget < RX_COUNT; budget++) {
        volatile struct rx_descriptor *descriptor = &rx_ring[rx_next];
        uint8_t status = descriptor->status;
        if (!(status & 1)) break;
        dma_barrier();
        size_t length = descriptor->length;
        if ((status & 2) && !descriptor->errors && length >= 14 && length <= 1514)
            receive(rx_buffers + rx_next * BUFFER_SIZE, length);
        descriptor->status = 0;
        dma_barrier();
        reg_write(REG_RDT, rx_next);
        rx_next = (rx_next + 1) % RX_COUNT;
    }
    (void)reg_read(REG_ICR);
}
