#pragma once

#include <stdint.h>

/* Minimal ATA PIO driver: primary channel (I/O base 0x1F0, control
 * 0x3F6), master drive, LBA28 addressing, polling for completion (no
 * IRQ binding yet -- that's still a listed TODO). This is deliberately
 * the smallest thing that can read a real sector: no DMA, no secondary
 * channel, no slave-drive support, no write path yet. Enough to prove
 * real disk I/O and unblock filesystem bring-up, not a complete driver.
 *
 * Returns 1 on success, 0 on failure (drive not present, error status,
 * or timeout).
 */
int ata_read_sector(uint32_t lba, void *buf512);
