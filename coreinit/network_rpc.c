#include "network_rpc.h"

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get64(const uint8_t *p)
{
    return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32);
}

static void put32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (i * 8));
}

static void put64(uint8_t *p, uint64_t v)
{
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

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

static void header(uint8_t *p, uint32_t type, uint64_t request_id,
                   uint64_t source, uint64_t destination,
                   uint64_t target, uint32_t method, uint32_t payload_len,
                   uint32_t response_cap, uint32_t status)
{
    put64(p + 0, NETWORK_RPC_MAGIC);
    put32(p + 8, NETWORK_RPC_VERSION);
    put32(p + 12, type);
    put64(p + 16, request_id);
    put64(p + 24, source);
    put64(p + 32, destination);
    put64(p + 40, target);
    put32(p + 48, method);
    put32(p + 52, payload_len);
    put32(p + 56, response_cap);
    put32(p + 60, status);
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
    header(request, NETWORK_RPC_REQUEST, id, s->local_node, s->remote_node,
           target.handle, method_id, (uint32_t)len, (uint32_t)response_cap, 0);
    copy_bytes(request + NETWORK_RPC_HEADER_SIZE, msg, len);

    size_t reply_len = 0;
    if (!s->exchange(s->exchange_ctx, request,
                     NETWORK_RPC_HEADER_SIZE + len, reply, sizeof(reply),
                     &reply_len) || reply_len < NETWORK_RPC_HEADER_SIZE ||
        get64(reply) != NETWORK_RPC_MAGIC ||
        get32(reply + 8) != NETWORK_RPC_VERSION ||
        get32(reply + 12) != NETWORK_RPC_RESPONSE ||
        get64(reply + 16) != id || get64(reply + 24) != s->remote_node ||
        get64(reply + 32) != s->local_node) return 0;

    uint32_t payload_len = get32(reply + 52);
    if (payload_len > response_cap ||
        reply_len != NETWORK_RPC_HEADER_SIZE + payload_len ||
        get32(reply + 60) == 0) return 0;
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
    uint32_t payload_len = get32(request + 52);
    uint32_t wanted = get32(request + 56);
    if (get64(request) != NETWORK_RPC_MAGIC ||
        get32(request + 8) != NETWORK_RPC_VERSION ||
        get32(request + 12) != NETWORK_RPC_REQUEST ||
        get64(request + 32) != local_node ||
        payload_len > NETWORK_RPC_MAX_PAYLOAD ||
        wanted > NETWORK_RPC_MAX_PAYLOAD ||
        request_len != NETWORK_RPC_HEADER_SIZE + payload_len ||
        response_cap < NETWORK_RPC_HEADER_SIZE + wanted) return 0;

    capability_t target = {get64(request + 40)};
    size_t result_len = 0;
    int ok = local_invoke(target, get32(request + 48),
                          request + NETWORK_RPC_HEADER_SIZE, payload_len,
                          response + NETWORK_RPC_HEADER_SIZE, wanted,
                          &result_len);
    if (result_len > wanted) return 0;
    header(response, NETWORK_RPC_RESPONSE, get64(request + 16), local_node,
           get64(request + 24), target.handle, get32(request + 48),
           (uint32_t)result_len, 0, ok ? 1u : 0u);
    if (out_response_len)
        *out_response_len = NETWORK_RPC_HEADER_SIZE + result_len;
    return 1;
}
