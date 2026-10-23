/* Independently-linked second-stage image. This bootstrap proves a new
 * object can be created in its own Mach-O and invoked through the ONE IPC
 * entry point. Authorization is not claimed here: raw pointer handles and
 * a ring-0 bootstrap image provide no privilege isolation yet. */
#include "../include/securinit_abi.h"
#include "capnp_build.h"
#include "generated_schemas.h"
#include <stdint.h>

static int (*g_invoke)(capability_t, uint32_t, const void *, size_t,
                       void *, size_t, size_t *);
static capability_t g_sink;
static uint8_t g_status_record[CAP_RECORD_SIZE]
    __attribute__((aligned(CAP_RECORD_ALIGN)));

static void status_handler(void *data, uint32_t method_id, const void *msg,
                           size_t len, void *response, size_t response_cap,
                           size_t *out_response_len)
{
    (void)data;
    (void)method_id;
    (void)response;
    (void)response_cap;
    *out_response_len = 0;
    if (g_sink.handle) {
        size_t ignored = 0;
        g_invoke(g_sink, 0, msg, len, NULL, 0, &ignored);
    }
}

uint64_t securinit_main(const struct securinit_boot_info *info)
{
    securinit_schemas_init();
    g_invoke = info->kernel_invoke;
    g_sink = info->status_sink;

    uint64_t create_words[5] = {
        (uint64_t)(uintptr_t)status_handler, 0,
        (uint64_t)(uintptr_t)&g_status_table, 0,
        (uint64_t)(uintptr_t)g_status_record,
    };
    uint8_t create_req[64], create_resp[24];
    size_t create_len = capnp_build_flat(create_req, sizeof(create_req),
                                          create_words, 5);
    size_t response_len = 0;
    if (!g_invoke(info->factory, 0, create_req, create_len, create_resp,
                  sizeof(create_resp), &response_len) || response_len < 24) {
        return 0;
    }
    capability_t status = {capnp_read_flat(create_resp)[0]};
    uint64_t marker = SECURINIT_BOOT_MARKER;
    uint8_t report_req[24];
    size_t report_len = capnp_build_flat(report_req, sizeof(report_req),
                                         &marker, 1);
    if (!g_invoke(status, STATUS_METHOD_REPORT, report_req, report_len,
                  NULL, 0, &response_len)) {
        return 0;
    }
    return status.handle;
}
