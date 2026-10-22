/* aarch64 backend for ata.h's presence seam. */
#include "ata.h"

/* No ATA on the ARM platforms in scope. */
int ata_present(void)
{
    return 0;
}

/* Unsupported stub. It exists only so the neutral API links on a node
 * with no ATA: callers gate on ata_present() and never reach here,
 * exactly as pagefault_aarch64.c's stubs anticipate for nodes without
 * page-fault mechanics. */
int ata_read_sector(uint32_t lba, void *buf512)
{
    (void)lba;
    (void)buf512;
    return 0;
}
