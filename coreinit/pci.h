#pragma once

#include <stdint.h>

typedef struct {
    uint8_t bus, device, function;
    uint16_t vendor_id, device_id;
    uint8_t class_code, subclass, prog_if, revision;
    uint8_t header_type;
} pci_device_info_t;

typedef void (*pci_found_fn)(const pci_device_info_t *dev, void *user_data);

/* Scans PCI bus 0 for present devices, calling on_found for each one.
 *
 * This is deliberately the simplest correct mechanism, not the most
 * complete one: it uses the legacy port-based configuration access
 * method (CONFIG_ADDRESS/CONFIG_DATA at 0xCF8/0xCFC), which needs no
 * ACPI table parsing at all to get real hardware discovery working, and
 * it only scans bus 0. A PCI-to-PCI bridge (header_type 0x01) found on
 * bus 0 would have devices on a secondary bus this scan never visits --
 * QEMU's default machine puts everything on bus 0, so this is enough to
 * see real devices now, but a complete implementation would recurse into
 * any bridge it finds. Tracked as a known simplification, not silently
 * assumed to be the whole story. */
void pci_scan(pci_found_fn on_found, void *user_data);
