/* Host regression for the common ARM SVC / x86 INT80 copyin/out policy.
 * The mock privileged transport only receives a Gate capability and bytes
 * copied into trusted buffers; malformed guest pointers are never read. */
#include "user_ipc.h"
#include "generated_schemas.h"
#include "capnp_build.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned deliveries;

static int mock_invoke(capability_t target, uint32_t method, const void *msg,
                       size_t len, void *response, size_t capacity,
                       size_t *out_len)
{
    assert(target.handle == 0xabbau && method == GATE_METHOD_INVOKE);
    assert(len == 24 && capnp_read_flat(msg)[0] == 0x5a5aull);
    uint64_t ok = 1;
    *out_len = capnp_build_flat(response, capacity, &ok, 1);
    deliveries++;
    return 1;
}

int main(void)
{
    uint8_t user[8192] = {0};
    uint8_t trusted_request[WRAPPER_IPC_MAX_BYTES];
    uint8_t trusted_response[WRAPPER_IPC_MAX_BYTES];
    uint64_t base = (uint64_t)(uintptr_t)user;
    capability_t gate = {0xabbau};
    uint64_t marker = 0x5a5aull;
    size_t len = capnp_build_flat(user + 0x100, 24, &marker, 1);
    wrapper_ipc_call_t call = {
        .local_endpoint = WRAPPER_GATE_ENDPOINT,
        .version = WRAPPER_IPC_VERSION,
        .method_id = GATE_METHOD_INVOKE,
        .request_ptr = base + 0x100,
        .request_len = len,
        .response_ptr = base + 0x200,
        .response_capacity = 24,
    };
    memcpy(user + 0x300, &call, sizeof(call));
    assert(wrapper_dispatch_guest_call(base, sizeof(user), gate, mock_invoke,
                                       base + 0x300, trusted_request,
                                       trusted_response) == WRAPPER_IPC_OK);
    assert(deliveries == 1);
    memcpy(&call, user + 0x300, sizeof(call));
    assert(call.response_len == 24 && capnp_read_flat(user + 0x200)[0] == 1);

    call.local_endpoint = 2;
    call.request_ptr = (uint64_t)(uintptr_t)&marker;
    memcpy(user + 0x300, &call, sizeof(call));
    assert(wrapper_dispatch_guest_call(base, sizeof(user), gate, mock_invoke,
                                       base + 0x300, trusted_request,
                                       trusted_response) == WRAPPER_IPC_BAD_ENDPOINT);
    assert(deliveries == 1);

    call.local_endpoint = WRAPPER_GATE_ENDPOINT;
    memcpy(user + 0x300, &call, sizeof(call));
    assert(wrapper_dispatch_guest_call(base, sizeof(user), gate, mock_invoke,
                                       base + 0x300, trusted_request,
                                       trusted_response) == WRAPPER_IPC_BOUNDS);
    call.request_ptr = base + 0x100;
    call.response_capacity = WRAPPER_IPC_MAX_BYTES + 1u;
    memcpy(user + 0x300, &call, sizeof(call));
    assert(wrapper_dispatch_guest_call(base, sizeof(user), gate, mock_invoke,
                                       base + 0x300, trusted_request,
                                       trusted_response) == WRAPPER_IPC_BOUNDS);
    call.response_capacity = 24;
    call.version = 9;
    memcpy(user + 0x300, &call, sizeof(call));
    assert(wrapper_dispatch_guest_call(base, sizeof(user), gate, mock_invoke,
                                       base + 0x300, trusted_request,
                                       trusted_response) == WRAPPER_IPC_BAD_DESCRIPTOR);
    assert(wrapper_dispatch_guest_call(base, sizeof(user), gate, mock_invoke,
                                       (uint64_t)(uintptr_t)&call, trusted_request,
                                       trusted_response) == WRAPPER_IPC_BAD_DESCRIPTOR);
    assert(deliveries == 1);
    puts("wrapper-owned Cap'n Proto IPC bounds and copyin/out: PASS");
    return 0;
}
