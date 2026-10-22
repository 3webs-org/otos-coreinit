#include "topology.h"
#include "serial.h"
#include "../include/fdt.h"
#include <stdint.h>
#include <stddef.h>

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
    /* Return the MPIDR affinity value directly. The device tree's /cpus
     * children declare their "reg" property in this same namespace, so
     * this value can be matched against topology_t.cores[].target to
     * identify which entry IS this core. */
    return read_mpidr();
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

int topology_discover(topology_t *out, uint64_t arch_hint)
{
    /* aarch64 implementation: arch_hint is the DTB physical address.
     * 0 means "no DTB supplied" (distinct from "DTB supplied but invalid"). */
    out->count = 0;
    out->arch_opaque[0] = 0;
    out->arch_opaque[1] = 0;
    out->arch_opaque[2] = 0;
    out->arch_opaque[3] = 0;

    if (arch_hint == 0) {
        kprintf("[coreinit]   topology: no DTB supplied (arch_hint == 0)\n");
        return 0;
    }

    const void *blob = (const void *)(uintptr_t)arch_hint;
    if (!fdt_valid(blob, 0)) {
        kprintf("[coreinit]   topology: DTB at 0x%llX failed validation\n",
               (unsigned long long)arch_hint);
        return 0;
    }

    kprintf("[coreinit]   topology: DTB valid at 0x%llX\n",
           (unsigned long long)arch_hint);

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
     *   [3] = unused/reserved */

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

    /* Find GIC (try v3 first, fall back to generic "arm,gic") */
    int gic_offset = FDT_ERR_NODE;
    if (!fdt_node_by_compatible(blob, "arm,gic-v3", &gic_offset)) {
        fdt_node_by_compatible(blob, "arm,gic", &gic_offset);
    }
    if (gic_offset != FDT_ERR_NODE && node_enabled(blob, gic_offset)) {
        fdt_range_t gic_range;
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
