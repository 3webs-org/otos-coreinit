#include "topology.h"
#include "cluster_scheduler.h"
#include "serial.h"
#include "../include/fdt.h"
#include <stdint.h>
#include <stddef.h>

/* Backend wiring entry points. None is declared in a header -- each
 * backend's own file documents why -- so the topology backend (the
 * wiring point for this architecture) forward-declares them here. */
void serial_aarch64_set_base(uint64_t base);
void irq_aarch64_set_base(uint64_t base);
void irq_aarch64_set_cpu_base(uint64_t base);
void cross_core_signal_aarch64_set_base(void *base);
void cross_core_signal_aarch64_set_gic_v2(void);

/* Read MPIDR_EL1 (Multiprocessor Affinity Register) to get this core's
 * own affinity identifier. This is the aarch64 equivalent of reading the
 * LAPIC ID on x86_64 -- a hardware register that uniquely identifies this
 * core in the same namespace the device tree uses for its /cpus children. */
static inline uint64_t read_mpidr(void)
{
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return mpidr;
}

uint64_t topology_self_id(void)
{
    /* MPIDR_EL1 bit 31 is RES1 on this machine, not part of the affinity
     * ID. ACPI GICC and FDT /cpus declare only Aff3:Aff2:Aff1:Aff0;
     * strip MT/U/RES1 so the local ID matches those records and joins
     * the same cluster-scheduler node. */
    return read_mpidr() & 0xFF00FFFFFFull;
}

/* Helper to check if a node is enabled per device tree convention:
 * absent "status" property means enabled, "okay" or "ok" means enabled,
 * anything else (e.g. "disabled") means skip this node. */
static int node_enabled(const void *blob, int node_offset)
{
    uint32_t len = 0;
    const char *status = (const char *)fdt_getprop(blob, node_offset, "status", &len);
    if (!status) {
        /* No status property means enabled by default */
        return 1;
    }
    /* Check for "okay" or "ok" (both are valid per spec) */
    if (len >= 3 && status[0] == 'o' && status[1] == 'k') {
        if (status[2] == '\0' || (len >= 5 && status[2] == 'a' && status[3] == 'y' && status[4] == '\0')) {
            return 1;
        }
    }
    return 0;
}

/* Read the first cell of a node's "reg" property, sized according to the
 * parent's #address-cells. Returns 1 on success, 0 if no reg exists. */
static int read_reg_first_cell(const void *blob, int node_offset, uint64_t *out)
{
    uint32_t len = 0;
    const uint32_t *reg = (const uint32_t *)fdt_getprop(blob, node_offset, "reg", &len);
    if (!reg || len == 0) {
        return 0;
    }

    /* Get the parent's #address-cells to know how wide the first cell is.
     * fdt_cell_counts gives us the counts that size THIS node's reg entries,
     * which is exactly what we need here. */
    int addr_cells = 2; /* spec default */
    int size_cells = 2;
    fdt_cell_counts(blob, node_offset, &addr_cells, &size_cells);

    if (addr_cells == 1) {
        *out = (uint64_t)__builtin_bswap32(reg[0]);
    } else if (addr_cells == 2) {
        uint64_t hi = (uint64_t)__builtin_bswap32(reg[0]);
        uint64_t lo = (uint64_t)__builtin_bswap32(reg[1]);
        *out = (hi << 32) | lo;
    } else {
        /* Unexpected cell count -- fail rather than misread */
        return 0;
    }
    return 1;
}


/* ---- ACPI: the other standard arm64 machine description ----
 *
 * Arm SBSA/SBBR platforms -- and AAVMF by default -- publish ACPI rather
 * than a device tree. The kernel found the RSDP through the EFI
 * configuration table and handed it over in early_rsdp_phys, exactly as on
 * x86_64. The tables differ from x86's only in which MADT entries describe
 * the cores and interrupt controller:
 *   MADT GICC (type 0x0B): flags @12 (bit 0 enabled), MPIDR @68
 *   MADT GICD (type 0x0C): distributor physical base @8
 *   SPCR: interface type @36 (0x03 = PL011), base address @44 (GAS.address)
 * The timer frequency is not described in ACPI (GTDT carries interrupts
 * only); slot [2] stays 0 and timer_aarch64 reads CNTFRQ_EL0. */

typedef struct {
    char signature[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t extended_checksum;
    uint8_t reserved[3];
} __attribute__((packed)) acpi_rsdp_t;

typedef struct {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed)) acpi_sdt_t;

#define MADT_TYPE_GICC 0x0B
#define MADT_TYPE_GICD 0x0C
#define MADT_GICC_FLAG_ENABLED 0x1u
#define SPCR_TYPE_PL011 0x03

static uint8_t acpi_sum(const void *p, size_t len)
{
    const uint8_t *b = (const uint8_t *)p;
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum = (uint8_t)(sum + b[i]);
    }
    return sum;
}

static int acpi_sig(const char *sig, const char *expect, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (sig[i] != expect[i]) {
            return 0;
        }
    }
    return 1;
}

static int is_rsdp(uint64_t addr)
{
    const acpi_rsdp_t *r = (const acpi_rsdp_t *)(uintptr_t)addr;
    return acpi_sig(r->signature, "RSD PTR ", 8) && acpi_sum(r, 20) == 0;
}

static uint64_t le_read(const uint8_t *p, size_t n)
{
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        v |= (uint64_t)p[i] << (8 * i);
    }
    return v;
}

/* XSDT when the RSDP is 2.0+, otherwise RSDT; both are validated before
 * any entry is followed. */
static const acpi_sdt_t *acpi_find(const acpi_rsdp_t *rsdp, const char *sig)
{
    int wide = rsdp->revision >= 2 && rsdp->xsdt_address != 0;
    const acpi_sdt_t *root = wide
        ? (const acpi_sdt_t *)(uintptr_t)rsdp->xsdt_address
        : (const acpi_sdt_t *)(uintptr_t)rsdp->rsdt_address;
    if (!acpi_sig(root->signature, wide ? "XSDT" : "RSDT", 4) ||
        root->length < sizeof(acpi_sdt_t) || acpi_sum(root, root->length) != 0) {
        return NULL;
    }
    size_t esz = wide ? 8 : 4;
    size_t n = (root->length - sizeof(acpi_sdt_t)) / esz;
    const uint8_t *e = (const uint8_t *)root + sizeof(acpi_sdt_t);
    for (size_t i = 0; i < n; i++) {
        const acpi_sdt_t *t = (const acpi_sdt_t *)(uintptr_t)le_read(e + i * esz, esz);
        if (t && acpi_sig(t->signature, sig, 4) && acpi_sum(t, t->length) == 0) {
            return t;
        }
    }
    return NULL;
}

static int topology_discover_acpi(topology_t *out, uint64_t rsdp_phys)
{
    const acpi_rsdp_t *rsdp = (const acpi_rsdp_t *)(uintptr_t)rsdp_phys;
    if (rsdp->revision >= 2 && acpi_sum(rsdp, rsdp->length) != 0) {
        kprintf("[coreinit]   topology: RSDP at 0x%llX failed extended checksum\n",
                (unsigned long long)rsdp_phys);
        return 0;
    }
    kprintf("[coreinit]   topology: ACPI RSDP at 0x%llX, revision %u\n",
            (unsigned long long)rsdp_phys, (unsigned)rsdp->revision);

    const acpi_sdt_t *madt = acpi_find(rsdp, "APIC");
    if (!madt) {
        kprintf("[coreinit]   topology: MADT (APIC table) not found\n");
        return 0;
    }

    size_t found = 0, enabled = 0;
    uint64_t gicc_base = 0;
    uint8_t gic_version = 0;
    const uint8_t *p = (const uint8_t *)madt + sizeof(acpi_sdt_t) + 8;
    const uint8_t *end = (const uint8_t *)madt + madt->length;
    while (p + 2 <= end) {
        uint8_t type = p[0], len = p[1];
        if (len < 2 || p + len > end) {
            break; /* malformed -- stop rather than misread */
        }
        if (type == MADT_TYPE_GICC && len >= 76) {
            if (found >= TOPOLOGY_MAX_CORES) {
                kprintf("[coreinit]   topology: WARNING: found more than %u cores, capping\n",
                        (unsigned int)TOPOLOGY_MAX_CORES);
                break;
            }
            int en = (le_read(p + 12, 4) & MADT_GICC_FLAG_ENABLED) != 0;
            /* Only the affinity fields: MPIDR_EL1 bits 31 (RES1) and 30 (U)
             * are not part of the namespace topology_self_id compares in. */
            out->cores[found].target = le_read(p + 68, 8) & 0xFF00FFFFFFull;
            out->cores[found].enabled = (uint8_t)en;
            if (en && gicc_base == 0) {
                gicc_base = le_read(p + 32, 8);
            }
            found++;
            enabled += (size_t)en;
        } else if (type == MADT_TYPE_GICD && len >= 16) {
            out->arch_opaque[1] = le_read(p + 8, 8);
            if (len >= 21) {
                gic_version = p[20];
            }
        }
        p += len;
    }
    out->count = found;
    if (gic_version == 2 && gicc_base != 0) {
        irq_aarch64_set_cpu_base(gicc_base);
        cross_core_signal_aarch64_set_gic_v2();
    }
    kprintf("[coreinit]   topology: discovered %u cores (%u enabled)\n",
            (unsigned int)found, (unsigned int)enabled);
    if (out->arch_opaque[1] != 0) {
        kprintf("[coreinit]   topology: GIC distributor at 0x%llX\n",
                (unsigned long long)out->arch_opaque[1]);
    }

    const acpi_sdt_t *spcr = acpi_find(rsdp, "SPCR");
    if (spcr && spcr->length >= 52 &&
        ((const uint8_t *)spcr)[36] == SPCR_TYPE_PL011 &&
        ((const uint8_t *)spcr)[40] == 0 /* GAS: system memory */) {
        out->arch_opaque[0] = le_read((const uint8_t *)spcr + 44, 8);
        kprintf("[coreinit]   topology: PL011 UART at 0x%llX (SPCR)\n",
                (unsigned long long)out->arch_opaque[0]);
    }
    return 1;
}

static void topology_wire_backends(const topology_t *out)
{
    /* The topology backend wires its own architecture's backends -- the
     * shared flow stays neutral, same principle as the build script's
     * per-arch file lists. Each nonzero base is handed to the backend
     * that owns it; the GIC distributor feeds BOTH the IRQ backend and
     * the SGI signaling backend. irq_core_init's existing
     * NULL-tolerance means a missing GIC in the DT degrades gracefully:
     * no wiring happens, and the IRQ backend keeps its no-op behavior
     * instead of faulting. */
    if (out->arch_opaque[0] != 0) {
        serial_aarch64_set_base(out->arch_opaque[0]);
        kprintf("[coreinit]   topology: serial backend wired to UART at 0x%llX\n",
                (unsigned long long)out->arch_opaque[0]);
    }
    if (out->arch_opaque[1] != 0) {
        irq_aarch64_set_base(out->arch_opaque[1]);
        kprintf("[coreinit]   topology: irq backend wired to GICD at 0x%llX\n",
                (unsigned long long)out->arch_opaque[1]);
        cross_core_signal_aarch64_set_base(
            (void *)(uintptr_t)out->arch_opaque[1]);
        kprintf("[coreinit]   topology: cross-core-signal backend wired to "
                "GICD at 0x%llX\n",
                (unsigned long long)out->arch_opaque[1]);
    }
}

int topology_discover(topology_t *out, uint64_t arch_hint)
{
    /* aarch64 implementation: arch_hint is the platform's machine
     * description -- a DTB or an ACPI RSDP (see topology_boot_hint). 0
     * means "none supplied" (distinct from "supplied but invalid"). The
     * two formats are told apart by their own validated headers, never
     * by assumption. */
    out->count = 0;
    out->arch_opaque[0] = 0;
    out->arch_opaque[1] = 0;
    out->arch_opaque[2] = 0;
    out->arch_opaque[3] = 0;

    if (arch_hint == 0) {
        kprintf("[coreinit]   topology: no DTB or RSDP supplied (arch_hint == 0)\n");
        return 0;
    }

    if (is_rsdp(arch_hint)) {
        if (!topology_discover_acpi(out, arch_hint)) {
            return 0;
        }
        topology_wire_backends(out);
        return (out->count > 0) ? 1 : 0;
    }

    const void *blob = (const void *)(uintptr_t)arch_hint;
    const uint8_t *header = (const uint8_t *)blob;
    /* fdt_valid requires the supplied extent; zero always fails. The
     * kernel already validated this handoff, and the FDT header declares
     * its total size in big-endian bytes 4..7. Bound that extent before
     * asking the FDT parser to validate its internal offsets. */
    uint32_t blob_size = ((uint32_t)header[4] << 24) |
                         ((uint32_t)header[5] << 16) |
                         ((uint32_t)header[6] << 8) | header[7];
    if (blob_size < 40 || blob_size > 16 * 1024 * 1024 ||
        !fdt_valid(blob, blob_size)) {
        kprintf("[coreinit]   topology: 0x%llX is neither a valid DTB nor an RSDP\n",
               (unsigned long long)arch_hint);
        return 0;
    }

    kprintf("[coreinit]   topology: DTB valid at 0x%llX\n",
           (unsigned long long)arch_hint);

    /* Recorded verbatim (the arch_hint as received) so later backends can
     * keep reading the tree without anyone threading the DTB pointer
     * through their signatures -- smp_aarch64.c needs /psci out of this
     * same blob, and smp_bring_up_all is handed the topology, not the
     * blob. Left 0 on an ACPI boot: there is no tree to read. */
    out->arch_opaque[3] = arch_hint;

    /* Find /cpus node */
    int cpus_offset = fdt_path_offset(blob, "/cpus");
    if (cpus_offset == FDT_ERR_NODE) {
        kprintf("[coreinit]   topology: /cpus node not found\n");
        return 0;
    }

    /* Iterate children of /cpus to discover cores */
    int cursor = 0;
    int child_offset;
    size_t found_count = 0;
    size_t enabled_count = 0;

    while ((child_offset = fdt_next_child(blob, cpus_offset, &cursor)) != 0) {
        if (found_count >= TOPOLOGY_MAX_CORES) {
            kprintf("[coreinit]   topology: WARNING: found more than %u cores, capping\n",
                   (unsigned int)TOPOLOGY_MAX_CORES);
            break;
        }

        /* Skip disabled nodes */
        if (!node_enabled(blob, child_offset)) {
            continue;
        }

        /* Read the first cell of "reg" -- this is the MPIDR affinity value */
        uint64_t mpidr = 0;
        if (!read_reg_first_cell(blob, child_offset, &mpidr)) {
            /* No reg property, or malformed -- skip this node */
            continue;
        }

        out->cores[found_count].target = mpidr;
        out->cores[found_count].enabled = 1;
        found_count++;
        enabled_count++;
    }

    out->count = found_count;
    kprintf("[coreinit]   topology: discovered %u cores (%u enabled)\n",
           (unsigned int)found_count, (unsigned int)enabled_count);

    /* Discover device addresses for other backends and stash in arch_opaque.
     * Slot assignments (documented here for reference):
     *   [0] = PL011 UART base physical address
     *   [1] = GIC distributor base physical address
     *   [2] = ARM generic timer frequency (Hz), if declared in DT
     *   [3] = DTB physical address (the arch_hint as received) -- kept so
     *         an arch_opaque-only caller (smp_aarch64.c, which needs
     *         /psci) can reach the tree it was built from */

    /* Find PL011 UART -- must be enabled (the DT has a second one marked
     * disabled, which is deliberate test data). */
    int uart_offset = FDT_ERR_NODE;
    if (fdt_node_by_compatible(blob, "arm,pl011", &uart_offset)) {
        if (node_enabled(blob, uart_offset)) {
            fdt_range_t uart_range;
            if (fdt_reg_to_cpu_addr(blob, uart_offset, 0, &uart_range)) {
                out->arch_opaque[0] = uart_range.base;
                kprintf("[coreinit]   topology: PL011 UART at 0x%llX\n",
                       (unsigned long long)uart_range.base);
            }
        }
    }

    /* QEMU virt's default GICv2 advertises arm,cortex-a15-gic; v3
     * advertises arm,gic-v3. The MSI-only arm,gic-v2m-frame is not a
     * distributor and must not be mistaken for one. */
    int gic_offset = FDT_ERR_NODE;
    int gic_v3 = fdt_node_by_compatible(blob, "arm,gic-v3", &gic_offset);
    if (!gic_v3 &&
        !fdt_node_by_compatible(blob, "arm,cortex-a15-gic", &gic_offset)) {
        fdt_node_by_compatible(blob, "arm,gic", &gic_offset);
    }
    if (gic_offset != FDT_ERR_NODE && node_enabled(blob, gic_offset)) {
        fdt_range_t gic_range;
        if (!gic_v3) {
            fdt_range_t cpu_range;
            if (fdt_reg_to_cpu_addr(blob, gic_offset, 1, &cpu_range)) {
                irq_aarch64_set_cpu_base(cpu_range.base);
                cross_core_signal_aarch64_set_gic_v2();
            }
        }
        if (fdt_reg_to_cpu_addr(blob, gic_offset, 0, &gic_range)) {
            out->arch_opaque[1] = gic_range.base;
            kprintf("[coreinit]   topology: GIC distributor at 0x%llX\n",
                   (unsigned long long)gic_range.base);
        }
    }

    /* Find ARM generic timer frequency, if declared in /timer node */
    int timer_offset = fdt_path_offset(blob, "/timer");
    if (timer_offset != FDT_ERR_NODE) {
        uint32_t len = 0;
        const uint32_t *freq = (const uint32_t *)fdt_getprop(blob, timer_offset,
                                                              "clock-frequency", &len);
        if (freq && len >= 4) {
            out->arch_opaque[2] = (uint64_t)__builtin_bswap32(freq[0]);
            kprintf("[coreinit]   topology: timer frequency %llu Hz\n",
                   (unsigned long long)out->arch_opaque[2]);
        }
    }

    topology_wire_backends(out);

    return (out->count > 0) ? 1 : 0;
}

/* Exposes the discovered UART base address so coreinit_main's aarch64 path
 * can call serial_aarch64_set_base(topology_aarch64_uart_base(&topo)).
 * Not declared in a header -- the bring-up code will forward-declare it
 * where needed, or this can be promoted to topology.h later if multiple
 * call sites emerge. */
uint64_t topology_aarch64_uart_base(const topology_t *t)
{
    return t->arch_opaque[0];
}

uint64_t topology_timer_hint(const topology_t *t)
{
    /* Slot [2] is the generic-timer frequency on this architecture --
     * timer_core_init takes a frequency here, a LAPIC base on x86_64. */
    return t->arch_opaque[2];
}

uint64_t topology_cluster_arch(void)
{
    return CLUSTER_ARCH_AARCH64;
}

uint64_t topology_boot_hint(const struct init_boot_info *info)
{
    /* A standard arm64 platform describes itself with a device tree or
     * with ACPI -- AAVMF publishes ACPI by default and a DTB only when ACPI
     * is disabled -- and the kernel hands over whichever it found, in the
     * same two fields the x86_64 path uses. The DTB wins when both exist:
     * it is the description this backend reads most completely (/psci for
     * SMP bring-up). topology_discover tells the two apart by their own
     * validated headers, so the choice made here cannot be misread later. */
    return info->fdt_blob_phys ? info->fdt_blob_phys : info->early_rsdp_phys;
}
