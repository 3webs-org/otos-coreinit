#include "network_device.h"

int network_device_init(const void *firmware_description, uint64_t node_id)
{
    (void)firmware_description;
    (void)node_id;
    return 0; /* virtio-pci transport is the x86 follow-on. */
}
int network_device_present(void) { return 0; }
const uint8_t *network_device_mac(void) { return 0; }
int network_device_send(const void *frame, size_t len)
{
    (void)frame; (void)len; return 0;
}
int network_device_poll(void *frame, size_t capacity, size_t *out_len)
{
    (void)frame; (void)capacity; if (out_len) *out_len = 0; return 0;
}
