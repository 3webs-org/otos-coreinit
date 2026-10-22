#include "irq.h"
#include "arch_x86_64.h"
#include "capnp_build.h"
#include "schema.h"
#include "serial.h"

#define IDT_ENTRIES 256
#define KERNEL_CODE_SELECTOR 0x08 /* boot.S's gdt64_code -- see kernel/boot.S */

typedef struct {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed)) idt_entry_t;

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) idt_ptr_t;

static idt_entry_t g_idt[IDT_ENTRIES];

/* IRQ line -> capability to invoke (method 0, message = IRQ number) on
 * every fire. handle 0 doubles as "unbound", same
 * convention as everywhere else caller/target identity is tracked in
 * this project. */
static capability_t g_irq_targets[16];

static int (*g_kernel_invoke)(capability_t target, uint32_t method_id,
                              const void *msg, size_t len, void *response,
                              size_t response_cap, size_t *out_response_len);

static void idt_set_gate_ist(int vector, void *handler, uint8_t selector,
                             uint8_t type_attr, uint8_t ist);

static void idt_set_gate(int vector, void *handler, uint8_t selector,
                         uint8_t type_attr)
{
    idt_set_gate_ist(vector, handler, selector, type_attr, 0);
}

static void idt_set_gate_ist(int vector, void *handler, uint8_t selector,
                             uint8_t type_attr, uint8_t ist)
{
    uint64_t addr = (uint64_t)(uintptr_t)handler;
    g_idt[vector].offset_low = (uint16_t)(addr & 0xFFFFu);
    g_idt[vector].selector = selector;
    g_idt[vector].ist = ist;
    g_idt[vector].type_attr = type_attr;
    g_idt[vector].offset_mid = (uint16_t)((addr >> 16) & 0xFFFFu);
    g_idt[vector].offset_high = (uint32_t)((addr >> 32) & 0xFFFFFFFFu);
    g_idt[vector].reserved = 0;
}

static inline void load_idt(uint16_t limit, uint64_t base)
{
    idt_ptr_t ptr;
    ptr.limit = limit;
    ptr.base = base;
    __asm__ volatile("lidt %0" : : "m"(ptr));
}

static inline void enable_interrupts(void)
{
    __asm__ volatile("sti");
}

/* Standard 8259 PIC remap sequence: without this, IRQ0-7 fire on
 * vectors 0x08-0x0F, colliding with CPU exception vectors (double
 * fault, etc.) -- remapping to 0x20-0x27 (master) / 0x28-0x2F (slave)
 * is the well-established fix every real-mode-descended x86 OS needs.
 *
 * Masks EVERYTHING (both PICs, 0xFF) -- found the hard way that
 * unmasking IRQ0/IRQ1 here was a real, genuinely intermittent bug:
 * coreinit.c's own early bring-up calls this, then immediately calls
 * irq_setup_idt_only (which wipes every IDT gate, including whatever
 * this function just enabled delivery for) before ever getting to
 * irq_init's own gate installation. In that window, an unmasked IRQ0
 * -- and the PIT genuinely is still ticking, nothing stops it here --
 * firing into an empty gate is exactly a #GP with error code 0x102
 * (IDT-referenced, selector index 0x20), reproduced directly and
 * confirmed via GDB. Masking everything here means remapping the
 * vectors out of exception-collision range is the ONLY thing this
 * function promises -- actually wanting delivery is an explicit,
 * separate opt-in (see irq_init's own unmask, right after ITS gates
 * are actually in place) rather than a side effect a caller might not
 * expect. */
void pic_remap(void)
{
    outb(0x20, 0x11); /* ICW1: begin init, ICW4 will follow */
    outb(0xA0, 0x11);
    outb(0x21, 0x20); /* ICW2: master offset = 0x20 */
    outb(0xA1, 0x28); /* ICW2: slave offset = 0x28 */
    outb(0x21, 0x04); /* ICW3: slave PIC is at master's IRQ2 */
    outb(0xA1, 0x02); /* ICW3: slave's own cascade identity */
    outb(0x21, 0x01); /* ICW4: 8086 mode */
    outb(0xA1, 0x01);
    outb(0x21, 0xFF); /* mask: everything -- see this function's own comment */
    outb(0xA1, 0xFF); /* slave: everything masked too */
}

/* PIT channel 0, mode 3 (square wave), rate generator -- fires IRQ0 at
 * the given frequency. 1193182 Hz is the PIT's fixed input clock. */
static void pit_init(uint32_t hz)
{
    uint32_t divisor = 1193182u / hz;
    outb(0x43, 0x36);
    outb(0x40, (uint8_t)(divisor & 0xFFu));
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFFu));
}

/* clang/gcc's x86 interrupt-attribute calling convention needs this
 * exact frame shape declared -- it's not a builtin type, the compiler
 * just knows how to generate the matching prologue/epilogue (including
 * the final iretq) for a function taking a pointer to it. */
struct interrupt_frame {
    uint64_t ip;
    uint64_t cs;
    uint64_t flags;
    uint64_t sp;
    uint64_t ss;
};

static uint64_t g_tick_count = 0;

static void init_irq_manager_schema(void);

/* --- #DF (double fault) diagnostics via IST ---
 *
 * Without this, ANY double fault is unrecoverable by construction: this
 * codebase never installs a #DF handler (vector 8) or a #GP handler
 * (vector 13), and never loads a TSS at all (no ltr anywhere) -- so a
 * double fault has nowhere valid to vector to, which itself becomes a
 * SECOND double fault, escalating straight to a triple fault (CPU
 * reset) with zero diagnostic output. That's exactly what was observed
 * chasing a rare (~1-in-10), timing-dependent crash: the underlying
 * fault's own state was never visible, only the silent reset.
 *
 * The fix is an IST (Interrupt Stack Table) entry: #DF (and, since it
 * commonly precedes a #DF in exactly this kind of corrupted-stack
 * scenario, #GP too) get their own dedicated, known-good stack,
 * specified in the TSS and referenced from the IDT gate's own ist
 * field -- the CPU switches to it automatically on entry, unconditionally,
 * regardless of what RSP was doing beforehand. This means a #DF or #GP
 * with an already-wrecked RSP can still run a real handler and report
 * the actual faulting state, rather than compounding into a silent
 * reset. */
typedef struct {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed)) tss64_t;

#define FAULT_IST_STACK_SIZE 8192
static uint8_t g_fault_ist_stack[FAULT_IST_STACK_SIZE] __attribute__((aligned(16)));
static tss64_t g_tss;

/* A fresh GDT with room for a TSS descriptor -- whatever GDT was
 * already active (boot.S's, for the BSP) has no slack for one, so this
 * builds a full replacement rather than trying to extend it in place.
 * Same code64/data64 entries at the same selector values (0x08, 0x10)
 * boot.S's own gdt64 already uses, so every existing selector constant
 * in this codebase stays valid without any other change; a new TSS
 * selector is added at 0x18. */
static uint64_t g_gdt_with_tss[5] __attribute__((aligned(16)));

__attribute__((interrupt)) static void isr_double_fault(
    struct interrupt_frame *frame, uint64_t error_code)
{
    (void)error_code; /* #DF's error code is architecturally always 0 */
    kprintf("[coreinit] DOUBLE FAULT -- faulting rip=0x%X cs=0x%X "
           "rflags=0x%X rsp=0x%X ss=0x%X\n",
           (unsigned)frame->ip, (unsigned)frame->cs, (unsigned)frame->flags,
           (unsigned)frame->sp, (unsigned)frame->ss);
    for (;;) {
        __asm__ volatile("hlt");
    }
}

__attribute__((interrupt)) static void isr_general_protection(
    struct interrupt_frame *frame, uint64_t error_code)
{
    kprintf("[coreinit] GENERAL PROTECTION FAULT -- error_code=0x%X "
           "faulting rip=0x%X cs=0x%X rflags=0x%X rsp=0x%X ss=0x%X\n",
           (unsigned)error_code, (unsigned)frame->ip, (unsigned)frame->cs,
           (unsigned)frame->flags, (unsigned)frame->sp, (unsigned)frame->ss);
    for (;;) {
        __asm__ volatile("hlt");
    }
}

static void setup_tss_and_ist(void)
{
    for (size_t i = 0; i < FAULT_IST_STACK_SIZE; i++) {
        g_fault_ist_stack[i] = 0;
    }
    uint64_t ist_top =
        (uint64_t)(uintptr_t)g_fault_ist_stack + FAULT_IST_STACK_SIZE;
    ist_top &= ~0xFull; /* defensive -- the buffer's own alignment and
                         * size already guarantee this, but the CPU
                         * requires a 16-byte-aligned stack at interrupt
                         * entry and this is cheap insurance */

    for (size_t i = 0; i < sizeof(g_tss); i++) {
        ((uint8_t *)&g_tss)[i] = 0;
    }
    g_tss.ist1 = ist_top;
    g_tss.iopb_offset = sizeof(tss64_t); /* no I/O bitmap -- point past
                                          * the TSS itself, which is the
                                          * documented way to say "none" */

    g_gdt_with_tss[0] = 0;
    g_gdt_with_tss[1] = 0x00AF9A000000FFFFull; /* 64-bit code, same as
                                                * boot.S's gdt64 */
    g_gdt_with_tss[2] = 0x00AF92000000FFFFull; /* 64-bit data, likewise */

    uint64_t tss_base = (uint64_t)(uintptr_t)&g_tss;
    uint32_t tss_limit = (uint32_t)sizeof(tss64_t) - 1;
    uint64_t desc_low = (uint64_t)(tss_limit & 0xFFFFu) |
                        ((tss_base & 0xFFFFFFull) << 16) |
                        (0x89ull << 40) | /* present, DPL0, type=available
                                          * 64-bit TSS */
                        (((uint64_t)(tss_limit >> 16) & 0xFull) << 48) |
                        (((tss_base >> 24) & 0xFFull) << 56);
    uint64_t desc_high = (tss_base >> 32) & 0xFFFFFFFFull;
    g_gdt_with_tss[3] = desc_low;
    g_gdt_with_tss[4] = desc_high;

    idt_ptr_t gdt_ptr; /* same {limit, base} shape as idt_ptr_t -- reused
                        * rather than declaring an identical struct */
    gdt_ptr.limit = (uint16_t)(sizeof(g_gdt_with_tss) - 1);
    gdt_ptr.base = (uint64_t)(uintptr_t)g_gdt_with_tss;
    __asm__ volatile("lgdt %0" : : "m"(gdt_ptr));

    /* lgdt alone doesn't change CS -- that needs a far jump/ret, which
     * this skips because the new table's code64 entry is byte-identical
     * to the old one at the same selector (0x08), so CS is already
     * valid against the new GDT with no reload. The data segment
     * registers DO need an explicit reload: "mov to a segment register"
     * is what actually makes the CPU re-validate against the (new)
     * GDT, and skipping it would leave stale, unvalidated descriptor
     * cache entries in place. */
    __asm__ volatile(
        "mov $0x10, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "mov %%ax, %%ss\n\t"
        : : : "ax");

    __asm__ volatile("ltr %0" : : "r"((uint16_t)0x18));

    idt_set_gate_ist(8 /* #DF */, (void *)isr_double_fault,
                     KERNEL_CODE_SELECTOR, 0x8E, 1);
    idt_set_gate_ist(13 /* #GP */, (void *)isr_general_protection,
                     KERNEL_CODE_SELECTOR, 0x8E, 1);

    kprintf("[coreinit]   irq: TSS loaded, IST1 (0x%X bytes) reserved for "
           "#DF/#GP -- a double fault can no longer silently "
           "triple-fault without reporting its own state first\n",
           (unsigned)FAULT_IST_STACK_SIZE);
}

static void dispatch_irq(uint8_t irq)
{
    capability_t target = g_irq_targets[irq];
    if (target.handle == 0) {
        return; /* nothing bound */
    }
    uint64_t word = irq;
    uint8_t msg[24];
    size_t msg_len = capnp_build_flat(msg, sizeof(msg), &word, 1);
    g_kernel_invoke(target, 0, msg, msg_len, NULL, 0, NULL);
}

__attribute__((interrupt)) static void isr_irq0(struct interrupt_frame *frame)
{
    (void)frame;
    g_tick_count++;
    if (g_tick_count % 100 == 0) {
        kprintf("[coreinit]   irq0 (timer): tick %u\n", (unsigned)g_tick_count);
    }
    dispatch_irq(0);
    outb(0x20, 0x20); /* EOI to master PIC */
}

__attribute__((interrupt)) static void isr_irq1(struct interrupt_frame *frame)
{
    (void)frame;
    (void)inb(0x60); /* must read the scancode to let the controller continue */
    dispatch_irq(1);
    outb(0x20, 0x20);
}

void irq_setup_idt_only(void)
{
    for (int i = 0; i < IDT_ENTRIES; i++) {
        idt_set_gate(i, 0, 0, 0);
    }
    load_idt(sizeof(g_idt) - 1, (uint64_t)(uintptr_t)g_idt);
    setup_tss_and_ist();
    kprintf("[coreinit]   irq: IDT loaded (no legacy PIC/PIT touched -- "
            "this core doesn't need it, see lapic_timer)\n");
}

void irq_init(int (*kernel_invoke_fn)(capability_t, uint32_t, const void *,
                                      size_t, void *, size_t, size_t *))
{
    init_irq_manager_schema();

    g_kernel_invoke = kernel_invoke_fn;
    for (int i = 0; i < 16; i++) {
        g_irq_targets[i].handle = 0;
    }

    for (int i = 0; i < IDT_ENTRIES; i++) {
        idt_set_gate(i, 0, 0, 0);
    }
    idt_set_gate(0x20, (void *)isr_irq0, KERNEL_CODE_SELECTOR, 0x8E);
    idt_set_gate(0x21, (void *)isr_irq1, KERNEL_CODE_SELECTOR, 0x8E);

    load_idt(sizeof(g_idt) - 1, (uint64_t)(uintptr_t)g_idt);
    setup_tss_and_ist();

    pic_remap();
    pit_init(100); /* 100 Hz: frequent enough to demonstrate quickly,
                    * slow enough not to flood the log */

    /* pic_remap() itself now masks everything (see that function's own
     * comment for why) -- this caller's own gates for IRQ0/IRQ1 are
     * already installed above, so unmasking them here, and only here,
     * is safe: delivery can't land on an empty gate the way it could
     * before this was split into two explicit steps. */
    outb(0x21, 0xFC); /* unmask IRQ0 (timer) and IRQ1 (keyboard) only */

    enable_interrupts();
    kprintf("[coreinit] irq_init: IDT loaded, PIC remapped, PIT at 100Hz, "
            "interrupts enabled\n");
}

static const struct_schema_t g_irq_bind_schema = {
    .data_words = 2, .pointer_count = 0, .pointers = NULL};
static method_schema_t g_irq_manager_methods[1];
object_schema_t g_irq_manager_schema;

static void init_irq_manager_schema(void)
{
    g_irq_manager_methods[0].method_id = 0;
    g_irq_manager_methods[0].param_schema = &g_irq_bind_schema;
    g_irq_manager_methods[0].result_schema = NULL;
    g_irq_manager_schema.methods = g_irq_manager_methods;
    g_irq_manager_schema.method_count = 1;
}

void irq_manager_handler(void *data, uint32_t method_id, const void *msg,
                         size_t len, void *response, size_t response_cap,
                         size_t *out_response_len)
{
    (void)data;
    (void)method_id;
    (void)len;
    (void)response;
    (void)response_cap;
    *out_response_len = 0;

    const uint64_t *words = capnp_read_flat(msg);
    uint64_t irq = words[0];
    capability_t target;
    target.handle = words[1];

    if (irq >= 16) {
        kprintf("[coreinit]   irq_manager: bind refused, IRQ %u out of "
                "range\n",
                (unsigned)irq);
        return;
    }
    g_irq_targets[irq] = target;
    kprintf("[coreinit]   irq_manager: bound IRQ %u to capability handle "
            "%u\n",
            (unsigned)irq, (unsigned)target.handle);
}

uint64_t irq_get_tick_count(void)
{
    return g_tick_count;
}

void irq_mask_irq0(void)
{
    uint8_t mask = inb(0x21);
    outb(0x21, mask | 0x01);
    kprintf("[coreinit]   irq0 masked at the PIC -- PIT is no longer this "
            "core's tick source\n");
}

void irq_install_gate(int vector, void *handler_addr)
{
    idt_set_gate(vector, handler_addr, KERNEL_CODE_SELECTOR, 0x8E);
}
