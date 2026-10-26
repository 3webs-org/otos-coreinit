#include "network_ether.h"
#include "network_device.h"
#include "network_rpc.h"
#include "generated_schemas.h"

#define ETH_HEADER 14u
#define ETHERTYPE_HI 0x88u
#define ETHERTYPE_LO 0xb5u
#define ANNOUNCE_MAGIC 0x4f544f5343415031ull /* "OTOSCAP1" */
#define FRAME_CAP 2048u

static void copy_bytes(void *dst, const void *src, size_t len)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < len; i++) d[i] = s[i];
}
static int same_mac(const uint8_t a[6], const uint8_t b[6])
{
    for (unsigned i = 0; i < 6; i++) if (a[i] != b[i]) return 0;
    return 1;
}
static int frame(const uint8_t dst[6], const void *payload, size_t payload_len)
{
    if (payload_len > FRAME_CAP - ETH_HEADER) return 0;
    uint8_t out[FRAME_CAP];
    copy_bytes(out, dst, 6);
    copy_bytes(out + 6, network_device_mac(), 6);
    out[12] = ETHERTYPE_HI;
    out[13] = ETHERTYPE_LO;
    copy_bytes(out + ETH_HEADER, payload, payload_len);
    return network_device_send(out, ETH_HEADER + payload_len);
}
static int poll(uint8_t source[6], void *payload, size_t cap, size_t *out_len)
{
    uint8_t in[FRAME_CAP];
    size_t len = 0;
    if (!network_device_poll(in, sizeof(in), &len) || len < ETH_HEADER ||
        in[12] != ETHERTYPE_HI || in[13] != ETHERTYPE_LO) return 0;
    size_t n = len - ETH_HEADER;
    if (n > cap) return 0;
    copy_bytes(source, in + 6, 6);
    copy_bytes(payload, in + ETH_HEADER, n);
    if (out_len) *out_len = n;
    return 1;
}

int network_ether_announce(uint64_t local_node, uint64_t service,
                           capability_t exported)
{
    uint8_t payload[48];
    uint8_t broadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    coreinit_network_discovery_announce_request_t announce = {
        .magic = ANNOUNCE_MAGIC,
        .node = local_node,
        .service = service,
        .capability = exported.handle,
    };
    size_t len = coreinit_network_discovery_announce_request_encode(
        payload, sizeof(payload), &announce);
    return len && frame(broadcast, payload, len);
}

int network_ether_wait_announcement(uint64_t service, uint64_t *out_node,
                                    capability_t *out_capability,
                                    uint8_t out_mac[6])
{
    uint8_t payload[FRAME_CAP], source[6];
    for (uint32_t spins = 0; spins < 100000000u; spins++) {
        size_t len = 0;
        if (!poll(source, payload, sizeof(payload), &len)) continue;
        const coreinit_network_discovery_announce_request_t *announce =
            coreinit_network_discovery_announce_request_read_exact(payload,
                                                                    len);
        if (!announce || announce->magic != ANNOUNCE_MAGIC ||
            announce->service != service)
            continue;
        *out_node = announce->node;
        out_capability->handle = announce->capability;
        copy_bytes(out_mac, source, 6);
        return 1;
    }
    return 0;
}

int network_ether_exchange(void *ctx, const void *request, size_t request_len,
                           void *response, size_t response_cap,
                           size_t *out_response_len)
{
    network_ether_route_t *route = (network_ether_route_t *)ctx;
    if (!route || !frame(route->peer_mac, request, request_len)) return 0;
    uint8_t source[6];
    for (uint32_t spins = 0; spins < 100000000u; spins++) {
        size_t len = 0;
        if (poll(source, response, response_cap, &len) &&
            same_mac(source, route->peer_mac) &&
            network_rpc_is_response(response, len)) {
            if (out_response_len) *out_response_len = len;
            return 1;
        }
    }
    return 0;
}

int network_ether_serve_once(
    uint64_t local_node,
    int (*local_invoke)(capability_t, uint32_t, const void *, size_t,
                        void *, size_t, size_t *))
{
    uint8_t request[FRAME_CAP], response[FRAME_CAP], source[6];
    size_t request_len = 0;
    if (!poll(source, request, sizeof(request), &request_len)) return 0;
    size_t response_len = 0;
    if (!network_rpc_serve(local_node, local_invoke, request, request_len,
                           response, sizeof(response), &response_len)) return 0;
    return frame(source, response, response_len);
}
