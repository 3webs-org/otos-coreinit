#include "smp.h"
#include "ap_trampoline_blob.h"
#include "capnp_build.h"
#include "cluster_scheduler.h"
#include "mem_mgr.h"
#include "mmio.h"
#include "scheduler.h"
#include "serial.h"
#include "../include/smp_layout.h"

#define LAPIC_REG_ID 0x020
#define LAPIC_REG_ICR_LOW 0x300
#define LAPIC_REG_ICR_HIGH 0x310
#define ICR_DELIVERY_STATUS_PENDING (1u << 12)

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) gdt_ptr_t;

/* The "real" 64-bit GDT every core ends up using after the trampoline's
 * own scratch 32-bit GDT gets it into long mode -- matching the layout
 * (null, 0x08=code64, 0x10=data64) kernel/boot.S's own gdt64 already
 * uses, so downstream code (irq.c's hardcoded selector) needs no
 * per-core-specific knowledge. One shared instance: its CONTENT is
 * identical for every core, so there's no reason to duplicate it. */
static const uint64_t g_ap_gdt64[3] = {
    0,
    0x00AF9A000000FFFFull, /* 64-bit code */
    0x00AF92000000FFFFull, /* 64-bit data */
};
static gdt_ptr_t g_ap_gdt_ptr;

uint8_t smp_read_bsp_apic_id(uint32_t lapic_base)
{
    uint32_t reg = mmio_read32((const volatile void *)(uintptr_t)(lapic_base + LAPIC_REG_ID));
    return (uint8_t)((reg >> 24) & 0xFF);
}

static uint32_t lapic_read(uint32_t lapic_base, uint32_t reg)
{
    return mmio_read32((const volatile void *)(uintptr_t)(lapic_base + reg));
}

static void lapic_write(uint32_t lapic_base, uint32_t reg, uint32_t val)
{
    mmio_write32((volatile void *)(uintptr_t)(lapic_base + reg), val);
}

/* Every ICR-based send (INIT/SIPI here, and cross_core_signal_send's
 * own self-IPI) shares this SAME hardware register -- reusing it while
 * a prior send is still "pending" (bit 12) is a genuine hardware-level
 * race, not just a software one: whichever send is in flight can be
 * corrupted or silently dropped by the next write. Real hardware
 * resolves this in a handful of cycles, so a short busy-wait here is
 * the conventional, correct way to use the ICR -- same category as
 * crude_delay's own INIT-SIPI-SIPI timing below (a bounded, one-time,
 * hardware-mandated wait during bring-up), not the kind of
 * software-contention spin removed elsewhere in this codebase. */
static void lapic_wait_icr_idle(uint32_t lapic_base)
{
    while (lapic_read(lapic_base, LAPIC_REG_ICR_LOW) &
          ICR_DELIVERY_STATUS_PENDING) {
    }
}

static void crude_delay(uint32_t iterations)
{
    for (volatile uint32_t i = 0; i < iterations; i++) {
    }
}

static void send_init_sipi_sipi(uint32_t lapic_base, uint8_t apic_id)
{
    uint8_t vector = (uint8_t)(AP_TRAMPOLINE_ADDR >> 12);

    lapic_wait_icr_idle(lapic_base); /* a prior, unrelated send (e.g.
                                      * cross_core_signal_send's own
                                      * self-test IPI) may still be in
                                      * flight -- see the note on this
                                      * helper */
    lapic_write(lapic_base, LAPIC_REG_ICR_HIGH, (uint32_t)apic_id << 24);
    lapic_write(lapic_base, LAPIC_REG_ICR_LOW, 0x4500); /* INIT, edge, assert */
    crude_delay(20000000); /* crude ~10ms-order busy wait -- no calibrated
                            * timer exists yet to do this precisely */

    lapic_wait_icr_idle(lapic_base);
    lapic_write(lapic_base, LAPIC_REG_ICR_HIGH, (uint32_t)apic_id << 24);
    lapic_write(lapic_base, LAPIC_REG_ICR_LOW, 0x4600u | vector); /* SIPI #1 */
    crude_delay(1000000);

    lapic_wait_icr_idle(lapic_base);
    lapic_write(lapic_base, LAPIC_REG_ICR_HIGH, (uint32_t)apic_id << 24);
    lapic_write(lapic_base, LAPIC_REG_ICR_LOW, 0x4600u | vector); /* SIPI #2 */
    crude_delay(1000000);
}

static void write_u64(uint64_t addr, uint64_t val)
{
    mmio_write64((volatile void *)(uintptr_t)addr, val);
}

/* Claims a region via the memory manager over an ORDINARY local invoke
 * -- smp.c runs on the BSP, which hosts that object directly, so this
 * is exactly the same kind of call coreinit makes to any other local
 * object, nothing SMP-specific about it. Returns 1 if the claim was
 * accepted, 0 if it was rejected (overlapping an existing claim) or the
 * call itself failed. */
/* Claims a region through the cluster scheduler's own
 * request_allocation (which wraps memory_manager's claim AND records
 * node attribution + a resource id) rather than calling memory_manager
 * directly -- "keeps track of any memory allocations they have
 * requested" is the cluster scheduler's own job now, not something to
 * bypass even from within smp.c. Returns 1 if accepted, 0 if rejected. */
static int claim_region(uint64_t base, uint64_t size, uint64_t kind)
{
    uint64_t resource_id = 0;
    return scheduler_request_allocation(base, size, kind,
                                        RESOURCE_SYNC_TRANSFER, &resource_id);
}

void smp_bring_up_all(const acpi_info_t *acpi, uint8_t bsp_apic_id,
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

    g_ap_gdt_ptr.limit = sizeof(g_ap_gdt64) - 1;
    g_ap_gdt_ptr.base = (uint64_t)(uintptr_t)g_ap_gdt64;

    /* The trampoline blob itself never changes between cores; copy it
     * to its fixed load address once. */
    uint8_t *trampoline_dst = (uint8_t *)(uintptr_t)AP_TRAMPOLINE_ADDR;
    for (size_t i = 0; i < sizeof(ap_trampoline_blob); i++) {
        trampoline_dst[i] = ap_trampoline_blob[i];
    }

    size_t region_index = 0;
    for (size_t i = 0; i < acpi->cpu_count; i++) {
        if (!acpi->cpus[i].enabled || acpi->cpus[i].apic_id == bsp_apic_id) {
            continue;
        }

        uint64_t region_base = CORE_REGION_BASE + region_index * CORE_REGION_SIZE;
        region_index++;

        if (!claim_region(region_base, CORE_REGION_SIZE, MEM_KIND_CORE_REGION)) {
            kprintf("[coreinit]   smp: region 0x%X for apic_id=%u was "
                    "REJECTED (overlaps an existing claim) -- skipping "
                    "this core rather than silently using memory "
                    "something else already owns\n",
                    (unsigned)region_base, (unsigned)acpi->cpus[i].apic_id);
            continue;
        }

        kprintf("[coreinit]   smp: bringing up apic_id=%u at region 0x%X "
                "(claimed)\n",
                (unsigned)acpi->cpus[i].apic_id, (unsigned)region_base);

        /* A raw, byte-for-byte copy of this kernel's own currently-
         * loaded image, not a re-parsed/re-loaded one -- see
         * kernel/macho.h's own macho_image_size comment for the full
         * reasoning: this kernel is PIE, already correctly laid out
         * in memory by GRUB, and the only code that ever actually
         * runs from within the copy (kernel_boot_ap and whatever it
         * calls) is itself PIE/RIP-relative, so nothing needs
         * re-relocating. Bounded against CORE_KERNEL_MAX_SIZE, the
         * space this AP's own region actually reserves for it --
         * exceeding that would silently corrupt whatever comes next
         * in the region (CORE_INIT_OFFSET) if left unchecked. */
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

        uint64_t entry64 = region_base + CORE_KERNEL_OFFSET +
                           (info->kernel_boot_ap_addr - info->kernel_load_base);
        uint64_t stack_top =
            region_base + CORE_STACK_OFFSET + CORE_STACK_SIZE;

        /* Discovery record: how this AP's own coreinit finds the
         * BSP-hosted services created before any of this started. */
        write_u64(region_base + CORE_DISCOVERY_OFFSET + CORE_DISCOVERY_BSP_INVOKE_OFF,
                 (uint64_t)(uintptr_t)info->kernel_invoke);
        write_u64(region_base + CORE_DISCOVERY_OFFSET + CORE_DISCOVERY_MUTEX_MGR_OFF,
                 mutex_manager.handle);
        write_u64(region_base + CORE_DISCOVERY_OFFSET + CORE_DISCOVERY_MEM_MGR_OFF,
                 memory_manager.handle);
        write_u64(region_base + CORE_DISCOVERY_OFFSET + CORE_DISCOVERY_HW_MUTEX_ID_OFF,
                 hardware_mutex_id);
        write_u64(region_base + CORE_DISCOVERY_OFFSET + CORE_DISCOVERY_LAPIC_CALIBRATION_OFF,
                 lapic_calibration_count);
        write_u64(region_base + CORE_DISCOVERY_OFFSET + CORE_DISCOVERY_CLUSTER_SCHED_OFF,
                 cluster_scheduler.handle);
        write_u64(region_base + CORE_DISCOVERY_OFFSET + CORE_DISCOVERY_SERIAL_RESOURCE_OFF,
                 serial_resource.handle);

        write_u64(AP_TRAMPOLINE_ADDR + AP_PARAM_PML4_OFF, info->pml4_phys);
        write_u64(AP_TRAMPOLINE_ADDR + AP_PARAM_GDT64_PTR_OFF,
                 (uint64_t)(uintptr_t)&g_ap_gdt_ptr);
        write_u64(AP_TRAMPOLINE_ADDR + AP_PARAM_ENTRY64_OFF, entry64);
        write_u64(AP_TRAMPOLINE_ADDR + AP_PARAM_STACK_OFF, stack_top);
        write_u64(AP_TRAMPOLINE_ADDR + AP_PARAM_REGION_BASE_OFF, region_base);

        /* A demo job for the AP's own scheduler_run to pick up on its
         * very first GET_NEXT_JOB call, once it's booted and joined --
         * submitted with THIS specific AP's apic_id as the affinity, so
         * only it (not the BSP, not any other AP) is eligible to run
         * it. */
        scheduler_submit_to_node(acpi->cpus[i].apic_id, JOB_TYPE_GREET_FROM_BSP,
                                 (uint64_t)bsp_apic_id, 95, 0,
                                 RESOURCE_KIND_NONE, 0);

        send_init_sipi_sipi(acpi->local_apic_address,
                            acpi->cpus[i].apic_id);
    }
}
