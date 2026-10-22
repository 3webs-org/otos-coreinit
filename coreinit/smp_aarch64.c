/* aarch64 backend for smp.h. The x86_64 backend wakes each AP by hand: a
 * 16-bit trampoline copied to 0x8000, per-core parameters written into that
 * trampoline's own memory, and an INIT-SIPI-SIPI sequence poked through the
 * Local APIC's ICR registers. None of that is needed here. PSCI CPU_ON
 * starts a core directly at an entry point with a context value, in the
 * exception level and machine state firmware already set up -- the firmware
 * itself is the trampoline, so there is no real-mode stub, no page-table
 * hand-off, and no vector-number arithmetic anywhere in this file.
 * Everything ARM-specific about bringing up secondary cores lives in this
 * one file; nothing above smp.h changes to accommodate it. */
#include "smp.h"
#include "cluster_scheduler.h" /* RESOURCE_SYNC_TRANSFER, RESOURCE_KIND_NONE */
#include "mem_mgr.h"           /* MEM_KIND_CORE_REGION */
#include "mmio.h"
#include "scheduler.h"
#include "serial.h"
#include "../include/fdt.h"
#include "../include/smp_layout.h"

#include <stddef.h>
#include <stdint.h>

/* PSCI function IDs.
 *
 * CPU_ON's ID is deliberately NOT one of them: the /psci node declares its
 * own "cpu_on" value and this file uses the declared one (see psci_parse),
 * because the 32-bit and 64-bit calling conventions number the same call
 * differently (0x84000003 vs 0xC4000003) and which one firmware implements
 * is the device tree's answer, not this file's.
 *
 * VERSION has no DT property to read -- the binding never names it -- so
 * this one is the standard 0.2 value, which is also the only version this
 * file claims to speak. */
#define PSCI_FUNC_VERSION 0x84000000u

/* Every PSCI call reports success as 0 in the first return register. */
#define PSCI_SUCCESS 0

static void write_u64(uint64_t addr, uint64_t val)
{
    mmio_write64((volatile void *)(uintptr_t)addr, val);
}

/* Claims a region through the cluster scheduler's own
 * request_allocation (which wraps memory_manager's claim AND records node
 * attribution + a resource id) rather than calling memory_manager directly
 * -- "keeps track of any memory allocations they have requested" is the
 * cluster scheduler's own job now, not something to bypass even from within
 * smp code. Returns 1 if accepted, 0 if rejected. Same helper, same
 * semantics, as the x86_64 backend's private copy: each backend owns its
 * own, exactly the way each owns its own copy of the constants it needs. */
static int claim_region(uint64_t base, uint64_t size, uint64_t kind)
{
    uint64_t resource_id = 0;
    return scheduler_request_allocation(base, size, kind,
                                        RESOURCE_SYNC_TRANSFER, &resource_id);
}

/* Issues one PSCI call. Arguments travel in registers, not on the stack:
 * x0 is the function ID, x1..x3 the arguments, and the first return value
 * comes back in x0. The conduit is selected by the DT's "method" property
 * -- hvc traps to the hypervisor, smc traps to secure firmware -- and the
 * two are not interchangeable: the wrong one either hangs or returns
 * NOT_SUPPORTED, which is why psci_parse reads it instead of guessing.
 *
 * All four registers are in/out ("+r"), not just x0: firmware is allowed to
 * clobber the argument registers as well as the return registers, and
 * declaring them input-only would let the compiler assume they survived.
 * The "memory" clobber keeps anything this call was about from being
 * reordered across it.
 *
 * __asm__ rather than asm, on the register declarations: this project's
 * build compiles with -std=c11, where the bare 'asm' keyword is a GNU
 * extension and only the '__asm__' spelling is in the dialect -- the
 * constraint strings themselves are the same GNU extended-asm syntax
 * topology_aarch64.c and irq_aarch64.c already use for mrs/msr. */
static uint64_t psci_call(uint64_t function_id, uint64_t arg0, uint64_t arg1,
                          uint64_t arg2, int use_hvc)
{
    register uint64_t x0 __asm__("x0") = function_id;
    register uint64_t x1 __asm__("x1") = arg0;
    register uint64_t x2 __asm__("x2") = arg1;
    register uint64_t x3 __asm__("x3") = arg2;

    if (use_hvc) {
        __asm__ volatile("hvc #0"
                         : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
                         :
                         : "memory");
    } else {
        __asm__ volatile("smc #0"
                         : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
                         :
                         : "memory");
    }
    return x0;
}

/* Compares a string property's bytes against `word`, length-checked rather
 * than trusting the blob to be NUL-terminated: a malformed tree must yield
 * "no match", never a read past the end of the property. */
static int prop_is_string(const char *s, uint32_t len, const char *word)
{
    uint32_t n = 0;
    while (word[n] != '\0') {
        n++;
    }
    if (len < n + 1) {
        return 0; /* too short to hold the word AND its terminating NUL */
    }
    for (uint32_t i = 0; i < n; i++) {
        if (s[i] != word[i]) {
            return 0;
        }
    }
    return s[n] == '\0';
}

/* Reads the two facts this file needs from the DT's /psci node: which
 * conduit firmware listens on, and the function ID it expects for CPU_ON.
 * Returns 1 on success, 0 if /psci is absent or either property is missing
 * or unrecognisable -- a normal state on a platform whose firmware simply
 * does not offer PSCI, not a bug to work around. */
static int psci_parse(const void *blob, int *out_use_hvc,
                      uint64_t *out_cpu_on_fid)
{
    int node = fdt_path_offset(blob, "/psci");
    if (node == FDT_ERR_NODE) {
        return 0;
    }

    uint32_t len = 0;
    const char *method = (const char *)fdt_getprop(blob, node, "method", &len);
    if (!method) {
        return 0;
    }
    if (prop_is_string(method, len, "hvc")) {
        *out_use_hvc = 1;
    } else if (prop_is_string(method, len, "smc")) {
        *out_use_hvc = 0;
    } else {
        /* Anything else ("smc-extended", a truncated value, noise) is a
         * conduit this file does not know how to invoke. Refusing to bring
         * up cores is the honest answer; guessing is not. */
        return 0;
    }

    const uint32_t *cpu_on = (const uint32_t *)fdt_getprop(blob, node, "cpu_on",
                                                           &len);
    if (!cpu_on || len < 4) {
        return 0;
    }
    /* DT cell values are big-endian, always, regardless of the CPU's own
     * endianness -- the same reason topology_aarch64.c byteswaps every reg
     * and clock-frequency value it reads. */
    *out_cpu_on_fid = (uint64_t)__builtin_bswap32(cpu_on[0]);
    return 1;
}

/* ---------------------------------------------------------------------------
 * AP ENTRY CONTRACT -- what the ARM kernel_boot_ap side must provide.
 *
 * The AP arrives here, via PSCI CPU_ON, in this state and no other:
 *   - MMU OFF, caches OFF (the standard CPU_ON entry state; the AP enables
 *     its own caches once its own MMU setup is complete).
 *   - The SAME exception level the BSP runs at -- CPU_ON does not drop,
 *     raise, or otherwise change it.
 *   - x0 = the context_id this file passes, which is region_base. That is
 *     the ONLY thing the AP is told about itself: no parameter block, no
 *     pointer to a per-core struct -- just the base of the region the BSP
 *     claimed and filled for it. Everything else it derives from that base
 *     plus the shared constants in smp_layout.h.
 *   - PC = entry, which points into the COPIED image's own code, executing
 *     at physical addresses.
 *
 * Running a byte-for-byte copy of the kernel at a different physical address
 * is sound for the same reason the x86_64 backend's copy relies on: this
 * kernel is PIE, so the only code that ever runs from inside the copy
 * (kernel_boot_ap and whatever it calls) reaches its own data through
 * addresses relative to the instruction pointer, never through link-time
 * absolutes. On x86_64 that means RIP-relative lea/mov; here the same
 * property is held a fortiori by ADRP-relative addressing, which is the only
 * way aarch64 code can name its own literals at all -- a PIE aarch64 image
 * has no way to encode a stale absolute address even by accident.
 *
 * The AP's entry code (the ARM kernel_boot_ap, saski-side work) derives its
 * own stack from CORE_STACK_OFFSET/CORE_STACK_SIZE and its services from the
 * discovery record at CORE_DISCOVERY_OFFSET, exactly as its x86_64
 * counterpart does from the trampoline's parameter block. The difference is
 * only that the region itself is the whole handoff, because there is no
 * trampoline left to write parameters into.
 *
 * No ap_trampoline_aarch64.S exists, and none is needed: the trampoline's
 * entire job -- real-mode entry at a fixed low address, switching to a
 * 32-bit GDT, paging up into long mode, loading a stack, reading the
 * per-core parameters -- is done by firmware before CPU_ON ever returns
 * control. What remains is genuinely the AP's own boot code, which is the
 * same kernel_boot_ap the BSP would run.
 * -------------------------------------------------------------------------*/
void smp_bring_up_all(const topology_t *topo,
                      const struct init_boot_info *info,
                      capability_t memory_manager, capability_t mutex_manager,
                      uint64_t hardware_mutex_id, uint32_t lapic_calibration_count,
                      capability_t cluster_scheduler, capability_t serial_resource)
{
    if (info->kernel_image_addr == 0) {
        kprintf("[coreinit]   smp: no kernel image module available, "
                "cannot bring up additional cores\n");
        return;
    }

    /* /psci lives in the device tree, so this backend needs the blob's
     * address -- and smp_bring_up_all's signature carries the topology, not
     * the DTB pointer. topology_aarch64.c records arch_hint in
     * arch_opaque[3] for exactly this reason. */
    if (topo->arch_opaque[3] == 0) {
        kprintf("[coreinit]   smp: no DTB recorded in the topology "
                "(arch_opaque[3] == 0), cannot find /psci -- cannot bring "
                "up additional cores\n");
        return;
    }
    const void *blob = (const void *)(uintptr_t)topo->arch_opaque[3];

    int use_hvc = 0;
    uint64_t cpu_on_fid = 0;
    if (!psci_parse(blob, &use_hvc, &cpu_on_fid)) {
        kprintf("[coreinit]   smp: no usable /psci node in the DTB -- this "
                "platform's firmware does not offer PSCI the way this "
                "backend needs it, so no additional cores will be brought "
                "up (single-core boot continues normally)\n");
        return;
    }

    /* Ask firmware which PSCI it speaks before relying on its semantics:
     * CPU_ON's argument shape changed in 0.2, so an older firmware would
     * misread the MPIDR/entry/context triple below rather than cleanly
     * refusing it. VERSION is the call with no DT declaration to read,
     * hence the constant above. */
    uint64_t version = psci_call(PSCI_FUNC_VERSION, 0, 0, 0, use_hvc);
    if (version == (uint64_t)-1) {
        /* PSCI's error returns are negative, so a firmware that does not
         * implement VERSION at all answers NOT_SUPPORTED here. Read as a
         * version number that would be 65535.65532, which would sail past
         * the check below -- so it is rejected by name first. */
        kprintf("[coreinit]   smp: PSCI VERSION is not supported over %s -- "
                "cannot bring up additional cores\n",
                use_hvc ? "hvc" : "smc");
        return;
    }
    unsigned int major = (unsigned int)(version >> 16);
    unsigned int minor = (unsigned int)(version & 0xFFFFu);
    if (major == 0 && minor < 2) {
        kprintf("[coreinit]   smp: PSCI version %u.%u is older than 0.2, "
                "whose CPU_ON argument shape this backend uses -- cannot "
                "bring up additional cores\n",
                major, minor);
        return;
    }

    /* MPIDR, not an APIC ID: the same value topology_discover put in
     * cores[].target, read from this core's own hardware register, so the
     * comparison below excludes the BSP exactly the way the x86_64
     * backend's does. */
    uint64_t my_mpidr = topology_self_id();

    size_t region_index = 0;
    for (size_t i = 0; i < topo->count; i++) {
        uint64_t target = topo->cores[i].target;
        if (!topo->cores[i].enabled || target == my_mpidr) {
            continue;
        }

        uint64_t region_base = CORE_REGION_BASE + region_index * CORE_REGION_SIZE;
        region_index++;

        if (!claim_region(region_base, CORE_REGION_SIZE, MEM_KIND_CORE_REGION)) {
            kprintf("[coreinit]   smp: region 0x%llX for mpidr=0x%llX was "
                    "REJECTED (overlaps an existing claim) -- skipping "
                    "this core rather than silently using memory "
                    "something else already owns\n",
                    (unsigned long long)region_base,
                    (unsigned long long)target);
            continue;
        }

        kprintf("[coreinit]   smp: bringing up mpidr=0x%llX at region "
                "0x%llX (claimed)\n",
                (unsigned long long)target, (unsigned long long)region_base);

        /* A raw, byte-for-byte copy of this kernel's own currently-loaded
         * image, not a re-parsed/re-loaded one -- identical reasoning to
         * the x86_64 backend's: this kernel is PIE, already correctly laid
         * out in memory by the loader, and the only code that ever actually
         * runs from within the copy (kernel_boot_ap and whatever it calls)
         * is itself ADRP-relative, so nothing needs re-relocating. Bounded
         * against CORE_KERNEL_MAX_SIZE, the space this AP's own region
         * actually reserves for it -- exceeding that would silently corrupt
         * whatever comes next in the region (CORE_INIT_OFFSET) if left
         * unchecked. */
        if (info->kernel_image_len == 0 ||
            info->kernel_image_len > CORE_KERNEL_MAX_SIZE) {
            kprintf("[coreinit]   smp: this kernel's own image size (%u "
                    "bytes) is 0 or exceeds CORE_KERNEL_MAX_SIZE (%u) -- "
                    "skipping this core rather than risking a silent "
                    "out-of-bounds copy\n",
                    (unsigned)info->kernel_image_len,
                    (unsigned)CORE_KERNEL_MAX_SIZE);
            continue;
        }
        const uint8_t *kernel_src =
            (const uint8_t *)(uintptr_t)info->kernel_image_addr;
        uint8_t *kernel_dst =
            (uint8_t *)(uintptr_t)(region_base + CORE_KERNEL_OFFSET);
        for (uint64_t b = 0; b < info->kernel_image_len; b++) {
            kernel_dst[b] = kernel_src[b];
        }

        /* Discovery record: how this AP's own coreinit finds the
         * BSP-hosted services created before any of this started. Same
         * fields, same offsets as the x86_64 backend writes -- one layout,
         * two ways of getting an AP to read it. */
        write_u64(region_base + CORE_DISCOVERY_OFFSET + CORE_DISCOVERY_BSP_INVOKE_OFF,
                  (uint64_t)(uintptr_t)info->kernel_invoke);
        write_u64(region_base + CORE_DISCOVERY_OFFSET + CORE_DISCOVERY_MUTEX_MGR_OFF,
                  mutex_manager.handle);
        write_u64(region_base + CORE_DISCOVERY_OFFSET + CORE_DISCOVERY_MEM_MGR_OFF,
                  memory_manager.handle);
        write_u64(region_base + CORE_DISCOVERY_OFFSET + CORE_DISCOVERY_HW_MUTEX_ID_OFF,
                  hardware_mutex_id);
        /* Named lapic_calibration because x86_64 has no other way to know
         * how fast its timer counts: the BSP measures counts-per-10ms once.
         * The ARM generic timer is self-describing -- CNTFRQ_EL0 states its
         * own frequency on every PE -- so an AP calibrates itself and its
         * timer path ignores this field. It is written anyway, at the same
         * offset, so a copied image reads one discovery record layout
         * regardless of which backend prepared its region. */
        write_u64(region_base + CORE_DISCOVERY_OFFSET + CORE_DISCOVERY_LAPIC_CALIBRATION_OFF,
                  lapic_calibration_count);
        write_u64(region_base + CORE_DISCOVERY_OFFSET + CORE_DISCOVERY_CLUSTER_SCHED_OFF,
                  cluster_scheduler.handle);
        write_u64(region_base + CORE_DISCOVERY_OFFSET + CORE_DISCOVERY_SERIAL_RESOURCE_OFF,
                  serial_resource.handle);
        /* APs cannot discover the RSDP themselves on an EFI-only boot path
         * (no legacy-BIOS scan available), so the BSP writes the same
         * early_rsdp_phys it used into each AP's discovery record. */
        write_u64(region_base + CORE_DISCOVERY_OFFSET + CORE_DISCOVERY_RSDP_OFF,
                  info->early_rsdp_phys);

        /* The entry point is inside the copy, not in the BSP's own image:
         * the delta from where this kernel was linked to where it loaded is
         * the same PIE slide the copy needs, applied to kernel_boot_ap's
         * address. Adding it to region_base + CORE_KERNEL_OFFSET is what
         * makes the copied instance's kernel_boot_ap the one that runs --
         * the x86_64 backend computes this identical value to store in its
         * trampoline's parameter block; here it is a CPU_ON argument. */
        uint64_t entry = region_base + CORE_KERNEL_OFFSET +
                         (info->kernel_boot_ap_addr - info->kernel_load_base);

        /* A demo job for the AP's own scheduler_run to pick up on its very
         * first GET_NEXT_JOB call, once it's booted and joined -- submitted
         * with THIS specific AP's target as the affinity, so only it (not
         * the BSP, not any other AP) is eligible to run it. Queued BEFORE
         * CPU_ON for the same reason the x86_64 backend submits before
         * sending SIPI: the AP starts polling the moment it joins.
         *
         * The affinity is the AP's target and the ctx is this core's own
         * identity -- the identity-as-node-id pattern x86_64 already uses,
         * where apic_id names the BSP. Here that identifier is the MPIDR,
         * so the ARM join path in coreinit.c must join the cluster
         * scheduler with its own MPIDR for a greeting to match its
         * recipient: a documented contract between the two ends of this one
         * call, honored by the later coreinit ARM-flow unit. */
        scheduler_submit_to_node(target, JOB_TYPE_GREET_FROM_BSP,
                                 my_mpidr, 95, 0,
                                 RESOURCE_KIND_NONE, 0);

        /* The entire "wakeup" on this architecture: x1 is the target's
         * MPIDR, x2 the entry point, x3 the context id -- which this
         * backend hands over as region_base, because that is the one value
         * the AP needs and the one it cannot obtain from anywhere else
         * (see the entry contract above). */
        uint64_t ret = psci_call(cpu_on_fid, target, entry, region_base, use_hvc);
        if (ret == PSCI_SUCCESS) {
            kprintf("[coreinit]   smp: mpidr=0x%llX brought up via PSCI "
                    "CPU_ON at entry 0x%llX\n",
                    (unsigned long long)target, (unsigned long long)entry);
        } else {
            kprintf("[coreinit]   smp: PSCI CPU_ON for mpidr=0x%llX was "
                    "rejected, return code 0x%llX\n",
                    (unsigned long long)target, (unsigned long long)ret);
        }
    }
}
