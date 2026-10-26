#include "network_rpc.h"
#include "generated_schemas.h"

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
    coreinit_network_rpc_envelope_route_request_t route = {
        .magic = NETWORK_RPC_MAGIC,
        .version_and_type =
            (uint64_t)NETWORK_RPC_VERSION | ((uint64_t)type << 32),
        .request_id = request_id,
        .source_node = source,
        .destination_node = destination,
        .target_handle = target,
        .method_and_payload_length =
            (uint64_t)method | ((uint64_t)payload_len << 32),
        .response_capacity_and_status =
            (uint64_t)response_cap | ((uint64_t)status << 32),
    };
    return coreinit_network_rpc_envelope_route_request_encode(p, cap, &route);
}

static const coreinit_network_rpc_envelope_route_request_t *
read_envelope(const void *packet, size_t packet_len)
{
    if (packet_len < NETWORK_RPC_HEADER_SIZE) return NULL;
    return coreinit_network_rpc_envelope_route_request_read_exact(
        packet, NETWORK_RPC_HEADER_SIZE);
}

int network_rpc_is_response(const void *packet, size_t packet_len)
{
    const coreinit_network_rpc_envelope_route_request_t *h =
        read_envelope(packet, packet_len);
    return h && h->magic == NETWORK_RPC_MAGIC &&
           (uint32_t)h->version_and_type == NETWORK_RPC_VERSION &&
           (uint32_t)(h->version_and_type >> 32) == NETWORK_RPC_RESPONSE;
}

int network_rpc_response_matches(const void *request, size_t request_len,
                                 const void *response, size_t response_len)
{
    const coreinit_network_rpc_envelope_route_request_t *request_envelope =
        read_envelope(request, request_len);
    const coreinit_network_rpc_envelope_route_request_t *response_envelope =
        read_envelope(response, response_len);
    if (!request_envelope || !response_envelope ||
        request_envelope->magic != NETWORK_RPC_MAGIC ||
        response_envelope->magic != NETWORK_RPC_MAGIC ||
        (uint32_t)request_envelope->version_and_type != NETWORK_RPC_VERSION ||
        (uint32_t)response_envelope->version_and_type != NETWORK_RPC_VERSION ||
        (uint32_t)(request_envelope->version_and_type >> 32) !=
            NETWORK_RPC_REQUEST ||
        (uint32_t)(response_envelope->version_and_type >> 32) !=
            NETWORK_RPC_RESPONSE)
        return 0;
    return response_envelope->request_id == request_envelope->request_id &&
           response_envelope->source_node ==
               request_envelope->destination_node &&
           response_envelope->destination_node ==
               request_envelope->source_node;
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
    const coreinit_network_rpc_envelope_route_request_t *rh =
        read_envelope(reply, reply_len);
    if (!rh || rh->magic != NETWORK_RPC_MAGIC ||
        (uint32_t)rh->version_and_type != NETWORK_RPC_VERSION ||
        (uint32_t)(rh->version_and_type >> 32) != NETWORK_RPC_RESPONSE ||
        rh->request_id != id || rh->source_node != s->remote_node ||
        rh->destination_node != s->local_node) return 0;

    uint32_t payload_len = (uint32_t)(rh->method_and_payload_length >> 32);
    if (payload_len > response_cap ||
        reply_len != NETWORK_RPC_HEADER_SIZE + payload_len ||
        (uint32_t)(rh->response_capacity_and_status >> 32) == 0) return 0;
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
    const coreinit_network_rpc_envelope_route_request_t *h =
        read_envelope(request, request_len);
    if (!h) return 0;
    uint32_t payload_len = (uint32_t)(h->method_and_payload_length >> 32);
    uint32_t wanted = (uint32_t)h->response_capacity_and_status;
    if (h->magic != NETWORK_RPC_MAGIC ||
        (uint32_t)h->version_and_type != NETWORK_RPC_VERSION ||
        (uint32_t)(h->version_and_type >> 32) != NETWORK_RPC_REQUEST ||
        h->destination_node != local_node ||
        payload_len > NETWORK_RPC_MAX_PAYLOAD ||
        wanted > NETWORK_RPC_MAX_PAYLOAD ||
        request_len != NETWORK_RPC_HEADER_SIZE + payload_len ||
        response_cap < NETWORK_RPC_HEADER_SIZE + wanted) return 0;

    capability_t target = {h->target_handle};
    size_t result_len = 0;
    int ok = local_invoke(target, (uint32_t)h->method_and_payload_length,
                          request + NETWORK_RPC_HEADER_SIZE, payload_len,
                          response + NETWORK_RPC_HEADER_SIZE, wanted,
                          &result_len);
    if (result_len > wanted) return 0;
    if (envelope(response, response_cap, NETWORK_RPC_RESPONSE, h->request_id,
                 local_node, h->source_node, target.handle,
                 (uint32_t)h->method_and_payload_length,
                 (uint32_t)result_len, 0, ok ? 1u : 0u) !=
        NETWORK_RPC_HEADER_SIZE) return 0;
    if (out_response_len)
        *out_response_len = NETWORK_RPC_HEADER_SIZE + result_len;
    return 1;
}
