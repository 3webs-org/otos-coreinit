#include "serial.h"
#include "mmio.h"
#include "core_state.h"
#include <stdarg.h>
#include <stdint.h>

/* PL011 UART register offsets from base address. */
#define PL011_DR     0x00  /* Data register */
#define PL011_FR     0x18  /* Flag register */
#define PL011_IBRD   0x24  /* Integer baud rate divisor */
#define PL011_FBRD   0x28  /* Fractional baud rate divisor */
#define PL011_LCR_H  0x2C  /* Line control register */
#define PL011_CR     0x30  /* Control register */
#define PL011_IMSC   0x38  /* Interrupt mask set/clear */
#define PL011_ICR    0x44  /* Interrupt clear */

/* Flag register bits */
#define PL011_FR_TXFF (1 << 5)  /* Transmit FIFO full */
#define PL011_FR_RXFE (1 << 4)  /* Receive FIFO empty */
#define PL011_FR_TXFE (1 << 7)  /* Transmit FIFO empty */

/* Line control register bits */
#define PL011_LCR_H_FEN  (1 << 4)  /* FIFO enable */
#define PL011_LCR_H_WLEN_8 (3 << 5)  /* 8-bit word length */

/* Control register bits */
#define PL011_CR_UARTEN (1 << 0)  /* UART enable */
#define PL011_CR_TXE    (1 << 8)  /* Transmit enable */
#define PL011_CR_RXE    (1 << 9)  /* Receive enable */

#define CADENCE_CR 0x00
#define CADENCE_CR_RX_DIS (1 << 3)
#define CADENCE_CR_TX_EN (1 << 4)
#define CADENCE_CR_TX_DIS (1 << 5)
#define CADENCE_SR 0x2C
#define CADENCE_FIFO 0x30
#define CADENCE_SR_TXFULL (1 << 4)

/* UART base address, discovered from device tree at runtime. Zero means
 * "not yet configured" -- serial_init() with base==0 sets g_ready=0 so
 * every output call becomes a cheap no-op rather than faulting or
 * silently pretending to work. Once serial_aarch64_set_base() is called,
 * the UART is initialized and g_ready is set to 1.
 *
 * This design exists because the DTB is not parsed yet when serial_init()
 * is first called (topology_aarch64.c does not exist), but we must not
 * block on FDT availability -- the serial layer must tolerate being
 * initialized before the base address is known, and silently discard
 * output until the base is set. */
static uint64_t g_pl011_base = 0;
static int g_is_cadence = 0;
static int g_ready = 0;

/* core_tag now lives in struct core_state (see core_state.h's own note
 * on why -1, not core_state's zero default, means "not yet set"). Falls
 * back to untagged if this core has no core_state yet, the same posture
 * as every other core_state consumer. */
void serial_set_core_tag(int core_id)
{
    struct core_state *cs = core_state_get();
    if (cs != NULL) {
        cs->core_tag = core_id;
    }
}

/* Called by the FDT parser (or boot code) once the UART base address is
 * known. Initializes the PL011 and marks the serial layer ready. */
void serial_aarch64_set_base(uint64_t base)
{
    g_pl011_base = base;
    g_is_cadence = 0;
    
    /* Disable UART during configuration */
    mmio_write32((volatile void *)(g_pl011_base + PL011_CR), 0);
    
    /* Set baud rate divisors for 115200 baud (assuming 24 MHz UARTCLK):
     * BAUDDIV = (24000000 / (16 * 115200)) = 13.02
     * IBRD = 13, FBRD = round(0.02 * 64) = 1 */
    mmio_write32((volatile void *)(g_pl011_base + PL011_IBRD), 13);
    mmio_write32((volatile void *)(g_pl011_base + PL011_FBRD), 1);
    
    /* 8N1, FIFO enabled */
    mmio_write32((volatile void *)(g_pl011_base + PL011_LCR_H),
                 PL011_LCR_H_WLEN_8 | PL011_LCR_H_FEN);
    
    /* Disable all interrupts */
    mmio_write32((volatile void *)(g_pl011_base + PL011_IMSC), 0);
    
    /* Enable UART, transmit, and receive */
    mmio_write32((volatile void *)(g_pl011_base + PL011_CR),
                 PL011_CR_UARTEN | PL011_CR_TXE | PL011_CR_RXE);
    
    g_ready = 1;
}

void serial_aarch64_set_cadence_base(uint64_t base)
{
    g_pl011_base = base;
    g_is_cadence = 1;
    uint32_t control = mmio_read32(
        (const volatile void *)(g_pl011_base + CADENCE_CR));
    control &= ~(CADENCE_CR_TX_DIS | CADENCE_CR_RX_DIS);
    control |= CADENCE_CR_TX_EN;
    mmio_write32((volatile void *)(g_pl011_base + CADENCE_CR), control);
    g_ready = 1;
}

void serial_init(void)
{
    /* If base address is not yet known, mark as not ready. Output calls
     * will be no-ops until serial_aarch64_set_base() is called. */
    if (g_pl011_base == 0) {
        g_ready = 0;
        return;
    }
    
    /* Base is already set, initialize now */
    serial_aarch64_set_base(g_pl011_base);
}

static int transmit_ready(void)
{
    uint64_t status = g_is_cadence ? CADENCE_SR : PL011_FR;
    uint32_t full = g_is_cadence ? CADENCE_SR_TXFULL : PL011_FR_TXFF;
    uint32_t value = mmio_read32(
        (const volatile void *)(g_pl011_base + status));
    return !(value & full);  /* Ready when FIFO is NOT full */
}

static void putc(char c)
{
    if (!g_ready) {
        return;  /* Silently discard output until base is set */
    }
    
    /* Wait until transmit FIFO has space. Do NOT write without checking
     * TXFF -- a full FIFO silently drops bytes and that looks like
     * corrupt output. */
    while (!transmit_ready()) {
    }
    
    mmio_write32((volatile void *)(g_pl011_base +
                 (g_is_cadence ? CADENCE_FIFO : PL011_DR)), (uint32_t)c);
}

static void puts_raw(const char *s)
{
    while (*s) {
        if (*s == '\n') {
            putc('\r');
        }
        putc(*s++);
    }
}

static void put_uint(unsigned long v, unsigned base, int upper)
{
    char buf[32];
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int i = 0;

    if (v == 0) {
        putc('0');
        return;
    }
    while (v > 0 && i < (int)sizeof(buf)) {
        buf[i++] = digits[v % base];
        v /= base;
    }
    while (i > 0) {
        putc(buf[--i]);
    }
}

static void put_int(long v)
{
    if (v < 0) {
        putc('-');
        put_uint((unsigned long)(-v), 10, 0);
    } else {
        put_uint((unsigned long)v, 10, 0);
    }
}

/* Minimal freestanding formatter: %s %d %u %x %X %c %p, their ll
 * integer forms, and %%. Not a general libc printf. */
void kvprintf(const char *fmt, va_list ap)
{
    struct core_state *cs = core_state_get();
    if (cs != NULL && cs->core_tag >= 0) {
        putc('[');
        put_uint((unsigned long)cs->core_tag, 10, 0);
        putc(']');
        putc(' ');
    }

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            if (*p == '\n') {
                putc('\r');
            }
            putc(*p);
            continue;
        }
        p++;
        if (*p == 'l' && p[1] == 'l') {
            p += 2;
            if (*p == 'u') {
                put_uint(va_arg(ap, unsigned long long), 10, 0);
            } else if (*p == 'x' || *p == 'X') {
                put_uint(va_arg(ap, unsigned long long), 16, *p == 'X');
            } else if (*p == 'd') {
                put_int(va_arg(ap, long long));
            } else {
                putc('%');
                putc('l');
                putc('l');
                putc(*p);
            }
            continue;
        }
        switch (*p) {
        case 's':
            puts_raw(va_arg(ap, const char *));
            break;
        case 'd':
            put_int(va_arg(ap, int));
            break;
        case 'u':
            put_uint(va_arg(ap, unsigned int), 10, 0);
            break;
        case 'x':
            put_uint(va_arg(ap, unsigned int), 16, 0);
            break;
        case 'X':
            put_uint(va_arg(ap, unsigned int), 16, 1);
            break;
        case 'c':
            putc((char)va_arg(ap, int));
            break;
        case 'p':
            putc('0');
            putc('x');
            put_uint((unsigned long)(uintptr_t)va_arg(ap, void *), 16, 0);
            break;
        case '%':
            putc('%');
            break;
        default:
            putc('%');
            putc(*p);
            break;
        }
    }
}

void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
}
