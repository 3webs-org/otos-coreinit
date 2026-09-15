#include "proxy.h"
#include "capnp_build.h"
#include "schema.h"
#include "serial.h"

#define MAX_PROXIES 16

typedef struct {
    capability_t remote_target;
    remote_invoke_fn_t remote_invoke;
    /* This proxy's own capability record storage -- CREATE now requires
     * the caller to supply it (see cap.h); a proxy slot is already a
     * stable, long-lived allocation, so embedding it here needs no
     * separate storage source. */
    uint8_t record_storage[CAP_RECORD_SIZE] __attribute__((aligned(CAP_RECORD_ALIGN)));
    int used;
} proxy_data_t;

static proxy_data_t g_proxies[MAX_PROXIES];
static int (*g_local_invoke)(capability_t, uint32_t, const void *, size_t,
                             void *, size_t, size_t *);
static capability_t g_factory;

void proxy_init(int (*local_invoke)(capability_t, uint32_t, const void *,
                                    size_t, void *, size_t, size_t *),
                capability_t factory)
{
    g_local_invoke = local_invoke;
    g_factory = factory;
    for (int i = 0; i < MAX_PROXIES; i++) {
        g_proxies[i].used = 0;
    }
}

static void proxy_handler(void *data, uint32_t method_id, const void *msg,
                          size_t len, void *response, size_t response_cap,
                          size_t *out_response_len)
{
    proxy_data_t *p = (proxy_data_t *)data;
    p->remote_invoke(p->remote_target, method_id, msg, len, response,
                     response_cap, out_response_len);
}

capability_t create_proxy(capability_t remote_target,
                          remote_invoke_fn_t remote_invoke,
                          const object_schema_t *schema)
{
    capability_t none = {0};

    proxy_data_t *slot = NULL;
    for (int i = 0; i < MAX_PROXIES; i++) {
        if (!g_proxies[i].used) {
            slot = &g_proxies[i];
            break;
        }
    }
    if (!slot) {
        kprintf("[coreinit]   proxy: create failed, proxy table full\n");
        return none;
    }
    slot->used = 1;
    slot->remote_target = remote_target;
    slot->remote_invoke = remote_invoke;

    uint64_t words[4] = {(uint64_t)(uintptr_t)proxy_handler,
                         (uint64_t)(uintptr_t)slot, (uint64_t)(uintptr_t)schema,
                         (uint64_t)(uintptr_t)slot->record_storage};
    uint8_t req[64];
    size_t req_len = capnp_build_flat(req, sizeof(req), words, 4);
    uint8_t resp[24];
    size_t resp_len = 0;
    if (!g_local_invoke(g_factory, 0, req, req_len, resp, sizeof(resp),
                        &resp_len) ||
        resp_len < 24) {
        kprintf("[coreinit]   proxy: local create_object failed\n");
        slot->used = 0;
        return none;
    }

    capability_t result;
    result.handle = capnp_read_flat(resp)[0];
    kprintf("[coreinit]   proxy: local handle %u now forwards to remote "
            "handle %u\n",
            (unsigned)result.handle, (unsigned)remote_target.handle);
    return result;
}
