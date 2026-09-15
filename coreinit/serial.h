#pragma once

#include <stdarg.h>

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
