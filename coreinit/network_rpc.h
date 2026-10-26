#pragma once

#include <stddef.h>
#include <stdint.h>
#include "cap.h"

/* Location-transparent capability invocation over a packet transport.
 * The transport owns link framing (virtio-net, Ethernet, a
 * test loopback), including peer authentication and replay protection; this
 * layer owns the stable cross-machine request format and request correlation.
 * Node ids are routing metadata, not credentials. Do not expose this codec
 * directly to an untrusted network. Retransmitting a state-changing request
 * also requires a bounded reply/deduplication cache keyed by peer+request id;
 * a transport must not blindly retry after an ambiguous send. */
#define NETWORK_RPC_MAGIC 0x4f544f5352504331ull /* "OTOSRPC1" */
#define NETWORK_RPC_VERSION 2u
#define NETWORK_RPC_REQUEST 1u
#define NETWORK_RPC_RESPONSE 2u
#define NETWORK_RPC_HEADER_SIZE 80u /* flat Cap'n Proto root + eight words */
#define NETWORK_RPC_MAX_PAYLOAD 4096u

typedef int (*network_rpc_exchange_fn)(void *ctx,
                                       const void *request, size_t request_len,
                                       void *response, size_t response_cap,
                                       size_t *out_response_len);

typedef struct {
    uint64_t local_node;
    uint64_t remote_node;
    uint64_t next_request_id;
    network_rpc_exchange_fn exchange;
    void *exchange_ctx;
} network_rpc_session_t;

void network_rpc_session_init(network_rpc_session_t *session,
                              uint64_t local_node, uint64_t remote_node,
                              network_rpc_exchange_fn exchange, void *ctx);

/* Signature deliberately matches proxy's context-aware forwarding hook. */
int network_rpc_invoke(void *session_ctx, capability_t target,
                       uint32_t method_id, const void *msg, size_t len,
                       void *response, size_t response_cap,
                       size_t *out_response_len);

/* Strictly validates the Cap'n Proto envelope and identifies a response.
 * Used by packet carriers so they never hand-parse routing fields. */
int network_rpc_is_response(const void *packet, size_t packet_len);

/* Server half. Validates destination, bounds, version, and packet identity
 * before invoking a capability in this machine's local kernel. */
int network_rpc_serve(
    uint64_t local_node,
    int (*local_invoke)(capability_t, uint32_t, const void *, size_t,
                        void *, size_t, size_t *),
    const void *request, size_t request_len,
    void *response, size_t response_cap, size_t *out_response_len);
