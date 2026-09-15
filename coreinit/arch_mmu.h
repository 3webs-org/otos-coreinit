#pragma once

/* Whether THIS core has a functioning MMU (paging hardware) at all --
 * not every node in a cluster is guaranteed one (a simple accelerator
 * or microcontroller core joining the cluster might have none), and
 * anything that depends on page-fault-driven mechanics -- most
 * directly, RESOURCE_SYNC_STREAM and RESOURCE_SYNC_COW resources (see
 * cluster_scheduler.h), which rely entirely on pagefault.c splitting a
 * region and catching faults locally -- simply isn't available on a
 * node without one. This is the single, portable question cluster
 * join and resource declaration need answered; how it's actually
 * determined is architecture-specific and lives in exactly one .c file
 * implementing this header, same split as cross_core_signal.h. */

/* Returns 1 if this core has a working MMU, 0 if it doesn't. Intended
 * to be read once, at cluster-join time, and handed to JOIN_NODE
 * alongside node_id/arch/region_base/signal_target -- see coreinit.c's
 * own join call and cluster_scheduler.c's handle_join_node, which
 * records it verbatim (as the node's own RESOURCE_KIND_MMU resource,
 * or the absence of one) without needing to know how it was
 * determined. */
int arch_has_mmu(void);
