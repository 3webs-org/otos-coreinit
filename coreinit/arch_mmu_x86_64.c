#include "arch_mmu.h"

/* On x86_64, this is trivially always true, not a placeholder: reaching
 * long mode at all -- which every core running this coreinit image
 * already has, by construction, before coreinit_main ever runs --
 * REQUIRES paging to be enabled (CR0.PG and CR4.PAE both set; see
 * kernel/boot.S's own transition and kernel.c's build_ap_page_tables
 * for the AP path). There is no such thing as an x86_64 core in long
 * mode without a functioning MMU already active under it. A different
 * architecture's implementation of this same header is where a real
 * "no, this core has none" answer could actually occur -- e.g. a
 * simple accelerator or microcontroller core joining the cluster with
 * no paging hardware at all. */
int arch_has_mmu(void)
{
    return 1;
}
