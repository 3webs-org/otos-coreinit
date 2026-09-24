#pragma once

#include <ownership.h>

/* The lock's state, as a fact the analysis carries rather than a comment.
 *
 * A paired token, not a single "held" flag: a lock is always in exactly one
 * of the two states, so acquiring consumes `unheld` and grants `held`, and
 * releasing does the reverse. That is what makes a double-acquire or a
 * release-without-acquire expressible at all -- there is no token to consume
 * -- rather than merely discouraged in prose. `lock_held` is the qualifier
 * the lock-discipline checker reads to know which of the pair means held.
 *
 * mem_mgr.c and cluster_scheduler.c both carry "Caller must hold g_lock"
 * comments over functions that genuinely require it. Those comments are the
 * thing this is meant to replace; see TODO.md for why they cannot be
 * expressed yet (the lock is a file-static, so there is no parameter to
 * name it by). */
tokdef trylock_unheld;
tokdef trylock_held lock_held;

/* The handoff contracts, which the lock checker reads separately from the
 * tokens above. A function that returns holding a lock is not a leak, and a
 * function that requires one on entry is not an unlocked access -- neither
 * is expressible as a token on a parameter, so both are stated here.
 *
 * Spelled out rather than taken from ownership.h because they are not in
 * it: they are the shape LockDisciplineChecker.cpp reads directly, and
 * calgebra's own pthread_cond.c carries its own copy for the same reason. */
#define lock_acquires_for_caller \
    __attribute__((annotate("ntlibc_lock_acquires_for_caller")))
#define lock_requires_held_on_entry(argument) \
    __attribute__((annotate("ntlibc_lock_requires_held_on_entry:" #argument)))

/* Raw atomic test-and-set, TRY-ONLY -- there is deliberately no
 * "acquire and wait" primitive here at all, and no such primitive
 * should be added back. A core that finds a lock contended has better
 * things to do than sit in a loop burning cycles (and, for an RTOS, an
 * unbounded spin is also a priority-inversion hazard: a low-priority
 * holder can stall a high-priority spinner for an unbounded time).
 * Every caller in this codebase is expected to try once and, on
 * failure, do something that actually yields -- hlt and let the next
 * interrupt retry, or job_exec_wait on a proper queue once the caller
 * is job-aware -- never spin here. Compiler intrinsics, not a libc
 * dependency -- safe in this freestanding environment. */

typedef struct {
    volatile int locked;
} trylock_t;

static inline void trylock_init(trylock_t *lock construct(trylock)
                                grant(trylock_unheld))
{
    lock->locked = 0;
}

/* Returns 1 if the lock was free and is now held by the caller, 0 if
 * it was already held by someone else -- never blocks, never loops. */
lock_succeeds_nonzero
static inline int trylock_try_acquire(trylock_t *lock handle(trylock)
                                      consume(trylock_unheld)
                                      grant(trylock_held))
{
    return __sync_lock_test_and_set(&lock->locked, 1) == 0;
}

static inline void trylock_release(trylock_t *lock handle(trylock)
                                   consume(trylock_held)
                                   grant(trylock_unheld))
{
    __sync_lock_release(&lock->locked);
}
