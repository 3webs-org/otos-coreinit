#pragma once

#include <stdarg.h>
#include <stdint.h>

void serial_init(void);
void kprintf(const char *fmt, ...);

/* Same formatting as kprintf, taking an already-started va_list --
 * lets a caller (job_kprintf, specifically) wrap the print with its own
 * logic (reserving the serial resource) without duplicating the
 * formatter itself. */
void kvprintf(const char *fmt, va_list ap);

/* Tags every subsequent kprintf line with [core_id] -- see serial.c's
 * own comment for why this exists: multiple cores writing to the same
 * UART concurrently otherwise produce genuinely ambiguous output. */
void serial_set_core_tag(int core_id);

/* Mutual exclusion for the UART, supplied from outside.
 *
 * serial.c cannot reach the serial resource itself -- that is a capability,
 * invoked through the kernel, and this file is the thing capabilities are
 * reported THROUGH. So whoever owns the resource installs two callbacks
 * instead: reserve tries once and returns 1 if it took the port, release
 * gives it back.
 *
 * Try-once by construction, never a spin: reserve is one compare-and-swap
 * inside the resource object, and a caller that loses skips the line rather
 * than waiting for it. Same policy job_kprintf has always used, and the
 * same reason -- see try_lock.h on why nothing here may spin.
 *
 * Until this is installed, printing is unguarded, which is correct for the
 * early boot where there is only one core and no resource yet. */
typedef int (*serial_reserve_fn)(void);
typedef void (*serial_release_fn)(void);
void serial_set_guard(serial_reserve_fn reserve, serial_release_fn release);

/* Which job this core is currently running, for the guard above to reserve
 * the port as. Set by the scheduler around each dispatch, and once at
 * bringup to the job the BSP submitted for this core (see
 * JOB_TYPE_KERNEL_BRINGUP) so that even a core still coming up has an
 * identity. Any stable non-zero value per core is enough for exclusion;
 * using the real job id additionally makes a held reservation attributable
 * to the work that holds it. */
void serial_set_current_job(uint64_t job_id);
uint64_t serial_current_job(void);
