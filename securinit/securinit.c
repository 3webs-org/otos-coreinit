/* Independent second-stage Mach-O. Admin and Gate are ordinary factory
 * objects: only the Admin handle can change the Gate's policy, and Gate
 * forwards to Status after that change. This proves capability-mediated
 * wrapping, NOT a security boundary while all code shares ring 0. */
#include "../include/securinit_abi.h"
#include "arm_wrapper.h"
#include "x86_wrapper.h"
#include "capnp_build.h"
#include "generated_schemas.h"
#include <stdint.h>

static int (*g_invoke)(capability_t, uint32_t, const void *, size_t,
                       void *, size_t, size_t *);
static capability_t g_sink, g_status;
static uint64_t g_allowed, g_denied, g_forwarded;
static uint8_t g_status_record[CAP_RECORD_SIZE]
    __attribute__((aligned(CAP_RECORD_ALIGN)));
static uint8_t g_gate_record[CAP_RECORD_SIZE]
    __attribute__((aligned(CAP_RECORD_ALIGN)));
static uint8_t g_admin_record[CAP_RECORD_SIZE]
    __attribute__((aligned(CAP_RECORD_ALIGN)));

static capability_t create_stage_object(capability_t factory,
                                        object_handler_fn handler,
                                        schema_table_t *table, void *record)
{
    uint64_t words[5] = {(uint64_t)(uintptr_t)handler, 0,
                         (uint64_t)(uintptr_t)table, 0,
                         (uint64_t)(uintptr_t)record};
    uint8_t request[64], response[24];
    size_t request_len = capnp_build_flat(request, sizeof(request), words, 5);
    size_t response_len = 0;
    capability_t result = {0};
    if (g_invoke(factory, 0, request, request_len, response,
                 sizeof(response), &response_len) && response_len >= 24) {
        result.handle = capnp_read_flat(response)[0];
    }
    return result;
}

static void status_handler(void *data, uint32_t method_id, const void *msg,
                           size_t len, void *response, size_t response_cap,
                           size_t *out_response_len)
{
    (void)data;
    (void)method_id;
    (void)response;
    (void)response_cap;
    *out_response_len = 0;
    size_t ignored = 0;
    g_invoke(g_sink, 0, msg, len, NULL, 0, &ignored);
}

static void gate_handler(void *data, uint32_t method_id, const void *msg,
                         size_t len, void *response, size_t response_cap,
                         size_t *out_response_len)
{
    (void)data;
    (void)len;
    if (method_id == GATE_METHOD_GET_STATS) {
        uint64_t stats[2] = {
            __atomic_load_n(&g_denied, __ATOMIC_RELAXED),
            __atomic_load_n(&g_forwarded, __ATOMIC_RELAXED),
        };
        *out_response_len = capnp_build_flat(response, response_cap, stats, 2);
        return;
    }
    uint64_t ok = 0;
    if (__atomic_load_n(&g_allowed, __ATOMIC_ACQUIRE)) {
        uint64_t word = capnp_read_flat(msg)[0];
        uint8_t request[24];
        size_t size = capnp_build_flat(request, sizeof(request), &word, 1);
        size_t ignored = 0;
        ok = g_invoke(g_status, STATUS_METHOD_REPORT, request, size,
                      NULL, 0, &ignored) ? 1 : 0;
        if (ok) {
            __atomic_fetch_add(&g_forwarded, 1, __ATOMIC_RELAXED);
        }
    }
    if (!ok) {
        __atomic_fetch_add(&g_denied, 1, __ATOMIC_RELAXED);
    }
    *out_response_len = capnp_build_flat(response, response_cap, &ok, 1);
}

static void admin_handler(void *data, uint32_t method_id, const void *msg,
                          size_t len, void *response, size_t response_cap,
                          size_t *out_response_len)
{
    (void)data;
    (void)method_id;
    (void)len;
    uint64_t allowed = capnp_read_flat(msg)[0] != 0;
    __atomic_store_n(&g_allowed, allowed, __ATOMIC_RELEASE);
    uint64_t ok = 1;
    *out_response_len = capnp_build_flat(response, response_cap, &ok, 1);
}

uint64_t securinit_main(const struct securinit_boot_info *info)
{
    securinit_schemas_init();
    g_invoke = info->kernel_invoke;
    g_sink = info->status_sink;
    __atomic_store_n(&g_allowed, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_denied, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_forwarded, 0, __ATOMIC_RELAXED);

    g_status = create_stage_object(info->factory, status_handler,
                                    &g_status_table, g_status_record);
    capability_t gate = create_stage_object(info->factory, gate_handler,
                                             &g_gate_table, g_gate_record);
    capability_t admin = create_stage_object(info->factory, admin_handler,
                                              &g_admin_table, g_admin_record);
    if (!g_status.handle || !gate.handle || !admin.handle) {
        return 0;
    }

    uint64_t marker = SECURINIT_BOOT_MARKER;
    uint8_t request[24], response[24];
    size_t request_len = capnp_build_flat(request, sizeof(request), &marker, 1);
    size_t response_len = 0;
    if (!g_invoke(gate, GATE_METHOD_INVOKE, request, request_len,
                  response, sizeof(response), &response_len) ||
        response_len < 24 || capnp_read_flat(response)[0] != 0) {
        return 0; /* A closed Gate must never forward. */
    }

    uint64_t allow = 1;
    request_len = capnp_build_flat(request, sizeof(request), &allow, 1);
    if (!g_invoke(admin, ADMIN_METHOD_SET_ALLOWED, request, request_len,
                  response, sizeof(response), &response_len) ||
        response_len < 24 || capnp_read_flat(response)[0] != 1) {
        return 0;
    }
    request_len = capnp_build_flat(request, sizeof(request), &marker, 1);
    if (!g_invoke(gate, GATE_METHOD_INVOKE, request, request_len,
                  response, sizeof(response), &response_len) ||
        response_len < 24 || capnp_read_flat(response)[0] != 1) {
        return 0;
    }
#if defined(__aarch64__)
    if (info->exception_router.handle) {
        if (!securinit_arm_isolation_probe(info,
                                           (uint64_t)(uintptr_t)g_admin_record,
                                           gate)) {
            return 0;
        }
        uint64_t proof = SECURINIT_EL0_DENIED_MARKER;
        request_len = capnp_build_flat(request, sizeof(request), &proof, 1);
        if (!g_invoke(g_status, STATUS_METHOD_REPORT, request, request_len,
                      NULL, 0, &response_len)) {
            return 0;
        }
    }
#elif defined(__x86_64__)
    if (info->exception_router.handle) {
        if (!securinit_x86_isolation_probe(info,
                                           (uint64_t)(uintptr_t)g_admin_record,
                                           gate)) {
            return 0;
        }
        uint64_t proof = SECURINIT_RING3_DENIED_MARKER;
        request_len = capnp_build_flat(request, sizeof(request), &proof, 1);
        if (!g_invoke(g_status, STATUS_METHOD_REPORT, request, request_len,
                      NULL, 0, &response_len)) {
            return 0;
        }
    }
#endif
    return gate.handle; /* Admin remains private to this stage. */
}
