#pragma once

#include <stddef.h>
#include <stdint.h>
#include "cap.h"

/* kinv -- the invoke library. S3's answer to "how does userspace call an
 * object": a named call site with a null check, nothing more.
 *
 * The kernel's entire syscall surface is kernel_invoke (see cap.h). But
 * kernel_invoke is a FUNCTION POINTER in this system, not a symbol: the
 * BSP runs the kernel's own copy, an AP reaches the BSP's copy through
 * the discovery record, and a proxy forwards through a remote invoke.
 * Every call site therefore had to carry its own (fn, target) pair --
 * g_hw_mutex_invoke/g_hw_mutex_manager being the pattern's first
 * instance -- and every new piece of code re-learned which pair to use.
 *
 * kinv collapses that: the invoke function is an EXPLICIT argument, the
 * target is an ordinary capability_t, and the only behavior the library
 * adds over a bare call is the null-fn guard (a null fn means "no
 * kernel reachable from this context yet" -- returning 0 is the honest
 * answer, not a crash). No backend enum, no syscall, no hidden state:
 * on a target where the kernel is directly linked, kinv(fn, ...) IS a
 * direct call. Everything else -- which fn, which target -- is the
 * caller's business, exactly as it was before, just with one named
 * call site to do it at.
 *
 * Compiled unit, not a header of statics (same reasoning as lib/fdt.c):
 * one definition, one place to read, no per-TU code bloat. */

/* Invoke method_id on target through fn. Returns fn's return value
 * (1 = success, 0 = failure per cap.h's kernel_invoke contract), or 0
 * immediately if fn is NULL. out_response_len may be NULL when the
 * caller does not need the length -- it is forwarded as-is, matching
 * kernel_invoke's own tolerance of a NULL out pointer. */
int kinv(int (*fn)(capability_t target, uint32_t method_id, const void *msg,
                   size_t len, void *response, size_t response_cap,
                   size_t *out_response_len),
         capability_t target, uint32_t method_id, const void *msg, size_t len,
         void *response, size_t response_cap, size_t *out_response_len);
