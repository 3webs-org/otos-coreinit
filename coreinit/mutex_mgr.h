#pragma once

#include "cap.h"

/* A capability object exposing cross-core mutual exclusion. Method 0 =
 * create_mutex (no meaningful request fields -> {mutexId: u64}); method
 * 1 = lock ({mutexId: u64} -> no response, blocks until acquired);
 * method 2 = unlock ({mutexId: u64} -> no response).
 *
 * This exists specifically so multiple cores' coreinit instances can
 * safely coordinate around genuinely shared state -- the legacy
 * PIC/PIT being the concrete, motivating case: without this, every
 * core's irq_core_init() independently reconfigures the same physical
 * hardware with no arbitration at all, and whichever one runs last
 * silently wins. See irq_core_init's own use of the discovered hardware
 * mutex for how this actually gets used, not just published. */
extern schema_table_t g_mutex_manager_table;
#define MUTEX_MGR_SCHEMA_MUTEX_MANAGER 0
void mutex_manager_handler(void *data, uint32_t method_id, const void *msg,
                           size_t len, void *response, size_t response_cap,
                           size_t *out_response_len);

/* Resets the mutex table itself (the object's own state, not its
 * schema -- the schema is const data needing no initialization). */
void mutex_manager_init_schema(void);
