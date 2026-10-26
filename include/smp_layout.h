#pragma once

/* The __aarch64__ physical addresses in this file are QEMU virt's 512 MiB
 * reference layout (RAM 0x40000000..0x60000000), not a general ARM ABI.
 * The AP region's internal offsets are architecture-neutral; another ARM
 * board, or a smaller RAM configuration, must place regions from its own
 * discovered memory map before enabling AP boot or the allocation pool.
 * GRUB's XNU entry convention is common across standard platforms; the
 * physical RAM map is not.
 *
 * Per-core memory region layout: the one hardcoded convention init's own
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
 * reserved window (discovery record, per-core page tables) is the ONLY
 * part of a region anything outside that core is meant to touch, and
 * only through explicit volatile-qualified accessors (see
 * coreinit/mmio.h), never as ordinary shared C state. */

/* Both ports use the same offsets within a 4 MiB AP region; only its
 * physical base differs. QEMU virt RAM starts at 0x40000000 on ARM,
 * whereas 0x4000000 is usable low RAM on x86. */
#if defined(__aarch64__)
#define CORE_REGION_BASE 0x42000000ull /* 32 MiB into QEMU virt RAM */
#else
#define CORE_REGION_BASE 0x4000000ull  /* 64 MiB */
#endif
#define CORE_REGION_SIZE 0x400000ull   /* 4 MiB per core */

#define CORE_KERNEL_OFFSET 0x000000ull    /* fresh kernel image copy */
#define CORE_KERNEL_MAX_SIZE 0x100000ull  /* 1 MiB reserved for it */

#define CORE_INIT_OFFSET 0x100000ull    /* fresh init image copy */
#define CORE_INIT_MAX_SIZE 0x200000ull  /* 2 MiB reserved for it */

#define CORE_STACK_OFFSET 0x300000ull
#define CORE_STACK_SIZE 0x080000ull /* 512 KiB */

/* Originally planned as a job "mailbox" -- a per-core slot the cluster
 * scheduler would deliver a pending job into, polled on this core's own
 * idle-loop checks. That delivery mechanism was never actually wired
 * up: real cross-core job delivery goes entirely through the
 * capability/IPC system instead (coreinit/scheduler.c's
 * scheduler_submit_to_node invokes the shared, BSP-hosted cluster
 * scheduler object's SUBMIT_JOB/GET_NEXT_JOB methods, the same
 * mechanism as every other cross-core call in this codebase), so no
 * code anywhere ever reads or writes a mailbox field at this offset.
 * The name and the reserved window survive only because something
 * still needs to be here -- an AP's own discovery record
 * (CORE_DISCOVERY_OFFSET below) and per-core page tables
 * (CORE_PGTABLE_OFFSET) are both carved out of this same generously-
 * sized slack rather than growing CORE_REGION_SIZE -- not because a
 * mailbox still lives at this specific offset. */
#define CORE_MAILBOX_OFFSET 0x380000ull
#define CORE_MAILBOX_SIZE 0x080000ull /* 512 KiB, generously more than needed */

/* The BSP doesn't live inside the CORE_REGION_BASE scheme at all (it
 * stays wherever GRUB originally placed it), but mem_mgr.c still walls
 * off the equivalent of an AP's own reserved scratch window for it, so
 * it gets one dedicated, virtual "region base" purely for that
 * reservation's address arithmetic to work uniformly (same formula,
 * region_base + CORE_MAILBOX_OFFSET, everywhere). Reference point
 * chosen safely above where the kernel/init modules actually land --
 * see BSP_INIT_LOAD_BASE's own comment for the full picture; this
 * specific value (18 MiB) clears this project's own Mach-O/GRUB-xnu
 * kernel image (loaded at 16 MiB via -pagezero_size, extending to
 * roughly 16.2 MiB) with room to spare. */
#if defined(__aarch64__)
#define BSP_MAILBOX_REGION_BASE (0x41800000ull - CORE_MAILBOX_OFFSET)
#else
#define BSP_MAILBOX_REGION_BASE (0x1200000ull - CORE_MAILBOX_OFFSET)
#endif

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
 *   +0x38  early_rsdp_phys (the ACPI RSDP physical address the BSP
 *          discovered; see init_abi.h's own boot_info field of that name)
 *   +0x40  ap_core_state_phys (the AP's preallocated per-core state; legacy
 *          copied-image APs cannot reach InitDescription's allocator)
 *
 * That last field is here for the same reason lapic_calibration_count is:
 * it is a fact the BSP discovered and an AP cannot discover for itself.
 * The BSP finds the RSDP through the EFI configuration table, which only
 * exists in ITS OWN boot_args; an AP arrives with no boot_args at all. On
 * an EFI-only boot there is nothing for an AP to scan either -- boot.S's
 * own comment records that i386-pc/BIOS is out of scope for this kernel.
 * Without this field an AP simply has no RSDP, so its timer never comes
 * up and it cannot participate in scheduling.
 */
#define CORE_DISCOVERY_BSP_INVOKE_OFF 0x00ull
#define CORE_DISCOVERY_MUTEX_MGR_OFF 0x08ull
#define CORE_DISCOVERY_MEM_MGR_OFF 0x10ull
#define CORE_DISCOVERY_HW_MUTEX_ID_OFF 0x18ull
#define CORE_DISCOVERY_LAPIC_CALIBRATION_OFF 0x20ull
#define CORE_DISCOVERY_CLUSTER_SCHED_OFF 0x28ull
#define CORE_DISCOVERY_SERIAL_RESOURCE_OFF 0x30ull
#define CORE_DISCOVERY_RSDP_OFF 0x38ull
#define CORE_DISCOVERY_AP_CORE_STATE_OFF 0x40ull

/* The ARM kernel is linked at RAM + 16 MiB for the -kernel, EFI-stub,
 * and GRUB xnu paths; x86 retains its own 16 MiB low-RAM load base. */
#if defined(__aarch64__)
#define KERNEL_LOAD_BASE 0x41000000ull
#else
#define KERNEL_LOAD_BASE 0x1000000ull
#endif

/* Where the BSP's own init is actually loaded. This is no longer
 * something the kernel decides or consults at all -- kernel_boot loads
 * init with target_base=0, meaning "wherever init's own vmaddr already
 * says" (see kernel/macho.h's own note on why), and init's own build
 * declares that vmaddr itself (this project's own reference
 * implementation does so via CMakeLists.txt's own -pagezero_size).
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
#if defined(__aarch64__)
#define BSP_INIT_LOAD_BASE 0x40200000ull /* 2 MiB into QEMU virt RAM */
#else
#define BSP_INIT_LOAD_BASE 0x1800000ull /* 24 MiB */
#endif

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

/* The new boot protocol's own fixed destinations (see
 * include/boot_protocol.h) -- where a loader baking a compile-time
 * InitDescription/boot-allocator objects (otos-boot, for a
 * known QEMU-virt-aarch64 target) copies them to, the same reasoning
 * mkstub.py's own kernel copy already uses: the EFI stub's OWN load
 * address is firmware's choice, not fixed, so anything that needs to
 * stay valid after control passes to saski needs a real, fixed
 * destination to be copied to, not just wherever the stub happened to
 * land. Carved out of the same gap CORE_PGTABLE_OFFSET's own comment
 * already describes (BSP_MAILBOX_REGION_BASE's generous slack ends at
 * 0x41880000; CORE_REGION_BASE starts at 0x42000000 -- 7.5 MiB between
 * them, of which this uses 1.0625 MiB).
 *
 * BOOT_ALLOCATOR_REGION: coreinit's boot allocator region. The loader
 * places InitDescription, the allocator object, and copied
 * allocator code at its front and initializes used past them; later
 * allocations continue from there. The 1.0625 MiB extent is deliberately
 * generous bookkeeping rather than a tight fit.
 *
 * x86_64 equivalents are not yet chosen -- this project's boot-protocol
 * work so far is aarch64-first (see otos-grub/otos-boot's own READMEs),
 * and guessing an x86_64 address here before any x86_64 loader exists
 * to use it would be exactly that, a guess. */
#if defined(__aarch64__)
#define BOOT_ALLOCATOR_REGION_BASE 0x41880000ull
#define BOOT_ALLOCATOR_REGION_SIZE 0x00110000ull
#endif

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
#if defined(__aarch64__)
/* Max 64 AP regions occupy 0x42000000..0x52000000. Leave 32 MiB
 * between those regions and this 64 MiB pool; keep another 128 MiB
 * before the end of QEMU virt's standard 512 MiB RAM/EFI tables. */
#define ALLOC_MIN 0x54000000ull
#define ALLOC_MAX 0x58000000ull
#else
#define ALLOC_MIN 0x1C00000ull  /* 28 MiB */
#define ALLOC_MAX CORE_REGION_BASE /* 64 MiB */
#endif
