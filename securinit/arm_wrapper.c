/* Wrapper-owned EL0 boundary: coreinit's EL1 vector only saves the frame
 * and routes exceptions here. This trusted callback validates/copies guest
 * Cap'n Proto bytes and invokes an explicitly granted Gate capability;
 * saski owns no SVC backend or kernel capability namespace. */
#include "arm_wrapper.h"
#include "user_ipc.h"
#include "capnp_build.h"
#include "generated_schemas.h"
#include <stdint.h>

#define PAGE_SIZE 4096ull
#define REGION_SIZE 0x200000ull
#define RAM_BASE 0x40000000ull
#define ADDR_MASK 0x0000FFFFFFFFF000ull
#define TABLE_DESC 0x3ull
#define PAGE_DESC 0x3ull
#define NORMAL (1ull << 2)
#define EL0_RW (1ull << 6)
#define INNER_SHAREABLE (3ull << 8)
#define ACCESS_FLAG (1ull << 10)
#define PXN (1ull << 53)

static uint64_t g_user_l2[512] __attribute__((aligned(4096)));
static uint64_t g_user_l3[512] __attribute__((aligned(4096)));
static uint64_t g_user_base, g_user_size, g_forbidden_addr;
static uint64_t g_resume_pc, g_saved_daif, g_denied_access, g_exits;
static uint64_t g_ipc_denied, g_ipc_forwarded;
static capability_t g_granted_gate;
static int (*g_priv_invoke)(capability_t, uint32_t, const void *, size_t,
                            void *, size_t, size_t *);
static uint8_t g_request[WRAPPER_IPC_MAX_BYTES], g_response[WRAPPER_IPC_MAX_BYTES];

extern const uint8_t securinit_arm_probe_start[], securinit_arm_probe_end[];
extern uint64_t securinit_arm_run_user(uint64_t entry, uint64_t user_stack,
                                        uint64_t forbidden, uint64_t *resume_pc,
                                        uint64_t *saved_daif, uint64_t result_words,
                                        uint64_t rejected_call, uint64_t granted_call);

static void flush_translations(void)
{
    __asm__ volatile("dsb ishst\n\ttlbi vmalle1is\n\tdsb ish\n\tisb"
                     ::: "memory");
}

static void sync_executable(uint64_t start, uint64_t end)
{
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    uint64_t dline = 4ull << ((ctr >> 16) & 0xfu);
    uint64_t iline = 4ull << (ctr & 0xfu);
    for (uint64_t p = start & ~(dline - 1); p < end; p += dline) {
        __asm__ volatile("dc cvau, %0" :: "r"(p) : "memory");
    }
    __asm__ volatile("dsb ish" ::: "memory");
    for (uint64_t p = start & ~(iline - 1); p < end; p += iline) {
        __asm__ volatile("ic ivau, %0" :: "r"(p) : "memory");
    }
    __asm__ volatile("dsb ish\n\tisb" ::: "memory");
}

static int within_user(uint64_t addr, uint64_t size)
{
    return addr >= g_user_base && size <= g_user_size &&
           addr - g_user_base <= g_user_size - size;
}

static int64_t dispatch_guest_call(uint64_t descriptor_addr)
{
    if (!within_user(descriptor_addr, sizeof(wrapper_ipc_call_t))) {
        g_ipc_denied++;
        return WRAPPER_IPC_BAD_DESCRIPTOR;
    }
    wrapper_ipc_call_t call;
    const uint8_t *src = (const uint8_t *)(uintptr_t)descriptor_addr;
    uint8_t *dst = (uint8_t *)&call;
    for (size_t i = 0; i < sizeof(call); i++) dst[i] = src[i];
    if (call.version != WRAPPER_IPC_VERSION) {
        g_ipc_denied++;
        return WRAPPER_IPC_BAD_DESCRIPTOR;
    }
    if (call.local_endpoint != WRAPPER_GATE_ENDPOINT ||
        call.method_id != GATE_METHOD_INVOKE) {
        g_ipc_denied++;
        return WRAPPER_IPC_BAD_ENDPOINT;
    }
    if (call.request_len > WRAPPER_IPC_MAX_BYTES ||
        call.response_capacity > WRAPPER_IPC_MAX_BYTES ||
        !within_user(call.request_ptr, call.request_len) ||
        !within_user(call.response_ptr, call.response_capacity)) {
        g_ipc_denied++;
        return WRAPPER_IPC_BOUNDS;
    }
    src = (const uint8_t *)(uintptr_t)call.request_ptr;
    for (uint64_t i = 0; i < call.request_len; i++) g_request[i] = src[i];
    size_t response_len = 0;
    if (!g_priv_invoke(g_granted_gate, call.method_id,
                       g_request, (size_t)call.request_len,
                       g_response, (size_t)call.response_capacity,
                       &response_len) || response_len > call.response_capacity) {
        g_ipc_denied++;
        return WRAPPER_IPC_REJECTED;
    }
    dst = (uint8_t *)(uintptr_t)call.response_ptr;
    for (size_t i = 0; i < response_len; i++) dst[i] = g_response[i];
    call.response_len = response_len;
    dst = (uint8_t *)(uintptr_t)(descriptor_addr + offsetof(wrapper_ipc_call_t, response_len));
    src = (const uint8_t *)&call.response_len;
    for (unsigned i = 0; i < 8; i++) dst[i] = src[i];
    g_ipc_forwarded++;
    return WRAPPER_IPC_OK;
}

static int wrapper_el0_sync(uint64_t *frame, uint64_t esr, uint64_t far)
{
    if ((frame[32] & 0xfu) != 0 || frame[31] < g_user_base ||
        frame[31] >= g_user_base + g_user_size) {
        return 0;
    }
    uint64_t ec = (esr >> 26) & 0x3fu;
    uint64_t fault = esr & 0x3fu;
    if (ec == 0x24 && far == g_forbidden_addr &&
        fault >= 12 && fault <= 15) {
        g_denied_access++;
        frame[31] += 4; /* Skip exactly the forbidden load. */
        frame[2] = 0;
        return 1;
    }
    if (ec == 0x15 && (esr & 0xffffu) == 0) { /* wrapper's only SVC */
        frame[0] = (uint64_t)dispatch_guest_call(frame[0]);
        return 1;
    }
    if (ec == 0x3c) { /* BRK terminates this one-shot wrapped probe. */
        g_exits++;
        frame[31] = g_resume_pc;
        frame[32] = 0x5ull | (g_saved_daif & 0x3c0ull); /* EL1h */
        frame[0] = 0;
        return 1;
    }
    return 0;
}

int securinit_arm_isolation_probe(const struct securinit_boot_info *info,
                                  uint64_t privileged_addr,
                                  capability_t granted_gate)
{
    uint64_t base = info->user_region_base;
    if (!info->exception_router.handle || !info->ttbr0_root_phys ||
        info->user_region_size != REGION_SIZE ||
        (base & (REGION_SIZE - 1)) || base < RAM_BASE ||
        base + REGION_SIZE > 0x80000000ull ||
        (privileged_addr >= base && privileged_addr < base + REGION_SIZE)) {
        return 0;
    }
    uint64_t *l1 = (uint64_t *)(uintptr_t)info->ttbr0_root_phys;
    uint64_t old_l1 = l1[1];
    if ((old_l1 & 3u) != TABLE_DESC) {
        return 0; /* BSP pagefault demo has not installed the RAM L2. */
    }
    const uint64_t *old_l2 = (const uint64_t *)(uintptr_t)(old_l1 & ADDR_MASK);
    for (unsigned i = 0; i < 512; i++) {
        g_user_l2[i] = old_l2[i];
    }
    for (unsigned i = 0; i < 512; i++) {
        g_user_l3[i] = (base + (uint64_t)i * PAGE_SIZE) |
                       PAGE_DESC | NORMAL | EL0_RW | INNER_SHAREABLE |
                       ACCESS_FLAG | PXN;
    }
    g_user_l2[(base >> 21) & 511u] =
        (uint64_t)(uintptr_t)g_user_l3 | TABLE_DESC;

    uint64_t entry = base + PAGE_SIZE;
    size_t bytes = (size_t)(securinit_arm_probe_end - securinit_arm_probe_start);
    if (bytes == 0 || bytes > PAGE_SIZE) {
        return 0;
    }
    for (size_t i = 0; i < bytes; i++) {
        ((uint8_t *)(uintptr_t)entry)[i] = securinit_arm_probe_start[i];
    }
    g_user_base = base;
    g_user_size = REGION_SIZE;
    g_forbidden_addr = privileged_addr;
    g_granted_gate = granted_gate;
    g_priv_invoke = info->kernel_invoke;
    g_resume_pc = g_saved_daif = g_denied_access = g_exits = 0;
    g_ipc_denied = g_ipc_forwarded = 0;

    const uint64_t rejected_addr = base + 0x2000;
    const uint64_t bad_pointer_addr = base + 0x2040;
    const uint64_t granted_addr = base + 0x2080;
    const uint64_t request_addr = base + 0x3000;
    const uint64_t response_addr = base + 0x3100;
    const uint64_t result_addr = base + 0x4000;
    uint64_t marker = SECURINIT_EL0_IPC_MARKER;
    size_t request_size = capnp_build_flat((void *)(uintptr_t)request_addr,
                                            24, &marker, 1);
    wrapper_ipc_call_t call = {
        .local_endpoint = WRAPPER_GATE_ENDPOINT,
        .version = WRAPPER_IPC_VERSION,
        .method_id = GATE_METHOD_INVOKE,
        .request_ptr = request_addr,
        .request_len = request_size,
        .response_ptr = response_addr,
        .response_capacity = 24,
    };
    const uint8_t *call_bytes = (const uint8_t *)&call;
    for (size_t i = 0; i < sizeof(call); i++) {
        ((uint8_t *)(uintptr_t)granted_addr)[i] = call_bytes[i];
    }
    call.request_ptr = privileged_addr; /* must fail BEFORE reading Admin */
    call_bytes = (const uint8_t *)&call;
    for (size_t i = 0; i < sizeof(call); i++) {
        ((uint8_t *)(uintptr_t)bad_pointer_addr)[i] = call_bytes[i];
    }
    call.request_ptr = request_addr;
    call.local_endpoint = WRAPPER_GATE_ENDPOINT + 1; /* not granted */
    call_bytes = (const uint8_t *)&call;
    for (size_t i = 0; i < sizeof(call); i++) {
        ((uint8_t *)(uintptr_t)rejected_addr)[i] = call_bytes[i];
    }
    ((uint64_t *)(uintptr_t)result_addr)[0] = UINT64_MAX;
    ((uint64_t *)(uintptr_t)result_addr)[1] = UINT64_MAX;
    ((uint64_t *)(uintptr_t)result_addr)[2] = UINT64_MAX;
    l1[1] = (uint64_t)(uintptr_t)g_user_l2 | TABLE_DESC;
    flush_translations();
    sync_executable(entry, entry + bytes);

    uint64_t callback = (uint64_t)(uintptr_t)wrapper_el0_sync;
    uint8_t request[24], response[24];
    size_t request_len = capnp_build_flat(request, sizeof(request), &callback, 1);
    size_t response_len = 0;
    if (!info->kernel_invoke(info->exception_router,
                             info->exception_router_bind_method,
                             request, request_len, response, sizeof(response),
                             &response_len) || response_len < 24 ||
        capnp_read_flat(response)[0] != 1) {
        l1[1] = old_l1;
        flush_translations();
        return 0;
    }

    uint64_t result = securinit_arm_run_user(entry, base + REGION_SIZE - 16,
                                              privileged_addr, &g_resume_pc,
                                              &g_saved_daif, result_addr,
                                              rejected_addr, granted_addr);
    l1[1] = old_l1;
    flush_translations();
    const uint64_t *guest_results = (const uint64_t *)(uintptr_t)result_addr;
    const wrapper_ipc_call_t *granted =
        (const wrapper_ipc_call_t *)(uintptr_t)granted_addr;
    uint64_t forwarded = capnp_read_flat((const void *)(uintptr_t)response_addr)[0];
    return result == 0 && g_denied_access == 1 && g_exits == 1 &&
           g_ipc_denied == 2 && g_ipc_forwarded == 1 &&
           guest_results[0] == (uint64_t)(int64_t)WRAPPER_IPC_BAD_ENDPOINT &&
           guest_results[1] == (uint64_t)(int64_t)WRAPPER_IPC_BOUNDS &&
           guest_results[2] == WRAPPER_IPC_OK &&
           granted->response_len == 24 && forwarded == 1;
}
