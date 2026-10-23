#pragma once

#include <stddef.h>
#include <stdint.h>

/* Wrapper-owned EL0 request descriptor. This is NOT a saski syscall ABI:
 * securinit installs its own lower-EL SVC callback, resolves local_endpoint
 * to an EL1-only capability, copies bounded Cap'n Proto bytes, and invokes
 * the ordinary kernel_invoke pointer from trusted ring-0 code. A raw native
 * capability or handler address is never valid as local_endpoint. */
#define WRAPPER_IPC_VERSION 1u
#define WRAPPER_IPC_MAX_BYTES 4096u
#define WRAPPER_GATE_ENDPOINT 1ull

typedef struct wrapper_ipc_call {
    uint64_t local_endpoint;
    uint32_t version;
    uint32_t method_id;
    uint64_t request_ptr;
    uint64_t request_len;
    uint64_t response_ptr;
    uint64_t response_capacity;
    uint64_t response_len;
} wrapper_ipc_call_t;

_Static_assert(sizeof(wrapper_ipc_call_t) == 56, "wrapper invocation descriptor");
_Static_assert(offsetof(wrapper_ipc_call_t, local_endpoint) == 0, "endpoint offset");
_Static_assert(offsetof(wrapper_ipc_call_t, version) == 8, "version offset");
_Static_assert(offsetof(wrapper_ipc_call_t, method_id) == 12, "method offset");
_Static_assert(offsetof(wrapper_ipc_call_t, request_ptr) == 16, "request offset");
_Static_assert(offsetof(wrapper_ipc_call_t, response_len) == 48, "response output offset");

enum wrapper_ipc_status {
    WRAPPER_IPC_OK = 0,
    WRAPPER_IPC_BAD_DESCRIPTOR = -1,
    WRAPPER_IPC_BAD_ENDPOINT = -2,
    WRAPPER_IPC_BOUNDS = -3,
    WRAPPER_IPC_REJECTED = -4,
};
