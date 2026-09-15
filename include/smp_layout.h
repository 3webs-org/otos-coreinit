#pragma once

/* Per-core memory region layout: the one hardcoded convention init's own
 * SMP bring-up code (this project's own reference implementation,
 * coreinit/smp.c) and the kernel's kernel_boot_ap must agree on ahead
 * of time, for the same reason init_abi.h's boot handoff shape is
 * hardcoded -- there is no earlier negotiation possible between a
 * freshly-copied kernel instance (which has no multiboot info, no
 * module list, nothing) and whoever set up its region.
 *
 * Each additional core (beyond the BSP, which keeps running wherever
 * GRUB originally placed it) gets one of these regions, at
 * CORE_REGION_BASE + (core_index - 1) * CORE_REGION_SIZE.
 *
 * "Treat each kernel's assigned memory region like MMIO" -- the trailing
 * mailbox area is the ONLY part of a region anything outside that core
 * is meant to touch, and only through explicit volatile-qualified
 * accessors (see coreinit/mmio.h), never as ordinary shared C state. */

#define CORE_REGION_BASE 0x4000000ull  /* 64 MiB */
#define CORE_REGION_SIZE 0x400000ull   /* 4 MiB per core */

#define CORE_KERNEL_OFFSET 0x000000ull    /* fresh kernel image copy */
#define CORE_KERNEL_MAX_SIZE 0x100000ull  /* 1 MiB reserved for it */

#define CORE_INIT_OFFSET 0x100000ull    /* fresh init image copy */
#define CORE_INIT_MAX_SIZE 0x200000ull  /* 2 MiB reserved for it */

#define CORE_STACK_OFFSET 0x300000ull
#define CORE_STACK_SIZE 0x080000ull /* 512 KiB */

/* The trailing coordination buffer -- "MMIO, plus a little extra
 * trailing buffer per core for limited coordination." Nothing about its
 * contents is a kernel concept; see coreinit/scheduler.c and
 * coreinit/cluster_scheduler.h for what actually lives here: a single
 * pending-job mailbox slot the cluster scheduler delivers into and this
 * core's own per-core scheduler drains on its own idle-loop checks.
 *
 * Cross-core job delivery ships a JOB TYPE, not a function pointer --
 * every core runs a byte-identical copy of init, so a small,
 * fixed vocabulary of job types (see scheduler.h's job_type_t) can be
 * dispatched through an identically-populated local table on whichever
 * core receives it, with zero pointer translation and zero risk of a
 * raw address from one core's private, differently-based memory being
 * meaningless on another's. ctx is a plain uint64_t value -- valid
 * cluster-wide only if it's either an ordinary integer the job
 * interprets itself, or a physical address into memory that is
 * genuinely shared (a claimed region from the memory manager, say),
 * never a pointer into a specific core's own relocated data segment. */
#define CORE_MAILBOX_OFFSET 0x380000ull
#define CORE_MAILBOX_SIZE 0x080000ull /* 512 KiB, generously more than needed */
/* Mailbox layout at CORE_MAILBOX_OFFSET, each field a plain uint64_t:
 *   +0x00  pending flag (0 = empty, 1 = a job is waiting)
 *   +0x08  job_type (an index into scheduler.h's job_type_t vocabulary)
 *   +0x10  ctx (see the cluster-wide-validity note above)
 *   +0x18  priority
 */
#define CORE_MAILBOX_PENDING_OFF 0x00ull
#define CORE_MAILBOX_JOB_TYPE_OFF 0x08ull
#define CORE_MAILBOX_CTX_OFF 0x10ull
#define CORE_MAILBOX_PRIORITY_OFF 0x18ull

/* The BSP doesn't live inside the CORE_REGION_BASE scheme at all (it
 * stays wherever GRUB originally placed it) but still needs a mailbox
 * for the same reason every AP does -- so it gets one dedicated,
 * virtual "region base" purely for the mailbox-address arithmetic to
 * work uniformly (mailbox = region_base + CORE_MAILBOX_OFFSET, same
 * formula everywhere). Reference point chosen safely above where the
 * kernel/init modules actually land -- see BSP_INIT_LOAD_BASE's
 * own comment for the full picture; this specific value (18 MiB)
 * clears this project's own Mach-O/GRUB-xnu kernel image (loaded at
 * 16 MiB via -pagezero_size, extending to roughly 16.2 MiB) with room
 * to spare. */
#define BSP_MAILBOX_REGION_BASE (0x1200000ull - CORE_MAILBOX_OFFSET)

/* Discovery record: written by the BSP's own init (this project's own
 * reference implementation's smp.c) into an AP's region before that AP
 * is ever sent SIPI, and read back by that AP's own kernel_boot_ap to
 * populate its own init_boot_info. This is how "objects the BSP
 * created before the trampoline" become findable by "the trampoline's
 * subsequent init instance" without any capability handle ever needing
 * to mean the same thing across two genuinely separate capability
 * tables -- what crosses the boundary here is not
 * a handle by itself, but the handle PLUS the address of the specific
 * kernel_invoke that handle is meaningful to. */
#define CORE_DISCOVERY_OFFSET 0x390000ull
/* Layout at CORE_DISCOVERY_OFFSET, each field a plain uint64_t:
 *   +0x00  bsp_kernel_invoke_addr
 *   +0x08  mutex_manager_handle
 *   +0x10  memory_manager_handle
 *   +0x18  hardware_mutex_id (the one, well-known mutex guarding shared
 *          legacy hardware like the PIC/PIT -- pre-created by the BSP,
 *          not something each core creates its own copy of)
 *   +0x20  lapic_calibration_count (counts-per-10ms, measured ONCE by
 *          the BSP -- see lapic_timer.h for why only the BSP can ever
 *          measure this at all)
 *   +0x30  serial_resource_handle (the one shared "serial" peripheral
 *          resource object, declared once by the BSP -- see
 *          job_kprintf's own note for why this exists)
 */
#define CORE_DISCOVERY_BSP_INVOKE_OFF 0x00ull
#define CORE_DISCOVERY_MUTEX_MGR_OFF 0x08ull
#define CORE_DISCOVERY_MEM_MGR_OFF 0x10ull
#define CORE_DISCOVERY_HW_MUTEX_ID_OFF 0x18ull
#define CORE_DISCOVERY_LAPIC_CALIBRATION_OFF 0x20ull
#define CORE_DISCOVERY_CLUSTER_SCHED_OFF 0x28ull
#define CORE_DISCOVERY_SERIAL_RESOURCE_OFF 0x30ull

#define KERNEL_LOAD_BASE 0x1000000ull /* build.sh's -pagezero_size
                                      * 0x1000000 -- where this kernel's
                                      * own __TEXT actually loads (see
                                      * that flag's own comment for why:
                                      * clear of firmware-owned low
                                      * memory, e.g. where OVMF places
                                      * the ACPI RSDP). Needed here too
                                      * so kernel_boot's own
                                      * boot_info.kernel_load_base is
                                      * correct -- init's own SMP
                                      * bring-up (smp.c) uses it to
                                      * compute the delta when copying
                                      * this running kernel image to an
                                      * AP's own region, so a stale
                                      * value here would silently
                                      * corrupt every address in that
                                      * copy. */

/* Where the BSP's own init is actually loaded. This is no longer
 * something the kernel decides or consults at all -- kernel_boot loads
 * init with target_base=0, meaning "wherever init's own vmaddr already
 * says" (see kernel/macho.h's own note on why), and init's own build
 * declares that vmaddr itself (this project's own reference
 * implementation does so via build_coreinit.sh's own -pagezero_size).
 * This constant now exists purely so that value is documented
 * somewhere and can be cross-checked against init's own memory-manager
 * reservation of the same span (mem_mgr.c, part of init itself, not
 * the kernel -- it needs to know where it itself was loaded to avoid
 * handing that span back out as free memory).
 *
 * The value (24 MiB) and its history: originally 2 MiB, pushed up
 * after a real collision found the hard way with GDB, under the
 * Mach-O/GRUB-xnu boot path specifically. OVMF places the ACPI RSDP at
 * a firmware-chosen low-memory address that isn't fixed (observed at
 * both 0x2469b and 0x20a75b across two otherwise-identical runs of
 * this same VM config), and a 2 MiB init load base sat squarely in
 * that range, silently overwriting it before init's own acpi_discover
 * ever got a chance to find it -- the same class of bug as loading at
 * vmaddr 0 entirely (which collided with multiboot_info and, later,
 * AP_TRAMPOLINE_ADDR, before this project's boot path changed to
 * Mach-O/GRUB-xnu and target_base=0 stopped meaning literal zero), just
 * against firmware-owned memory instead of this project's own. 24 MiB
 * clears both observed RSDP addresses and this project's own kernel
 * image (now loaded at 16 MiB for the same reason, plus its own
 * mailbox just above that -- see BSP_MAILBOX_REGION_BASE), with a full
 * 8 MiB of margin below it. AP_TRAMPOLINE_ADDR itself deliberately
 * stays low (0x8000) -- real-mode code there can only use 16-bit
 * offsets against a zero-based segment, so it can't simply move up
 * here too; moving init's own load address out of its way was the
 * actual fix, same reasoning as before, just repeated once more
 * against a different collision. */
#define BSP_INIT_LOAD_BASE 0x1800000ull /* 24 MiB */

/* Per-core page tables: each AP gets its own independent PML4/PDPT/PD/
 * PD_hi, built fresh by kernel_boot_ap (see kernel.c's
 * build_ap_page_tables) rather than continuing to share the BSP's
 * boot_pml4 the trampoline initially loaded it with just to safely
 * reach long mode. This is a prerequisite for a streamed or COW page
 * to genuinely differ per node -- with every core sharing one page
 * table, marking a page absent/read-only for "this AP" would silently
 * do the same to the BSP and every other AP too, since there would be
 * only one page table for all of them to mark. Carved out of the
 * mailbox region's own generous slack (512 KiB reserved, a few dozen
 * bytes used) rather than growing CORE_REGION_SIZE. Placed well clear
 * of the discovery record fields, which end around offset 0x390030. */
#define CORE_PGTABLE_OFFSET 0x3A0000ull
#define CORE_PGTABLE_PML4_OFF 0x0000ull
#define CORE_PGTABLE_PDPT_OFF 0x1000ull
#define CORE_PGTABLE_PD_OFF 0x2000ull
#define CORE_PGTABLE_PD_HI_OFF 0x3000ull

/* The allocatable window for the memory manager's own alloc/free
 * (mem_mgr.c methods 1/2) and the pool resource the allocator job owns
 * (cluster_scheduler.c) -- defined once, here, so both files agree
 * without either one duplicating the other's constant. Deliberately
 * clear of every fixed-address region this project's other callers
 * still use directly: below it, the BSP's own kernel/mailbox/init
 * (init now based at 24 MiB, see BSP_INIT_LOAD_BASE -- 28 MiB
 * gives it 4 MiB of its own headroom to grow into, same margin as
 * before); at and above CORE_REGION_BASE, each AP's own
 * formula-addressed region. */
#define ALLOC_MIN 0x1C00000ull  /* 28 MiB */
#define ALLOC_MAX CORE_REGION_BASE /* 64 MiB */
