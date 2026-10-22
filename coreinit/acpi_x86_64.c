#include "acpi.h"
#include "serial.h"
#include <stdint.h>

typedef struct {
    char signature[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address;
    /* ACPI 2.0+ only, valid when revision >= 2 */
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t extended_checksum;
    uint8_t reserved[3];
} __attribute__((packed)) rsdp_t;

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
} __attribute__((packed)) sdt_header_t;

static uint8_t sum_bytes(const void *p, size_t len)
{
    const uint8_t *b = (const uint8_t *)p;
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum = (uint8_t)(sum + b[i]);
    }
    return sum;
}

static int signature_matches(const char *sig, const char *expect, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (sig[i] != expect[i]) {
            return 0;
        }
    }
    return 1;
}

/* Validates a single candidate RSDP at a known address -- the
 * signature and checksum checks scan_for_rsdp applies inline, factored
 * out so acpi_discover can apply the exact same validation to an
 * address it didn't find itself (an early_rsdp_phys handed in from
 * boot.S), rather than trusting it blindly. */
static const rsdp_t *validate_rsdp(uintptr_t addr)
{
    const rsdp_t *candidate = (const rsdp_t *)addr;
    if (!signature_matches(candidate->signature, "RSD PTR ", 8)) {
        return NULL;
    }
    if (sum_bytes(candidate, 20) != 0) {
        return NULL;
    }
    if (candidate->revision >= 2 && sum_bytes(candidate, candidate->length) != 0) {
        return NULL;
    }
    return candidate;
}

/* Scans a physical memory range on 16-byte boundaries for the RSDP
 * signature, per the ACPI spec's own documented search algorithm --
 * there is no other way to find it on a legacy-BIOS/Multiboot system
 * (no UEFI configuration table available to us here). */
static const rsdp_t *scan_for_rsdp(uintptr_t start, uintptr_t end)
{
    for (uintptr_t addr = start; addr < end; addr += 16) {
        const rsdp_t *found = validate_rsdp(addr);
        if (found) {
            return found;
        }
    }
    return NULL;
}

static const rsdp_t *find_rsdp(void)
{
    /* EBDA base segment is a 16-bit real-mode segment value at physical
     * address 0x40E; the actual physical address is that value shifted
     * left 4 bits. Scan its first 1KB first, per spec. */
    uint16_t ebda_segment = *(const uint16_t *)(uintptr_t)0x40E;
    uintptr_t ebda_addr = (uintptr_t)ebda_segment << 4;
    if (ebda_addr != 0) {
        const rsdp_t *found = scan_for_rsdp(ebda_addr, ebda_addr + 1024);
        if (found) {
            return found;
        }
    }

    /* Fall back to the BIOS read-only memory range, also per spec --
     * where a real, legacy BIOS always places it. Only reached at all
     * when acpi_discover's own early_rsdp_phys is 0 -- boot.S normally
     * always supplies one; see that field's own comment. */
    return scan_for_rsdp(0xE0000, 0x100000);
}

static const sdt_header_t *find_table_rsdt(uint32_t rsdt_phys,
                                           const char *signature)
{
    const sdt_header_t *rsdt = (const sdt_header_t *)(uintptr_t)rsdt_phys;
    if (!signature_matches(rsdt->signature, "RSDT", 4)) {
        return NULL;
    }
    if (sum_bytes(rsdt, rsdt->length) != 0) {
        return NULL;
    }
    uint32_t entry_count = (rsdt->length - sizeof(sdt_header_t)) / 4;
    const uint32_t *entries =
        (const uint32_t *)((const uint8_t *)rsdt + sizeof(sdt_header_t));
    for (uint32_t i = 0; i < entry_count; i++) {
        const sdt_header_t *table = (const sdt_header_t *)(uintptr_t)entries[i];
        if (signature_matches(table->signature, signature, 4) &&
            sum_bytes(table, table->length) == 0) {
            return table;
        }
    }
    return NULL;
}

static const sdt_header_t *find_table_xsdt(uint64_t xsdt_phys,
                                           const char *signature)
{
    const sdt_header_t *xsdt = (const sdt_header_t *)(uintptr_t)xsdt_phys;
    if (!signature_matches(xsdt->signature, "XSDT", 4)) {
        return NULL;
    }
    if (sum_bytes(xsdt, xsdt->length) != 0) {
        return NULL;
    }
    uint32_t entry_count = (xsdt->length - sizeof(sdt_header_t)) / 8;
    const uint64_t *entries =
        (const uint64_t *)((const uint8_t *)xsdt + sizeof(sdt_header_t));
    for (uint32_t i = 0; i < entry_count; i++) {
        const sdt_header_t *table = (const sdt_header_t *)(uintptr_t)entries[i];
        if (signature_matches(table->signature, signature, 4) &&
            sum_bytes(table, table->length) == 0) {
            return table;
        }
    }
    return NULL;
}

#define MADT_TYPE_LOCAL_APIC 0
#define MADT_LAPIC_FLAG_ENABLED 0x1u

int acpi_discover(acpi_info_t *out, uint64_t early_rsdp_phys)
{
    out->cpu_count = 0;
    out->local_apic_address = 0;

    const rsdp_t *rsdp = early_rsdp_phys ? validate_rsdp((uintptr_t)early_rsdp_phys)
                                         : find_rsdp();
    if (!rsdp) {
        kprintf("[coreinit]   acpi: RSDP not found\n");
        return 0;
    }
    kprintf("[coreinit]   acpi: RSDP found, revision %u\n", rsdp->revision);

    const sdt_header_t *madt = NULL;
    if (rsdp->revision >= 2 && rsdp->xsdt_address != 0) {
        madt = find_table_xsdt(rsdp->xsdt_address, "APIC");
    }
    if (!madt) {
        madt = find_table_rsdt(rsdp->rsdt_address, "APIC");
    }
    if (!madt) {
        kprintf("[coreinit]   acpi: MADT (APIC table) not found\n");
        return 0;
    }

    const uint8_t *body = (const uint8_t *)madt + sizeof(sdt_header_t);
    out->local_apic_address = *(const uint32_t *)body;
    /* skip local_apic_address (4 bytes) + flags (4 bytes) */
    const uint8_t *p = body + 8;
    const uint8_t *end = (const uint8_t *)madt + madt->length;

    while (p < end && out->cpu_count < ACPI_MAX_CPUS) {
        uint8_t type = p[0];
        uint8_t len = p[1];
        if (len < 2) {
            break; /* malformed entry -- stop rather than loop forever */
        }
        if (type == MADT_TYPE_LOCAL_APIC && len >= 8) {
            uint8_t apic_id = p[3];
            uint32_t flags = (uint32_t)p[4] | ((uint32_t)p[5] << 8) |
                             ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
            out->cpus[out->cpu_count].apic_id = apic_id;
            out->cpus[out->cpu_count].enabled =
                (flags & MADT_LAPIC_FLAG_ENABLED) ? 1 : 0;
            out->cpu_count++;
        }
        p += len;
    }

    return 1;
}
