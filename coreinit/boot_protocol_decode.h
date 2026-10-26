#pragma once

/* Thin, coreinit-specific accessors over capnp_list.h's generic reader,
 * fixed to the InitDescription shape include/boot_protocol.h defines. Not vendored
 * anywhere else -- otos-grub/otos-boot are ENCODERS of these objects
 * (they need capnp_list.h's builder and boot_protocol.h's struct
 * shapes, not this), coreinit is the one DECODER.
 *
 * Nothing in coreinit calls these yet: saski doesn't produce a real
 * InitDescription object until a loader implementing
 * this protocol exists (see otos-grub/otos-boot's own READMEs). Written
 * now so the decode side of the contract is defined and tested
 * (tools/test-boot-protocol.c) alongside the encode side, not
 * invented later against whatever the first loader happens to produce. */

#include "../include/boot_protocol.h"
#include "../include/capnp_list.h"

/* Decodes an InitDescription message. Returns 1 on success (with
 * out_data, out_variants, and out_variant_count set), 0 on any
 * structural mismatch. */
static inline int
boot_protocol_read_init_description(const void *msg, size_t msg_len,
                                    const struct init_description_data **out_data,
                                    const struct init_variant **out_variants,
                                    uint32_t *out_variant_count)
{
    const uint64_t *elements = NULL;
    const uint64_t *data =
        capnp_read_list_root(msg, msg_len, BOOT_PROTO_ID_DATA_WORDS,
                             BOOT_PROTO_INIT_VARIANT_WORDS, &elements,
                             out_variant_count);
    if (data == NULL || data[0] != INIT_DESCRIPTION_MAGIC) {
        *out_data = NULL;
        *out_variants = NULL;
        *out_variant_count = 0;
        return 0;
    }
    *out_data = (const struct init_description_data *)data;
    *out_variants = (const struct init_variant *)elements;
    return 1;
}
