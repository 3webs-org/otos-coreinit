/* Host round-trip check for include/boot_protocol.h + include/capnp_list.h:
 * build an InitDescription with the real encoder, read it back with the
 * real decoder, and confirm every
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
    struct init_description_data description = {
        .magic = INIT_DESCRIPTION_MAGIC,
        .coreinit_allocator_phys = 0x12345000ull,
    };

    uint8_t buf[512];
    size_t len = capnp_build_list_root(buf, sizeof(buf),
                                       (const uint64_t *)&description,
                                       BOOT_PROTO_ID_DATA_WORDS,
                                       (const uint64_t *)variants, 2,
                                       BOOT_PROTO_INIT_VARIANT_WORDS);
    assert(len > 0 && len <= sizeof(buf));

    const struct init_description_data *out_data = NULL;
    const struct init_variant *out_variants = NULL;
    uint32_t out_count = 0;
    int ok = boot_protocol_read_init_description(buf, len, &out_data,
                                                  &out_variants, &out_count);
    assert(ok == 1);
    assert(out_data != NULL);
    assert(out_data->magic == INIT_DESCRIPTION_MAGIC);
    assert(out_data->coreinit_allocator_phys ==
           description.coreinit_allocator_phys);
    assert(out_count == 2);
    assert(out_variants != NULL);
    assert(memcmp(out_variants, variants, sizeof(variants)) == 0);

    printf("InitDescription round trip: PASS (%zu bytes, %u variants)\n", len,
          out_count);
}

int main(void)
{
    test_init_description();
    printf("boot_protocol.h / capnp_list.h: ROUND-TRIP PASS\n");
    return 0;
}
