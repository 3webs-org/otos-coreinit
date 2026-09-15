#pragma once

#include <stddef.h>
#include <stdint.h>

/* Builds a single-segment Cap'n Proto message whose root is a struct
 * with n_words of plain data and zero pointers -- exactly the shape
 * every message in this demo needs (the factory object's own CREATE/DESTROY
 * protocol, and every object built on top of it so far, are all flat
 * data, no pointers). Layout: [u32 segCount-1=0][u32 seg0Words][root
 * struct pointer][n_words data words]. Returns total bytes written, or
 * 0 if buf_cap is too small.
 *
 * This is a convenience for a narrow, common case, not a general
 * Cap'n Proto encoder -- an object needing pointers (lists, text,
 * nested structs) in its own protocol would need to build those by
 * hand, following the same encoding capnp_validate.c decodes. */
static inline size_t capnp_build_flat(void *buf, size_t buf_cap,
                                      const uint64_t *data_words,
                                      uint16_t n_words)
{
    size_t total = 8 /* header */ + 8 /* root ptr */ + (size_t)n_words * 8;
    if (buf_cap < total) {
        return 0;
    }

    uint8_t *b = (uint8_t *)buf;
    /* segment count - 1 = 0 */
    b[0] = 0;
    b[1] = 0;
    b[2] = 0;
    b[3] = 0;
    /* segment 0 size in words: 1 (root ptr) + n_words */
    uint32_t seg_words = 1u + n_words;
    b[4] = (uint8_t)(seg_words & 0xFF);
    b[5] = (uint8_t)((seg_words >> 8) & 0xFF);
    b[6] = (uint8_t)((seg_words >> 16) & 0xFF);
    b[7] = (uint8_t)((seg_words >> 24) & 0xFF);

    /* root pointer: struct, offset 0, dataWords=n_words, ptrWords=0 */
    uint64_t root_word = ((uint64_t)n_words) << 32;
    uint64_t *words = (uint64_t *)(b + 8);
    words[0] = root_word;
    for (uint16_t i = 0; i < n_words; i++) {
        words[1 + i] = data_words[i];
    }

    return total;
}

/* Reads the data section of a flat message built the way
 * capnp_build_flat lays one out -- valid to call on any message that
 * has already passed kernel_invoke's own structural validation for a
 * flat-data schema, since that's exactly the layout this assumes. Not a
 * general-purpose reader: it does not itself re-derive the struct's
 * shape from the wire bytes, it trusts the caller to already know it. */
static inline const uint64_t *capnp_read_flat(const void *msg)
{
    const uint8_t *b = (const uint8_t *)msg;
    return (const uint64_t *)(b + 16); /* skip 8-byte header + 8-byte root ptr */
}
