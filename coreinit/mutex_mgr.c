#include "mutex_mgr.h"
#include "capnp_build.h"
#include "schema.h"
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

#define METHOD_CREATE 0
#define METHOD_LOCK 1
#define METHOD_UNLOCK 2

static const struct_schema_t g_create_schema = {
    .data_words = 1, .pointer_count = 0, .pointers = NULL};
static const struct_schema_t g_create_result_schema = {
    .data_words = 1, .pointer_count = 0, .pointers = NULL};
static const struct_schema_t g_id_schema = {
    .data_words = 1, .pointer_count = 0, .pointers = NULL};
static const struct_schema_t g_lock_result_schema = {
    .data_words = 1, .pointer_count = 0, .pointers = NULL}; /* {ok: u64} */

static method_schema_t g_methods[3];
object_schema_t g_mutex_manager_schema;

void mutex_manager_build_schema(void)
{
    g_methods[0].method_id = METHOD_CREATE;
    g_methods[0].param_schema = &g_create_schema;
    g_methods[0].result_schema = &g_create_result_schema;

    g_methods[1].method_id = METHOD_LOCK;
    g_methods[1].param_schema = &g_id_schema;
    g_methods[1].result_schema = &g_lock_result_schema; /* try-only --
                                                          * see handler;
                                                          * caller must
                                                          * check ok and
                                                          * retry itself
                                                          * (never spins
                                                          * in here) */

    g_methods[2].method_id = METHOD_UNLOCK;
    g_methods[2].param_schema = &g_id_schema;
    g_methods[2].result_schema = NULL;

    g_mutex_manager_schema.methods = g_methods;
    g_mutex_manager_schema.method_count = 3;
}

void mutex_manager_init_schema(void)
{
    for (int i = 0; i < MAX_MUTEXES; i++) {
        g_mutex_used[i] = 0;
        trylock_init(&g_mutexes[i]);
    }

    mutex_manager_build_schema();
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
