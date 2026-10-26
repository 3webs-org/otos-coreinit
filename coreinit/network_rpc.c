#include "network_rpc.h"
#include "capnp_build.h"

static void copy_bytes(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}

void network_rpc_session_init(network_rpc_session_t *s,
                              uint64_t local_node, uint64_t remote_node,
                              network_rpc_exchange_fn exchange, void *ctx)
{
    s->local_node = local_node;
    s->remote_node = remote_node;
    s->next_request_id = 1;
    s->exchange = exchange;
    s->exchange_ctx = ctx;
}

static size_t envelope(uint8_t *p, size_t cap, uint32_t type,
                       uint64_t request_id, uint64_t source,
                       uint64_t destination, uint64_t target, uint32_t method,
                       uint32_t payload_len, uint32_t response_cap,
                       uint32_t status)
{
    uint64_t words[8] = {
        NETWORK_RPC_MAGIC,
        (uint64_t)NETWORK_RPC_VERSION | ((uint64_t)type << 32),
        request_id, source, destination, target,
        (uint64_t)method | ((uint64_t)payload_len << 32),
        (uint64_t)response_cap | ((uint64_t)status << 32),
    };
    return capnp_build_flat(p, cap, words, 8);
}

static const uint64_t *read_envelope(const void *packet, size_t packet_len)
{
    if (packet_len < NETWORK_RPC_HEADER_SIZE) return NULL;
    return capnp_read_flat_exact(packet, NETWORK_RPC_HEADER_SIZE, 8);
}

int network_rpc_is_response(const void *packet, size_t packet_len)
{
    const uint64_t *h = read_envelope(packet, packet_len);
    return h && h[0] == NETWORK_RPC_MAGIC &&
           (uint32_t)h[1] == NETWORK_RPC_VERSION &&
           (uint32_t)(h[1] >> 32) == NETWORK_RPC_RESPONSE;
}

int network_rpc_invoke(void *session_ctx, capability_t target,
                       uint32_t method_id, const void *msg, size_t len,
                       void *response, size_t response_cap,
                       size_t *out_response_len)
{
    network_rpc_session_t *s = (network_rpc_session_t *)session_ctx;
    if (out_response_len) *out_response_len = 0;
    if (!s || !s->exchange || len > NETWORK_RPC_MAX_PAYLOAD ||
        response_cap > NETWORK_RPC_MAX_PAYLOAD || (len && !msg) ||
        (response_cap && !response)) return 0;

    uint8_t request[NETWORK_RPC_HEADER_SIZE + NETWORK_RPC_MAX_PAYLOAD];
    uint8_t reply[NETWORK_RPC_HEADER_SIZE + NETWORK_RPC_MAX_PAYLOAD];
    uint64_t id = __atomic_fetch_add(&s->next_request_id, 1,
                                     __ATOMIC_RELAXED);
    if (envelope(request, sizeof(request), NETWORK_RPC_REQUEST, id,
                 s->local_node, s->remote_node, target.handle, method_id,
                 (uint32_t)len, (uint32_t)response_cap, 0) !=
        NETWORK_RPC_HEADER_SIZE) return 0;
    copy_bytes(request + NETWORK_RPC_HEADER_SIZE, msg, len);

    size_t reply_len = 0;
    if (!s->exchange(s->exchange_ctx, request,
                     NETWORK_RPC_HEADER_SIZE + len, reply, sizeof(reply),
                     &reply_len)) return 0;
    const uint64_t *rh = read_envelope(reply, reply_len);
    if (!rh || rh[0] != NETWORK_RPC_MAGIC ||
        (uint32_t)rh[1] != NETWORK_RPC_VERSION ||
        (uint32_t)(rh[1] >> 32) != NETWORK_RPC_RESPONSE ||
        rh[2] != id || rh[3] != s->remote_node ||
        rh[4] != s->local_node) return 0;

    uint32_t payload_len = (uint32_t)(rh[6] >> 32);
    if (payload_len > response_cap ||
        reply_len != NETWORK_RPC_HEADER_SIZE + payload_len ||
        (uint32_t)(rh[7] >> 32) == 0) return 0;
    copy_bytes(response, reply + NETWORK_RPC_HEADER_SIZE, payload_len);
    if (out_response_len) *out_response_len = payload_len;
    return 1;
}

int network_rpc_serve(
    uint64_t local_node,
    int (*local_invoke)(capability_t, uint32_t, const void *, size_t,
                        void *, size_t, size_t *),
    const void *request_buf, size_t request_len,
    void *response_buf, size_t response_cap, size_t *out_response_len)
{
    if (out_response_len) *out_response_len = 0;
    if (!local_invoke || !request_buf || !response_buf ||
        request_len < NETWORK_RPC_HEADER_SIZE ||
        response_cap < NETWORK_RPC_HEADER_SIZE) return 0;
    const uint8_t *request = (const uint8_t *)request_buf;
    uint8_t *response = (uint8_t *)response_buf;
    const uint64_t *h = read_envelope(request, request_len);
    if (!h) return 0;
    uint32_t payload_len = (uint32_t)(h[6] >> 32);
    uint32_t wanted = (uint32_t)h[7];
    if (h[0] != NETWORK_RPC_MAGIC ||
        (uint32_t)h[1] != NETWORK_RPC_VERSION ||
        (uint32_t)(h[1] >> 32) != NETWORK_RPC_REQUEST ||
        h[4] != local_node ||
        payload_len > NETWORK_RPC_MAX_PAYLOAD ||
        wanted > NETWORK_RPC_MAX_PAYLOAD ||
        request_len != NETWORK_RPC_HEADER_SIZE + payload_len ||
        response_cap < NETWORK_RPC_HEADER_SIZE + wanted) return 0;

    capability_t target = {h[5]};
    size_t result_len = 0;
    int ok = local_invoke(target, (uint32_t)h[6],
                          request + NETWORK_RPC_HEADER_SIZE, payload_len,
                          response + NETWORK_RPC_HEADER_SIZE, wanted,
                          &result_len);
    if (result_len > wanted) return 0;
    if (envelope(response, response_cap, NETWORK_RPC_RESPONSE, h[2],
                 local_node, h[3], target.handle, (uint32_t)h[6],
                 (uint32_t)result_len, 0, ok ? 1u : 0u) !=
        NETWORK_RPC_HEADER_SIZE) return 0;
    if (out_response_len)
        *out_response_len = NETWORK_RPC_HEADER_SIZE + result_len;
    return 1;
}
