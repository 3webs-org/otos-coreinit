/* Trusted securinit wrapper owns the x86 ring-3 page grant, IRET entry,
 * INT80 override and INT3 exit. coreinit installs its hardware gates and
 * routes CPL3 page faults to this callback; saski remains generic IPC. */
#include "x86_wrapper.h"
#include "user_ipc.h"
#include "capnp_build.h"
#include "generated_schemas.h"
#include <stdint.h>

#define PAGE_SIZE 4096ull
#define REGION_SIZE 0x200000ull
#define ADDR_MASK 0x000FFFFFFFFFF000ull
#define PAGE_PRESENT 0x1ull
#define PAGE_WRITE 0x2ull
#define PAGE_USER 0x4ull
#define PAGE_HUGE 0x80ull

static uint64_t g_user_pt[512] __attribute__((aligned(4096)));
static uint8_t g_ring0_stack[16384] __attribute__((aligned(16)));
static uint64_t g_user_base, g_user_size, g_forbidden;
static uint64_t g_denied_access, g_ipc_denied, g_ipc_forwarded, g_exits;
static uint64_t g_resume_rsp, g_resume_pc, g_saved_flags;
static capability_t g_granted_gate;
static wrapper_invoke_fn_t g_priv_invoke;
static uint8_t g_request[WRAPPER_IPC_MAX_BYTES], g_response[WRAPPER_IPC_MAX_BYTES];

extern const uint8_t securinit_x86_probe_start[] __attribute__((visibility("hidden")));
extern const uint8_t securinit_x86_probe_end[] __attribute__((visibility("hidden")));
extern uint64_t securinit_x86_run_user(uint64_t entry, uint64_t user_stack,
                                        uint64_t forbidden, uint64_t result_addr,
                                        uint64_t denied_call, uint64_t granted_call);
extern void securinit_x86_int80(void) __attribute__((visibility("hidden")));
extern void securinit_x86_int3(void) __attribute__((visibility("hidden")));

void securinit_x86_save_resume(uint64_t rsp, uint64_t pc)
{
    g_resume_rsp = rsp;
    g_resume_pc = pc;
    __asm__ volatile("pushfq\n\tpopq %0" : "=r"(g_saved_flags));
}

uint64_t securinit_x86_resume_rsp(void) { return g_resume_rsp; }
uint64_t securinit_x86_resume_pc(void) { return g_resume_pc; }
uint64_t securinit_x86_resume_flags(void) { return g_saved_flags; }

int securinit_x86_fault_hook(uint64_t *frame, uint64_t error, uint64_t cr2)
{
    if ((frame[1] & 3u) != 3 || !(error & 4u) || !(error & 1u) ||
        cr2 != g_forbidden || frame[0] != g_user_base + PAGE_SIZE) {
        return 0;
    }
    g_denied_access++;
    frame[0] += 3; /* x86 movq (%rsi), %rbx is exactly three bytes. */
    return 1;
}

/* Frame layout from x86_wrapper_x86_64.S: r15..r8 at [0..7], rdi at
 * [8], rax at [14], then hardware RIP/CS/RFLAGS/RSP/SS at [15..19]. */
int securinit_x86_dispatch(uint64_t *frame, uint64_t vector)
{
    if ((frame[16] & 3u) != 3 || frame[15] < g_user_base ||
        frame[15] >= g_user_base + g_user_size) {
        return 0;
    }
    if (vector == 3) {
        g_exits++;
        return 1; /* assembly jumps back to the saved ring-0 stack */
    }
    if (vector == 0x80) {
        int64_t status = wrapper_dispatch_guest_call(
            g_user_base, g_user_size, g_granted_gate, g_priv_invoke,
            frame[8], g_request, g_response); /* guest RDI */
        frame[14] = (uint64_t)status; /* restored guest RAX */
        if (status == WRAPPER_IPC_OK) g_ipc_forwarded++;
        else g_ipc_denied++;
    }
    return 0; /* iretq back to the CPL3 probe */
}

static void flush_page_tables(uint64_t root)
{
    __asm__ volatile("mov %0, %%cr3" : : "r"(root) : "memory");
}

int securinit_x86_isolation_probe(const struct securinit_boot_info *info,
                                  uint64_t privileged_addr,
                                  capability_t granted_gate)
{
    uint64_t base = info->user_region_base;
    if (!info->x86_pml4_phys || !info->exception_router.handle ||
        info->user_region_size != REGION_SIZE ||
        (base & (REGION_SIZE - 1)) || base + REGION_SIZE > 0x4000000ull ||
        (privileged_addr >= base && privileged_addr < base + REGION_SIZE)) {
        return 0;
    }
    uint64_t *pml4 = (uint64_t *)(uintptr_t)info->x86_pml4_phys;
    uint64_t old_pml4 = pml4[0];
    if (!(old_pml4 & PAGE_PRESENT)) return 0;
    uint64_t *pdpt = (uint64_t *)(uintptr_t)(old_pml4 & ADDR_MASK);
    uint64_t old_pdpt = pdpt[0];
    if (!(old_pdpt & PAGE_PRESENT)) return 0;
    uint64_t *pd = (uint64_t *)(uintptr_t)(old_pdpt & ADDR_MASK);
    uint64_t index = (base >> 21) & 511u;
    uint64_t old_pde = pd[index];
    if (!(old_pde & PAGE_PRESENT) || !(old_pde & PAGE_HUGE)) return 0;
    for (unsigned i = 0; i < 512; i++) {
        if (i != index && (pd[i] & PAGE_USER)) return 0;
        g_user_pt[i] = (base + (uint64_t)i * PAGE_SIZE) |
                       PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
    }
    size_t code_len = (size_t)(securinit_x86_probe_end - securinit_x86_probe_start);
    if (!code_len || code_len > PAGE_SIZE) return 0;
    uint64_t entry = base + PAGE_SIZE;
    for (size_t i = 0; i < code_len; i++) {
        ((uint8_t *)(uintptr_t)entry)[i] = securinit_x86_probe_start[i];
    }
    g_user_base = base;
    g_user_size = REGION_SIZE;
    g_forbidden = privileged_addr;
    g_granted_gate = granted_gate;
    g_priv_invoke = info->kernel_invoke;
    g_denied_access = g_ipc_denied = g_ipc_forwarded = g_exits = 0;
    uint64_t denied_addr = base + 0x2000, granted_addr = base + 0x2080;
    uint64_t result_addr = base + 0x4000;
    uint64_t marker = SECURINIT_RING3_IPC_MARKER;
    size_t user_message_len = capnp_build_flat((void *)(uintptr_t)(base + 0x3000),
                                                24, &marker, 1);
    wrapper_ipc_call_t call = {
        .local_endpoint = WRAPPER_GATE_ENDPOINT + 1,
        .version = WRAPPER_IPC_VERSION,
        .method_id = GATE_METHOD_INVOKE,
        .request_ptr = base + 0x3000,
        .request_len = user_message_len,
        .response_ptr = base + 0x3100,
        .response_capacity = 24,
    };
    for (size_t i = 0; i < sizeof(call); i++) {
        ((uint8_t *)(uintptr_t)denied_addr)[i] = ((uint8_t *)&call)[i];
    }
    call.local_endpoint = WRAPPER_GATE_ENDPOINT;
    for (size_t i = 0; i < sizeof(call); i++) {
        ((uint8_t *)(uintptr_t)granted_addr)[i] = ((uint8_t *)&call)[i];
    }
    ((uint64_t *)(uintptr_t)result_addr)[0] = UINT64_MAX;
    ((uint64_t *)(uintptr_t)result_addr)[1] = UINT64_MAX;

    pml4[0] = old_pml4 | PAGE_USER;
    pdpt[0] = old_pdpt | PAGE_USER;
    pd[index] = (uint64_t)(uintptr_t)g_user_pt |
                PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
    flush_page_tables(info->x86_pml4_phys);

    /* Each address must be calculated from THIS slid Mach-O image. A
     * constant pointer array would bake the zero-based link addresses
     * into __const; the stage loader does not apply dyld fixups. */
    uint64_t bind[4];
    volatile uint64_t *local = bind;
    local[0] = (uint64_t)(uintptr_t)securinit_x86_int80;
    local[1] = (uint64_t)(uintptr_t)securinit_x86_int3;
    local[2] = (uint64_t)(uintptr_t)(g_ring0_stack + sizeof(g_ring0_stack));
    local[3] = (uint64_t)(uintptr_t)securinit_x86_fault_hook;
    uint8_t request[48], response[24];
    size_t request_len = capnp_build_flat(request, sizeof(request), bind, 4);
    size_t response_len = 0;
    if (!info->kernel_invoke(info->exception_router,
                             info->exception_router_bind_method,
                             request, request_len, response, sizeof(response),
                             &response_len) || response_len < 24 ||
        capnp_read_flat(response)[0] != 1) {
        pd[index] = old_pde;
        pdpt[0] = old_pdpt;
        pml4[0] = old_pml4;
        flush_page_tables(info->x86_pml4_phys);
        return 0;
    }
    uint64_t result = securinit_x86_run_user(entry, base + REGION_SIZE - 16,
                                              privileged_addr, result_addr,
                                              denied_addr, granted_addr);
    pd[index] = old_pde;
    pdpt[0] = old_pdpt;
    pml4[0] = old_pml4;
    flush_page_tables(info->x86_pml4_phys);
    const uint64_t *guest_results = (const uint64_t *)(uintptr_t)result_addr;
    const wrapper_ipc_call_t *granted =
        (const wrapper_ipc_call_t *)(uintptr_t)granted_addr;
    uint64_t forwarded = capnp_read_flat((const void *)(uintptr_t)(base + 0x3100))[0];
    return result == 0 && g_denied_access == 1 && g_exits == 1 &&
           g_ipc_denied == 1 && g_ipc_forwarded == 1 &&
           guest_results[0] == (uint64_t)(int64_t)WRAPPER_IPC_BAD_ENDPOINT &&
           guest_results[1] == WRAPPER_IPC_OK &&
           granted->response_len == 24 && forwarded == 1;
}
