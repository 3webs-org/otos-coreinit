#pragma once

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

static inline void trylock_init(trylock_t *lock)
{
    lock->locked = 0;
}

/* Returns 1 if the lock was free and is now held by the caller, 0 if
 * it was already held by someone else -- never blocks, never loops. */
static inline int trylock_try_acquire(trylock_t *lock)
{
    return __sync_lock_test_and_set(&lock->locked, 1) == 0;
}

static inline void trylock_release(trylock_t *lock)
{
    __sync_lock_release(&lock->locked);
}
