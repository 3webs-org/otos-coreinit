/* Wrapper-owned Cap'n Proto transport, not a saski invoke trap. Only the
 * local endpoint granted by securinit resolves to a native capability;
 * descriptor and buffers are range-checked while still untrusted, then
 * copied into EL1/ring-0-only memory before kernel_invoke is called. */
#include "user_ipc.h"
#include "generated_schemas.h"

static int within_user(uint64_t base, uint64_t size,
                       uint64_t addr, uint64_t len)
{
    return addr >= base && len <= size && addr - base <= size - len;
}

int64_t wrapper_dispatch_guest_call(uint64_t user_base, uint64_t user_size,
                                    capability_t granted_gate,
                                    wrapper_invoke_fn_t invoke,
                                    uint64_t descriptor_addr,
                                    uint8_t *request_buffer,
                                    uint8_t *response_buffer)
{
    if (!within_user(user_base, user_size, descriptor_addr,
                     sizeof(wrapper_ipc_call_t))) {
        return WRAPPER_IPC_BAD_DESCRIPTOR;
    }
    wrapper_ipc_call_t call;
    const uint8_t *src = (const uint8_t *)(uintptr_t)descriptor_addr;
    for (size_t i = 0; i < sizeof(call); i++) {
        ((uint8_t *)&call)[i] = src[i];
    }
    if (call.version != WRAPPER_IPC_VERSION) {
        return WRAPPER_IPC_BAD_DESCRIPTOR;
    }
    if (call.local_endpoint != WRAPPER_GATE_ENDPOINT ||
        call.method_id != GATE_METHOD_INVOKE) {
        return WRAPPER_IPC_BAD_ENDPOINT;
    }
    if (call.request_len > WRAPPER_IPC_MAX_BYTES ||
        call.response_capacity > WRAPPER_IPC_MAX_BYTES ||
        !within_user(user_base, user_size, call.request_ptr, call.request_len) ||
        !within_user(user_base, user_size, call.response_ptr,
                     call.response_capacity)) {
        return WRAPPER_IPC_BOUNDS;
    }
    src = (const uint8_t *)(uintptr_t)call.request_ptr;
    for (uint64_t i = 0; i < call.request_len; i++) {
        request_buffer[i] = src[i];
    }
    size_t response_len = 0;
    if (!invoke(granted_gate, call.method_id, request_buffer,
                (size_t)call.request_len, response_buffer,
                (size_t)call.response_capacity, &response_len) ||
        response_len > call.response_capacity) {
        return WRAPPER_IPC_REJECTED;
    }
    uint8_t *dst = (uint8_t *)(uintptr_t)call.response_ptr;
    for (size_t i = 0; i < response_len; i++) {
        dst[i] = response_buffer[i];
    }
    call.response_len = response_len;
    dst = (uint8_t *)(uintptr_t)(descriptor_addr +
                                  offsetof(wrapper_ipc_call_t, response_len));
    src = (const uint8_t *)&call.response_len;
    for (unsigned i = 0; i < 8; i++) {
        dst[i] = src[i];
    }
    return WRAPPER_IPC_OK;
}
