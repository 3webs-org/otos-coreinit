#pragma once

#include <stddef.h>
#include <stdint.h>
#include "cap.h"

/* Experimental raw-Ethernet carrier for network_rpc. EtherType 0x88b5 is an
 * IEEE local experimental value; no IP stack or host configuration is needed
 * for the two-machine bootstrap proof. */
typedef struct {
    uint8_t peer_mac[6];
} network_ether_route_t;

#define NETWORK_SERVICE_EXECUTOR 1u
#define NETWORK_SERVICE_CLUSTER_SCHEDULER 2u

int network_ether_announce(uint64_t local_node, uint64_t service,
                           capability_t exported);
int network_ether_wait_announcement(uint64_t service, uint64_t *out_node,
                                    capability_t *out_capability,
                                    uint8_t out_mac[6]);
int network_ether_exchange(void *ctx, const void *request, size_t request_len,
                           void *response, size_t response_cap,
                           size_t *out_response_len);
int network_ether_serve_once(
    uint64_t local_node,
    int (*local_invoke)(capability_t, uint32_t, const void *, size_t,
                        void *, size_t, size_t *));
