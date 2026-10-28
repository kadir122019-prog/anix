#include <anix/ata.h>
#include <anix/serial.h>

#define ATA_DATA 0x1f0u
#define ATA_SECTOR_COUNT 0x1f2u
#define ATA_LBA_LOW 0x1f3u
#define ATA_LBA_MID 0x1f4u
#define ATA_LBA_HIGH 0x1f5u
#define ATA_DRIVE 0x1f6u
#define ATA_STATUS 0x1f7u
#define ATA_COMMAND 0x1f7u
#define ATA_ALT_STATUS 0x3f6u
#define ATA_STATUS_ERR 0x01u
#define ATA_STATUS_DRQ 0x08u
#define ATA_STATUS_DF 0x20u
#define ATA_STATUS_BSY 0x80u
#define ATA_LBA28_LIMIT (1ULL << 28)
#define ATA_POLL_LIMIT 1000000u

static struct block_device primary_master = { .name = "ata0" };

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}
static inline uint16_t inw(uint16_t port) {
    uint16_t value;
    __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}
static inline void outw(uint16_t port, uint16_t value) {
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

static void ata_delay(void) {
    (void)inb(ATA_ALT_STATUS); (void)inb(ATA_ALT_STATUS);
    (void)inb(ATA_ALT_STATUS); (void)inb(ATA_ALT_STATUS);
}

static bool ata_wait(bool data_ready) {
    for (uint32_t attempt = 0; attempt < ATA_POLL_LIMIT; ++attempt) {
        const uint8_t status = inb(ATA_STATUS);
        if (status == 0 || status == 0xffu) return false;
        if (status & ATA_STATUS_BSY) continue;
        if (status & (ATA_STATUS_ERR | ATA_STATUS_DF)) return false;
        if (!data_ready || (status & ATA_STATUS_DRQ)) return true;
    }
    return false;
}

static void ata_select(uint64_t lba) {
    outb(ATA_DRIVE, (uint8_t)(0xe0u | ((lba >> 24) & 0x0fu)));
    ata_delay();
}

static bool ata_transfer(struct block_device *device, uint64_t lba, uint32_t count,
    void *buffer, bool write) {
    (void)device;
    if (lba >= ATA_LBA28_LIMIT || count > 256u || count > ATA_LBA28_LIMIT - lba) return false;
    uint8_t *bytes = buffer;
    for (uint32_t sector = 0; sector < count; ++sector) {
        const uint64_t current = lba + sector;
        ata_select(current);
        outb(ATA_SECTOR_COUNT, 1u);
        outb(ATA_LBA_LOW, (uint8_t)current);
        outb(ATA_LBA_MID, (uint8_t)(current >> 8));
        outb(ATA_LBA_HIGH, (uint8_t)(current >> 16));
        outb(ATA_COMMAND, write ? 0x30u : 0x20u);
        if (!ata_wait(true)) return false;
        if (write) {
            for (size_t word = 0; word < ANIX_BLOCK_SECTOR_SIZE / sizeof(uint16_t); ++word) {
                const uint16_t value = (uint16_t)bytes[word * 2u] | ((uint16_t)bytes[word * 2u + 1u] << 8);
                outw(ATA_DATA, value);
            }
            if (!ata_wait(false)) return false;
        } else {
            for (size_t word = 0; word < ANIX_BLOCK_SECTOR_SIZE / sizeof(uint16_t); ++word) {
                const uint16_t value = inw(ATA_DATA);
                bytes[word * 2u] = (uint8_t)value;
                bytes[word * 2u + 1u] = (uint8_t)(value >> 8);
            }
            if (!ata_wait(false)) return false;
        }
        bytes += ANIX_BLOCK_SECTOR_SIZE;
    }
    return true;
}

static bool ata_read(struct block_device *device, uint64_t lba, uint32_t count, void *buffer) {
    return ata_transfer(device, lba, count, buffer, false);
}

static bool ata_write(struct block_device *device, uint64_t lba, uint32_t count, const void *buffer) {
    if (!ata_transfer(device, lba, count, (void *)buffer, true)) return false;
    outb(ATA_COMMAND, 0xe7u); /* FLUSH CACHE */
    return ata_wait(false);
}

static bool ata_flush(struct block_device *device) {
    (void)device;
    outb(ATA_DRIVE, 0xe0u);
    ata_delay();
    outb(ATA_COMMAND, 0xe7u);
    return ata_wait(false);
}

bool ata_pio_init(void) {
    outb(ATA_DRIVE, 0xa0u);
    ata_delay();
    outb(ATA_SECTOR_COUNT, 0);
    outb(ATA_LBA_LOW, 0);
    outb(ATA_LBA_MID, 0);
    outb(ATA_LBA_HIGH, 0);
    outb(ATA_COMMAND, 0xecu);
    const uint8_t initial_status = inb(ATA_STATUS);
    if (initial_status == 0 || initial_status == 0xffu) return false;
    if (!ata_wait(true) || inb(ATA_LBA_MID) != 0 || inb(ATA_LBA_HIGH) != 0) return false;
    uint16_t identify[256];
    for (size_t i = 0; i < 256u; ++i) identify[i] = inw(ATA_DATA);
    if (!(identify[49] & (1u << 9))) return false;
    const uint64_t sectors = (uint64_t)identify[60] | ((uint64_t)identify[61] << 16);
    if (!sectors || sectors > ATA_LBA28_LIMIT) return false;
    primary_master.sector_count = sectors;
    primary_master.read_sectors = ata_read;
    primary_master.write_sectors = ata_write;
    primary_master.flush = ata_flush;
    if (!block_register(&primary_master)) return false;
    serial_write("[storage] ATA PIO disk ata0: "); serial_write_dec(sectors);
    serial_write(" sectors (LBA28)\n");
    return true;
}
