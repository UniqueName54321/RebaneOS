#ifndef ROCKOS_NETWORK_H
#define ROCKOS_NETWORK_H

/*
 * Adapter discovery and live Internet transport status. Internet availability
 * does not depend on a mounted RockFS volume; installer disk prerequisites
 * are queried separately through network_has_mount().
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pci.h"

typedef enum {
    /* No supported NIC found on the PCI bus. */
    NETWORK_STATUS_NO_ADAPTER = 0,
    /* Supported NIC found but its driver/transport is not attached yet. */
    NETWORK_STATUS_ADAPTER_FOUND = 1,
    /* Attached driver, configured IPv4 transport, physical link up. */
    NETWORK_STATUS_READY = 2,
    /* Retained installer status value; transport probing never emits this. */
    NETWORK_STATUS_NO_MOUNT = 3
} network_status_t;

typedef struct {
    network_status_t status;
    const char* device_name;   /* human-readable adapter name or NULL */
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t bus;
    uint8_t device;
    uint8_t function;
} network_info_t;


/* Re-enumerate the PCI bus for known adapters. Safe to call repeatedly. */
network_status_t network_probe(void);

/* Grid the current adapter into `info`. Returns the same status code. */
network_status_t network_describe(network_info_t* info);

/* True only with live NETWORK_STATUS_READY; does not imply remote reachability. */
bool network_is_online(void);

/* True when a filesystem is mounted (required even with Skip). */
bool network_has_mount(void);

/* Short, printable adapter description ("" when none). */
const char* network_adapter_text(void);

#endif /* ROCKOS_NETWORK_H */
