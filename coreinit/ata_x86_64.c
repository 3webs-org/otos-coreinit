#include "ata.h"
#include "arch_x86_64.h"

#define ATA_PRIMARY_IO 0x1F0
#define ATA_PRIMARY_CTRL 0x3F6

#define ATA_REG_DATA (ATA_PRIMARY_IO + 0)
#define ATA_REG_SECCOUNT (ATA_PRIMARY_IO + 2)
#define ATA_REG_LBA_LOW (ATA_PRIMARY_IO + 3)
#define ATA_REG_LBA_MID (ATA_PRIMARY_IO + 4)
#define ATA_REG_LBA_HIGH (ATA_PRIMARY_IO + 5)
#define ATA_REG_DRIVE_HEAD (ATA_PRIMARY_IO + 6)
#define ATA_REG_COMMAND (ATA_PRIMARY_IO + 7)
#define ATA_REG_STATUS (ATA_PRIMARY_IO + 7)

#define ATA_STATUS_ERR 0x01
#define ATA_STATUS_DRQ 0x08
#define ATA_STATUS_BSY 0x80

#define ATA_CMD_READ_SECTORS 0x20

/* Real hardware needs ~400ns after selecting a drive before its status
 * is meaningful; reading the (otherwise unused here) alternate status
 * register four times is the standard way to burn that time without a
 * real timer. Harmless and unnecessary under QEMU's emulation, kept for
 * correctness against real hardware this driver hasn't been tested
 * against yet. */
static void ata_400ns_delay(void)
{
    for (int i = 0; i < 4; i++) {
        inb(ATA_PRIMARY_CTRL);
    }
}

int ata_read_sector(uint32_t lba, void *buf512)
{
    /* Master drive, LBA mode, top 4 LBA bits in the low nibble. */
    outb(ATA_REG_DRIVE_HEAD, (uint8_t)(0xE0 | ((lba >> 24) & 0x0F)));
    ata_400ns_delay();

    outb(ATA_REG_SECCOUNT, 1);
    outb(ATA_REG_LBA_LOW, (uint8_t)(lba & 0xFF));
    outb(ATA_REG_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_REG_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFF));
    outb(ATA_REG_COMMAND, ATA_CMD_READ_SECTORS);

    /* Poll for completion -- no IRQ binding exists yet (still a listed
     * TODO), so this is a busy-wait, not an interrupt-driven wait. */
    uint8_t status;
    int spins = 0;
    do {
        status = inb(ATA_REG_STATUS);
        spins++;
        if (spins > 1000000) {
            return 0; /* timeout: drive never became ready */
        }
    } while (status & ATA_STATUS_BSY);

    if (status & ATA_STATUS_ERR) {
        return 0;
    }
    if (!(status & ATA_STATUS_DRQ)) {
        return 0;
    }

    uint16_t *dst = (uint16_t *)buf512;
    for (int i = 0; i < 256; i++) {
        dst[i] = inw(ATA_REG_DATA);
    }

    return 1;
}
