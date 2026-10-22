/* aarch64 backend for pci.h's presence seam. */
#include "pci.h"

/* QEMU virt's DT-described ECAM/PCI bring-up is a future unit; until
 * then this platform honestly reports no PCI. */
int pci_present(void)
{
    return 0;
}

/* Unsupported stub. It exists only so the neutral API links on a node
 * with no PCI: callers gate on pci_present() and never reach here,
 * exactly as pagefault_aarch64.c's stubs anticipate for nodes without
 * page-fault mechanics. */
void pci_scan(pci_found_fn on_found, void *user_data)
{
    (void)on_found;
    (void)user_data;
}
