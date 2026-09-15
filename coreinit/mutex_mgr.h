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
 * core's irq_init() independently reconfigures the same physical
 * hardware with no arbitration at all, and whichever one runs last
 * silently wins. See irq_init's own use of the discovered hardware
 * mutex for how this actually gets used, not just published. */
extern object_schema_t g_mutex_manager_schema;
void mutex_manager_handler(void *data, uint32_t method_id, const void *msg,
                           size_t len, void *response, size_t response_cap,
                           size_t *out_response_len);

/* Called once, by whichever core hosts this object (the BSP, in this
 * design), before its schema is used. */
void mutex_manager_init_schema(void);

/* Populates JUST the schema with no other side effects -- what any node
 * needs before creating a LOCAL PROXY to a remote mutex_manager, since
 * kernel_invoke validates a request against the schema the LOCAL proxy
 * object was created with, not against whatever the real remote object
 * does. mutex_manager_init_schema calls this internally too. */
void mutex_manager_build_schema(void);
