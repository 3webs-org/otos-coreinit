#include "mutex_mgr.h"
#include "capnp_build.h"
#include "schema.h"
#include "generated_schemas.h"
#include "serial.h"
#include "try_lock.h"

#define MAX_MUTEXES 16

/* Each mutex is just a lock word -- METHOD_LOCK is try-only (see
 * mutex_manager_handler), so there is no wait state to keep here beyond
 * the word itself. */
static trylock_t g_mutexes[MAX_MUTEXES];
static volatile int g_mutex_used[MAX_MUTEXES]; /* CAS-claimed directly,
                                                * no separate table lock
                                                * -- same "claim a free
                                                * slot with a per-slot
                                                * CAS" pattern used
                                                * throughout
                                                * cluster_scheduler.c */



void mutex_manager_init_schema(void)
{
    for (int i = 0; i < MAX_MUTEXES; i++) {
        g_mutex_used[i] = 0;
        trylock_init(&g_mutexes[i]);
    }
}

void mutex_manager_handler(void *data, uint32_t method_id, const void *msg,
                           size_t len, void *response, size_t response_cap,
                           size_t *out_response_len)
{
    (void)data;
    (void)len;
    *out_response_len = 0;

    if (method_id == METHOD_CREATE) {
        int slot = -1;
        for (int i = 0; i < MAX_MUTEXES; i++) {
            if (__sync_bool_compare_and_swap(&g_mutex_used[i], 0, 1)) {
                slot = i;
                break;
            }
        }

        if (slot < 0) {
            kprintf("[coreinit]   mutex_manager: create failed, table full\n");
            return;
        }
        uint64_t id = (uint64_t)slot;
        *out_response_len =
            capnp_build_flat(response, response_cap, &id, 1);
        kprintf("[coreinit]   mutex_manager: created mutex %u\n",
                (unsigned)slot);
        return;
    }

    const uint64_t *words = capnp_read_flat(msg);
    uint64_t id = words[0];
    if (id >= MAX_MUTEXES || !g_mutex_used[id]) {
        kprintf("[coreinit]   mutex_manager: refused, mutex %u invalid\n",
                (unsigned)id);
        return;
    }

    if (method_id == METHOD_LOCK) {
        /* Try-only, always -- see try_lock.h. A caller that needs to
         * wait retries at its own level (hw_mutex_lock in coreinit.c
         * hlt's between attempts); this handler never blocks whoever
         * called kernel_invoke, which would otherwise stall that
         * core's whole dispatch chain, not just one job. */
        uint64_t ok = trylock_try_acquire(&g_mutexes[id]) ? 1u : 0u;
        *out_response_len = capnp_build_flat(response, response_cap, &ok, 1);
    } else if (method_id == METHOD_UNLOCK) {
        trylock_release(&g_mutexes[id]);
    }
}
