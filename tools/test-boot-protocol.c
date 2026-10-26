/* Host round-trip check for include/boot_protocol.h + include/capnp_list.h:
 * build a MachineDescription and an InitDescription with the real
 * encoder, read them back with the real decoder, and confirm every
 * scalar field and every list element survives the round trip exactly.
 * Also exercises capnp_read_list_root's own rejection paths (wrong
 * shape, truncated buffer) -- this path has no kernel_invoke-style
 * validation layer above it, so this is the only place those paths get
 * exercised at all.
 *
 * Run on the build host, not inside either target image:
 *   cc -std=c11 -I. tools/test-boot-protocol.c -o /tmp/test-boot-protocol
 *   /tmp/test-boot-protocol
 */
#include "../include/boot_protocol.h"
#include "../include/capnp_list.h"
#include "../coreinit/boot_protocol_decode.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_machine_description(void)
{
    struct machine_description_data md = {
        .magic = MACHINE_DESCRIPTION_MAGIC,
        .arch_tag = BOOT_PROTO_ARCH_AARCH64,
        .ram_size = 0x20000000ull,
        .free_memory_base = 0x54000000ull,
        .free_memory_len = 0x04000000ull,
        .fdt_blob_phys = 0x40000000ull,
        .early_rsdp_phys = 0,
        .uart_base = 0x9000000ull,
        .lapic_base = 0,
        .gicd_base = 0x8000000ull,
        .gicr_base = 0x80a0000ull,
        .gicr_size = 0xf60000ull,
        .gic_cpu_base = 0,
        .gic_is_v2 = 0,
        .timer_freq_hz = 62500000ull,
        .psci_use_hvc = 0,
    };
    struct core_entry cores[3] = {
        {.target = 0, .enabled = 1},
        {.target = 1, .enabled = 1},
        {.target = 2, .enabled = 0},
    };

    uint8_t buf[512];
    size_t len = capnp_build_list_root(buf, sizeof(buf), (const uint64_t *)&md,
                                       BOOT_PROTO_MD_DATA_WORDS,
                                       (const uint64_t *)cores, 3,
                                       BOOT_PROTO_CORE_ENTRY_WORDS);
    assert(len > 0 && len <= sizeof(buf));

    const struct core_entry *out_cores = NULL;
    uint32_t out_count = 0;
    const struct machine_description_data *out_md =
        boot_protocol_read_machine_description(buf, len, &out_cores, &out_count);
    assert(out_md != NULL);
    assert(memcmp(out_md, &md, sizeof(md)) == 0);
    assert(out_count == 3);
    assert(out_cores != NULL);
    for (int i = 0; i < 3; i++) {
        assert(out_cores[i].target == cores[i].target);
        assert(out_cores[i].enabled == cores[i].enabled);
    }

    /* Truncated buffer: must be rejected, not read out of bounds. */
    const struct core_entry *bad_cores = NULL;
    uint32_t bad_count = 0;
    const struct machine_description_data *bad_md =
        boot_protocol_read_machine_description(buf, 16, &bad_cores, &bad_count);
    assert(bad_md == NULL && bad_cores == NULL && bad_count == 0);

    /* Corrupted magic: must be rejected even though the shape is fine. */
    uint8_t corrupt[512];
    memcpy(corrupt, buf, len);
    corrupt[8] ^= 0xFF; /* first byte of the data section (magic) */
    const struct core_entry *corrupt_cores = NULL;
    uint32_t corrupt_count = 0;
    const struct machine_description_data *corrupt_md =
        boot_protocol_read_machine_description(corrupt, len, &corrupt_cores,
                                               &corrupt_count);
    assert(corrupt_md == NULL && corrupt_cores == NULL && corrupt_count == 0);

    printf("MachineDescription round trip: PASS (%zu bytes, %u cores)\n", len,
          out_count);
}

static void test_init_description(void)
{
    struct init_variant variants[2] = {
        {.arch_tag = BOOT_PROTO_ARCH_AARCH64,
         .kernel_macho_phys = 0x41000000ull,
         .kernel_macho_len = 0x20000ull,
         .init_macho_phys = 0x40200000ull,
         .init_macho_len = 0x24000ull},
        {.arch_tag = BOOT_PROTO_ARCH_X86_64,
         .kernel_macho_phys = 0x1000000ull,
         .kernel_macho_len = 0x400000ull,
         .init_macho_phys = 0x1800000ull,
         .init_macho_len = 0x400000ull},
    };
    uint64_t magic_word = INIT_DESCRIPTION_MAGIC;

    uint8_t buf[512];
    size_t len = capnp_build_list_root(buf, sizeof(buf), &magic_word,
                                       BOOT_PROTO_ID_DATA_WORDS,
                                       (const uint64_t *)variants, 2,
                                       BOOT_PROTO_INIT_VARIANT_WORDS);
    assert(len > 0 && len <= sizeof(buf));

    const struct init_variant *out_variants = NULL;
    uint32_t out_count = 0;
    int ok = boot_protocol_read_init_description(buf, len, &out_variants,
                                                  &out_count);
    assert(ok == 1);
    assert(out_count == 2);
    assert(out_variants != NULL);
    assert(memcmp(out_variants, variants, sizeof(variants)) == 0);

    /* Wrong elem_data_words (reading it as a MachineDescription's core
     * list instead) must be rejected, not misinterpret the bytes. */
    const struct core_entry *wrong_shape = NULL;
    uint32_t wrong_count = 0;
    const struct machine_description_data *wrong_md =
        boot_protocol_read_machine_description(buf, len, &wrong_shape,
                                               &wrong_count);
    assert(wrong_md == NULL && wrong_shape == NULL && wrong_count == 0);

    printf("InitDescription round trip: PASS (%zu bytes, %u variants)\n", len,
          out_count);
}

int main(void)
{
    test_machine_description();
    test_init_description();
    printf("boot_protocol.h / capnp_list.h: ROUND-TRIP PASS\n");
    return 0;
}
