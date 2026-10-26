#pragma once

#include <stddef.h>
#include <stdint.h>

/* Minimal raw-frame device seam. The first implementation is polling
 * virtio-net MMIO on aarch64/QEMU; RPC and discovery deliberately sit above
 * this interface and do not know which NIC or interrupt model is underneath. */
int network_device_init(const void *firmware_description, uint64_t node_id);
int network_device_present(void);
const uint8_t *network_device_mac(void);
int network_device_send(const void *frame, size_t len);
int network_device_poll(void *frame, size_t capacity, size_t *out_len);
