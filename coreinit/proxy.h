#pragma once

#include "cap.h"

typedef int (*remote_invoke_fn_t)(capability_t target, uint32_t method_id,
                                  const void *msg, size_t len, void *response,
                                  size_t response_cap, size_t *out_response_len);

/* The first wrapper built specifically to solve this: a capability
 * handle is only ever meaningful on the node whose kernel_invoke minted
 * it -- node B cannot invoke "handle 7" if handle 7 was minted by node
 * A's own object table. Every remote-object call site in this project
 * so far has worked around that by hand, carrying a (handle,
 * invoke-function) PAIR everywhere instead of an ordinary capability --
 * hw_mutex_lock/unlock's own g_hw_mutex_manager/g_hw_mutex_invoke globals
 * are exactly this, and it doesn't generalize: every new piece of code
 * that wants to reach a remote object has to re-learn which pair to use.
 *
 * create_proxy fixes this the same way auth_A/auth_B fixed
 * authorization earlier: an ordinary LOCAL object, composed from
 * another, whose handler just forwards whatever it receives to the real
 * remote target. Once created, the result is an ordinary capability_t,
 * invoked through this node's own ordinary local kernel_invoke like
 * anything else -- nothing downstream needs to know or care that it's
 * actually remote. This is also the piece resource reassignment will
 * need: a resource's descriptor can be a proxy's local handle rather
 * than a raw remote one, so "who currently serves this resource" can
 * change without every holder of a reference needing to know.
 *
 * schema must exactly match the remote object's own declared schema
 * (method ids, param/result shapes) -- kernel_invoke validates a
 * request against the LOCAL object's schema before ever calling this
 * proxy's handler, so a mismatch would reject legitimate calls. This is
 * safe in practice because every node runs a byte-identical compiled
 * image: referencing the same statically-defined schema struct (e.g.
 * &g_mutex_manager_schema) from any node's own code always means that
 * node's own copy of the identical data.
 *
 * SCOPE: remote_invoke must be a function already reachable from this
 * node -- today that means the BSP's own kernel_invoke (every non-BSP
 * node already has this via bsp_kernel_invoke_addr) for a BSP-hosted
 * object. Reaching an object hosted on a DIFFERENT, non-BSP node isn't
 * supported: this project only has "every node can reach the BSP," not
 * "every node can reach every other node." A peer-to-peer proxy is
 * real, separate future work, not quietly assumed to work here. */
capability_t create_proxy(capability_t remote_target,
                          remote_invoke_fn_t remote_invoke,
                          const object_schema_t *schema);

/* Called once, by each node, before create_proxy is used -- needs this
 * node's own LOCAL kernel_invoke, since a proxy is created the same
 * ordinary way any local object is (an invoke of the factory object's
 * CREATE method) -- see create_proxy's own use of the `factory` handle
 * passed in here. */
void proxy_init(int (*local_invoke)(capability_t target, uint32_t method_id,
                                    const void *msg, size_t len,
                                    void *response, size_t response_cap,
                                    size_t *out_response_len),
                capability_t factory);
