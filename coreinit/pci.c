#include "pci.h"
#include "arch_x86_64.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA 0xCFC

static uint32_t pci_config_read32(uint8_t bus, uint8_t device, uint8_t function,
                                  uint8_t offset)
{
    uint32_t address = (1u << 31) | ((uint32_t)bus << 16) |
                       ((uint32_t)device << 11) | ((uint32_t)function << 8) |
                       (offset & 0xFCu);
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

static uint16_t pci_config_read16(uint8_t bus, uint8_t device, uint8_t function,
                                  uint8_t offset)
{
    uint32_t dword = pci_config_read32(bus, device, function, offset);
    return (uint16_t)(dword >> ((offset & 2u) * 8u));
}

static uint8_t pci_config_read8(uint8_t bus, uint8_t device, uint8_t function,
                                uint8_t offset)
{
    uint32_t dword = pci_config_read32(bus, device, function, offset);
    return (uint8_t)(dword >> ((offset & 3u) * 8u));
}

void pci_scan(pci_found_fn on_found, void *user_data)
{
    const uint8_t bus = 0;

    for (uint16_t device = 0; device < 32; device++) {
        for (uint8_t function = 0; function < 8; function++) {
            uint16_t vendor_id =
                pci_config_read16(bus, (uint8_t)device, function, 0x00);
            if (vendor_id == 0xFFFF) {
                if (function == 0) {
                    break; /* nothing at all in this device slot */
                }
                continue; /* this function absent; sibling functions might not be */
            }

            pci_device_info_t info;
            info.bus = bus;
            info.device = (uint8_t)device;
            info.function = function;
            info.vendor_id = vendor_id;
            info.device_id =
                pci_config_read16(bus, (uint8_t)device, function, 0x02);
            info.revision = pci_config_read8(bus, (uint8_t)device, function, 0x08);
            info.prog_if = pci_config_read8(bus, (uint8_t)device, function, 0x09);
            info.subclass = pci_config_read8(bus, (uint8_t)device, function, 0x0A);
            info.class_code =
                pci_config_read8(bus, (uint8_t)device, function, 0x0B);
            info.header_type =
                pci_config_read8(bus, (uint8_t)device, function, 0x0E);

            on_found(&info, user_data);

            if (function == 0 && !(info.header_type & 0x80u)) {
                break; /* not multi-function: skip functions 1-7 */
            }
        }
    }
}
