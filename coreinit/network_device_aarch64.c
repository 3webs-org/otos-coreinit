#include "network_device.h"
#include "mmio.h"
#include "serial.h"
#include "../include/fdt.h"

#define VIRTIO_MAGIC 0x74726976u
#define VIRTIO_DEVICE_NET 1u
#define VIRTIO_STATUS_ACK 1u
#define VIRTIO_STATUS_DRIVER 2u
#define VIRTIO_STATUS_DRIVER_OK 4u
#define VIRTIO_STATUS_FEATURES_OK 8u
#define VIRTIO_STATUS_FAILED 128u
#define VIRTIO_F_VERSION_1 32u
#define VIRTIO_NET_F_MAC 5u

#define REG_MAGIC 0x000
#define REG_VERSION 0x004
#define REG_DEVICE_ID 0x008
#define REG_DEVICE_FEATURES 0x010
#define REG_DEVICE_FEATURES_SEL 0x014
#define REG_DRIVER_FEATURES 0x020
#define REG_DRIVER_FEATURES_SEL 0x024
#define REG_QUEUE_SEL 0x030
#define REG_QUEUE_NUM_MAX 0x034
#define REG_QUEUE_NUM 0x038
#define REG_QUEUE_READY 0x044
#define REG_QUEUE_NOTIFY 0x050
#define REG_INTERRUPT_STATUS 0x060
#define REG_INTERRUPT_ACK 0x064
#define REG_STATUS 0x070
#define REG_QUEUE_DESC_LOW 0x080
#define REG_QUEUE_DESC_HIGH 0x084
#define REG_QUEUE_DRIVER_LOW 0x090
#define REG_QUEUE_DRIVER_HIGH 0x094
#define REG_QUEUE_DEVICE_LOW 0x0a0
#define REG_QUEUE_DEVICE_HIGH 0x0a4
#define REG_CONFIG 0x100

#define QUEUE_SIZE 8u
#define BUFFER_SIZE 2048u
#define NET_HEADER_SIZE 10u /* MRG_RXBUF is deliberately not negotiated. */
#define DESC_F_WRITE 2u

typedef struct {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed)) virtq_desc_t;

typedef struct {
    uint16_t flags;
    volatile uint16_t idx;
    uint16_t ring[QUEUE_SIZE];
} __attribute__((packed)) virtq_avail_t;

typedef struct {
    uint32_t id;
    uint32_t len;
} __attribute__((packed)) virtq_used_elem_t;

typedef struct {
    uint16_t flags;
    volatile uint16_t idx;
    virtq_used_elem_t ring[QUEUE_SIZE];
} __attribute__((packed)) virtq_used_t;

typedef struct {
    virtq_desc_t desc[QUEUE_SIZE] __attribute__((aligned(16)));
    virtq_avail_t avail __attribute__((aligned(2)));
    virtq_used_t used __attribute__((aligned(4)));
} __attribute__((aligned(4096))) virtq_t;

static struct {
    uint64_t base;
    uint8_t mac[6];
    uint16_t rx_last_used;
    uint16_t tx_last_used;
    int ready;
    virtq_t rx;
    virtq_t tx;
    uint8_t rx_buffers[QUEUE_SIZE][BUFFER_SIZE] __attribute__((aligned(16)));
    uint8_t tx_buffer[BUFFER_SIZE] __attribute__((aligned(16)));
} g_net;

static uint32_t rd(uint32_t reg)
{
    return mmio_read32((const volatile void *)(uintptr_t)(g_net.base + reg));
}

static void wr(uint32_t reg, uint32_t value)
{
    mmio_write32((volatile void *)(uintptr_t)(g_net.base + reg), value);
}

static void barrier(void)
{
    __asm__ volatile("dmb ish" ::: "memory");
}

static void zero_bytes(void *p, size_t len)
{
    uint8_t *b = (uint8_t *)p;
    for (size_t i = 0; i < len; i++) b[i] = 0;
}

static void copy_bytes(void *dst, const void *src, size_t len)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < len; i++) d[i] = s[i];
}

static void set_addr(uint32_t low_reg, uint64_t address)
{
    wr(low_reg, (uint32_t)address);
    wr(low_reg + 4, (uint32_t)(address >> 32));
}

static int setup_queue(uint32_t index, virtq_t *q)
{
    wr(REG_QUEUE_SEL, index);
    if (rd(REG_QUEUE_NUM_MAX) < QUEUE_SIZE || rd(REG_QUEUE_READY)) return 0;
    zero_bytes(q, sizeof(*q));
    wr(REG_QUEUE_NUM, QUEUE_SIZE);
    set_addr(REG_QUEUE_DESC_LOW, (uint64_t)(uintptr_t)q->desc);
    set_addr(REG_QUEUE_DRIVER_LOW, (uint64_t)(uintptr_t)&q->avail);
    set_addr(REG_QUEUE_DEVICE_LOW, (uint64_t)(uintptr_t)&q->used);
    wr(REG_QUEUE_READY, 1);
    return rd(REG_QUEUE_READY) == 1;
}

int network_device_init(const void *blob, uint64_t node_id)
{
    if (!blob) return 0;
    int node = FDT_ERR_NODE;
    fdt_range_t range;
    for (;;) {
        node = fdt_next_node_by_compatible_offset(blob, "virtio,mmio", node);
        if (node == FDT_ERR_NODE) {
            kprintf("[coreinit] virtio-net: no network transport found\n");
            return 0;
        }
        if (!fdt_is_enabled(blob, node) ||
            !fdt_reg_to_cpu_addr(blob, node, 0, &range)) continue;
        g_net.base = range.base;
        if (rd(REG_MAGIC) == VIRTIO_MAGIC && rd(REG_VERSION) == 2 &&
            rd(REG_DEVICE_ID) == VIRTIO_DEVICE_NET) break;
    }

    wr(REG_STATUS, 0);
    wr(REG_STATUS, VIRTIO_STATUS_ACK);
    wr(REG_STATUS, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);
    wr(REG_DEVICE_FEATURES_SEL, 1);
    uint32_t high = rd(REG_DEVICE_FEATURES);
    if (!(high & 1u)) {
        wr(REG_STATUS, VIRTIO_STATUS_FAILED);
        return 0;
    }
    wr(REG_DEVICE_FEATURES_SEL, 0);
    uint32_t low = rd(REG_DEVICE_FEATURES);
    uint32_t accept_low = low & (1u << VIRTIO_NET_F_MAC);
    wr(REG_DRIVER_FEATURES_SEL, 0);
    wr(REG_DRIVER_FEATURES, accept_low);
    wr(REG_DRIVER_FEATURES_SEL, 1);
    wr(REG_DRIVER_FEATURES, 1u << (VIRTIO_F_VERSION_1 - 32));
    uint32_t status = VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER |
                      VIRTIO_STATUS_FEATURES_OK;
    wr(REG_STATUS, status);
    if (!(rd(REG_STATUS) & VIRTIO_STATUS_FEATURES_OK)) return 0;

    if (!setup_queue(0, &g_net.rx) || !setup_queue(1, &g_net.tx)) {
        wr(REG_STATUS, status | VIRTIO_STATUS_FAILED);
        return 0;
    }
    for (uint16_t i = 0; i < QUEUE_SIZE; i++) {
        g_net.rx.desc[i].addr = (uint64_t)(uintptr_t)g_net.rx_buffers[i];
        g_net.rx.desc[i].len = BUFFER_SIZE;
        g_net.rx.desc[i].flags = DESC_F_WRITE;
        g_net.rx.avail.ring[i] = i;
    }
    barrier();
    g_net.rx.avail.idx = QUEUE_SIZE;
    barrier();
    wr(REG_QUEUE_NOTIFY, 0);

    if (accept_low) {
        volatile const uint8_t *config =
            (volatile const uint8_t *)(uintptr_t)(g_net.base + REG_CONFIG);
        for (unsigned i = 0; i < 6; i++) g_net.mac[i] = config[i];
    } else {
        g_net.mac[0] = 0x02;
        g_net.mac[5] = (uint8_t)node_id;
    }
    wr(REG_STATUS, status | VIRTIO_STATUS_DRIVER_OK);
    g_net.ready = 1;
    return 1;
}

int network_device_present(void) { return g_net.ready; }
const uint8_t *network_device_mac(void) { return g_net.mac; }

int network_device_send(const void *frame, size_t len)
{
    if (!g_net.ready || !frame || len > BUFFER_SIZE - NET_HEADER_SIZE) return 0;
    zero_bytes(g_net.tx_buffer, NET_HEADER_SIZE);
    copy_bytes(g_net.tx_buffer + NET_HEADER_SIZE, frame, len);
    g_net.tx.desc[0].addr = (uint64_t)(uintptr_t)g_net.tx_buffer;
    g_net.tx.desc[0].len = (uint32_t)(NET_HEADER_SIZE + len);
    g_net.tx.desc[0].flags = 0;
    uint16_t avail = g_net.tx.avail.idx;
    g_net.tx.avail.ring[avail % QUEUE_SIZE] = 0;
    barrier();
    g_net.tx.avail.idx = (uint16_t)(avail + 1);
    barrier();
    wr(REG_QUEUE_NOTIFY, 1);
    for (uint32_t spins = 0; spins < 10000000u; spins++) {
        barrier();
        if (g_net.tx.used.idx != g_net.tx_last_used) {
            g_net.tx_last_used = g_net.tx.used.idx;
            return 1;
        }
    }
    return 0;
}

int network_device_poll(void *frame, size_t capacity, size_t *out_len)
{
    if (out_len) *out_len = 0;
    if (!g_net.ready || !frame || g_net.rx.used.idx == g_net.rx_last_used)
        return 0;
    barrier();
    virtq_used_elem_t elem =
        g_net.rx.used.ring[g_net.rx_last_used % QUEUE_SIZE];
    g_net.rx_last_used++;
    if (elem.id >= QUEUE_SIZE || elem.len < NET_HEADER_SIZE) return 0;
    size_t len = elem.len - NET_HEADER_SIZE;
    if (len > capacity) len = capacity;
    copy_bytes(frame, g_net.rx_buffers[elem.id] + NET_HEADER_SIZE, len);
    uint16_t avail = g_net.rx.avail.idx;
    g_net.rx.avail.ring[avail % QUEUE_SIZE] = (uint16_t)elem.id;
    barrier();
    g_net.rx.avail.idx = (uint16_t)(avail + 1);
    barrier();
    wr(REG_QUEUE_NOTIFY, 0);
    uint32_t irq = rd(REG_INTERRUPT_STATUS);
    if (irq) wr(REG_INTERRUPT_ACK, irq);
    if (out_len) *out_len = len;
    return 1;
}
