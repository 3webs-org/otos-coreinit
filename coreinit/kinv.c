#include "kinv.h"

/* The whole library. See kinv.h for why this is one guard and a call --
 * the null-fn check is the only behavior added over a bare invocation,
 * and it exists so a caller can probe "is a kernel reachable from this
 * context" by passing NULL without a crash: the honest answer to "can
 * I invoke anything" is 0, not a fault. */
int kinv(int (*fn)(capability_t target, uint32_t method_id, const void *msg,
                   size_t len, void *response, size_t response_cap,
                   size_t *out_response_len),
         capability_t target, uint32_t method_id, const void *msg, size_t len,
         void *response, size_t response_cap, size_t *out_response_len)
{
    if (!fn) {
        return 0;
    }
    return fn(target, method_id, msg, len, response, response_cap,
              out_response_len);
}
