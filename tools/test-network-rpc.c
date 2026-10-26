#include "network_rpc.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    uint64_t server_node;
    int calls;
} loopback_t;

static int fake_kernel(capability_t target, uint32_t method,
                       const void *msg, size_t len, void *response,
                       size_t response_cap, size_t *out_len)
{
    if (target.handle != 0x123456789abcdef0ull || method != 7 || len != 5 ||
        memcmp(msg, "hello", 5) || response_cap < 5) return 0;
    memcpy(response, "world", 5);
    *out_len = 5;
    return 1;
}

static int loop_exchange(void *ctx, const void *request, size_t request_len,
                         void *response, size_t response_cap, size_t *out_len)
{
    loopback_t *loop = (loopback_t *)ctx;
    loop->calls++;
    return network_rpc_serve(loop->server_node, fake_kernel, request,
                             request_len, response, response_cap, out_len);
}

int main(void)
{
    loopback_t loop = {.server_node = 22};
    network_rpc_session_t session;
    network_rpc_session_init(&session, 11, 22, loop_exchange, &loop);
    capability_t remote = {0x123456789abcdef0ull};
    char response[8] = {0};
    size_t response_len = 0;
    if (!network_rpc_invoke(&session, remote, 7, "hello", 5, response,
                            sizeof(response), &response_len) ||
        response_len != 5 || memcmp(response, "world", 5) || loop.calls != 1) {
        fprintf(stderr, "network RPC loopback failed\n");
        return 1;
    }

    /* Destination identity is part of the routing envelope: a server must
     * not accept a frame addressed to another node. Authentication belongs
     * to the eventual link/session layer and is intentionally not claimed by
     * this codec test. */
    loop.server_node = 23;
    if (network_rpc_invoke(&session, remote, 7, "hello", 5, response,
                           sizeof(response), &response_len)) {
        fprintf(stderr, "wrong-destination frame was accepted\n");
        return 1;
    }
    puts("network RPC loopback and destination rejection: PASS");
    return 0;
}
