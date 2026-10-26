#pragma once

#include <stddef.h>
#include <stdint.h>

/* A standalone encoder/reader for exactly one shape: a single-segment
 * Cap'n Proto message whose root struct has N plain data words followed
 * by exactly one pointer field, a composite list of fixed-shape
 * structs with data words only (no per-element pointers). This is the
 * shape boot_protocol.h's InitDescription needs -- a list of init variants --
 * and nothing else in this project has ever needed a real Cap'n Proto
 * list before (coreinit/capnp_build.h's capnp_build_flat is flat,
 * pointer-free, by its own explicit design).
 *
 * Deliberately self-contained, duplicating (not including) the handful
 * of bit-packing lines saski/kernel/capnp.h already implements for the
 * same wire format: this header needs to be vendored into otos-grub
 * and otos-boot, which have no access to the saski repo at all. The
 * wire format itself is the real thing (capnproto.org/encoding.html),
 * not a project-specific subset -- any real Cap'n Proto reader can
 * decode what this builds, this project just never needs one.
 *
 * The composite-list tag-word convention this relies on: a list
 * pointer with element_size==7 points at a "tag word" shaped exactly
 * like a struct pointer, except its offset field is repurposed to hold
 * the element COUNT (not a real offset), and its data/ptr word counts
 * describe every element uniformly. The list pointer's own
 * element_count field (29 bits) holds the total WORD COUNT of the list
 * body (tag word + every element), not the element count itself.
 * Verified against saski/kernel/capnp_validate.c's own real decoder,
 * which this project already ships and already correctly implements
 * this exact convention for IPC message validation (a different
 * consumption path -- kernel_invoke dispatch -- than this header's
 * direct physical-memory reads, but the identical wire format). */

/* Builds the message described above. buf/buf_cap: output. data_words/
 * n_data_words: the root struct's plain data section, written verbatim.
 * elements/n_elements: the list's elements, flattened -- n_elements *
 * elem_data_words words total, elements[0..elem_data_words) is element
 * 0, and so on. elem_data_words: how many plain data words make up ONE
 * element (2 for CoreEntry, 5 for InitVariant -- see boot_protocol.h).
 * Returns total bytes written, or 0 if buf_cap is too small. */
static inline size_t capnp_build_list_root(void *buf, size_t buf_cap,
                                           const uint64_t *data_words,
                                           uint16_t n_data_words,
                                           const uint64_t *elements,
                                           uint32_t n_elements,
                                           uint16_t elem_data_words)
{
    uint64_t element_words = (uint64_t)n_elements * elem_data_words;
    uint64_t list_body_words = 1ull + element_words; /* tag word + elements */
    uint64_t seg_words = 1ull /* root ptr */ + n_data_words +
                        1ull /* list ptr */ + list_body_words;
    size_t total = 8 /* header */ + (size_t)seg_words * 8;
    if (buf_cap < total) {
        return 0;
    }

    uint8_t *b = (uint8_t *)buf;
    b[0] = b[1] = b[2] = b[3] = 0; /* segment count - 1 = 0 */
    b[4] = (uint8_t)(seg_words & 0xFF);
    b[5] = (uint8_t)((seg_words >> 8) & 0xFF);
    b[6] = (uint8_t)((seg_words >> 16) & 0xFF);
    b[7] = (uint8_t)((seg_words >> 24) & 0xFF);

    uint64_t *w = (uint64_t *)(b + 8);
    size_t i = 0;
    /* root pointer: struct, offset 0, data_words=n_data_words, ptr_words=1 */
    w[i++] = ((uint64_t)n_data_words << 32) | (1ull << 48);
    for (uint16_t j = 0; j < n_data_words; j++) {
        w[i++] = data_words[j];
    }
    /* list pointer: tag=1 (list), offset 0 (body immediately follows),
     * element_size=7 (composite), element_count=list_body_words */
    w[i++] = 1ull | (7ull << 32) | (list_body_words << 35);
    /* tag word: struct-shaped, "offset" repurposed as element count,
     * data_words=elem_data_words, ptr_words=0 */
    w[i++] = ((uint64_t)n_elements << 2) | ((uint64_t)elem_data_words << 32);
    for (uint64_t j = 0; j < element_words; j++) {
        w[i++] = elements[j];
    }
    return total;
}

/* Reads a message built by capnp_build_list_root. Callers must already
 * know the expected shape (data_words, elem_data_words) -- same
 * "trusts the caller" posture as coreinit/capnp_build.h's own
 * capnp_read_flat, and the same reason: this is not a general-purpose
 * schema-driven decoder, it is the fixed counterpart to the fixed
 * encoder above. msg_len bounds every word access actually performed,
 * so a corrupt or undersized message is rejected (NULL) rather than
 * read out of bounds -- this path has no kernel_invoke-style
 * validation layer above it, since it never goes through kernel_invoke
 * at all.
 *
 * On success, returns a pointer to the root struct's data_words-word
 * data section (native-endian, read directly) and sets *out_elements/
 * *out_count to the list's first element and how many elements it has
 * (each element elem_data_words words wide, flattened the same way the
 * builder above takes them). Returns NULL, and sets *out_elements=NULL
 * and *out_count=0, on any structural mismatch. */
static inline const uint64_t *capnp_read_list_root(const void *msg,
                                                    size_t msg_len,
                                                    uint16_t data_words,
                                                    uint16_t elem_data_words,
                                                    const uint64_t **out_elements,
                                                    uint32_t *out_count)
{
    *out_elements = NULL;
    *out_count = 0;

    if (msg_len < 8 || ((msg_len - 8) % 8) != 0) {
        return NULL;
    }
    uint64_t seg_words_avail = (msg_len - 8) / 8;
    const uint64_t *w = (const uint64_t *)((const uint8_t *)msg + 8);

    /* root pointer: must be a struct pointer, offset 0 (this reader
     * doesn't honor any other offset -- nothing this project builds
     * ever emits one), with exactly 1 pointer field. */
    if (1u + (uint64_t)data_words >= seg_words_avail) {
        return NULL;
    }
    uint64_t root = w[0];
    if ((root & 0x3u) != 0) {
        return NULL;
    }
    uint16_t root_data_words = (uint16_t)((root >> 32) & 0xFFFFu);
    uint16_t root_ptr_words = (uint16_t)((root >> 48) & 0xFFFFu);
    if (root_data_words != data_words || root_ptr_words < 1) {
        return NULL;
    }

    const uint64_t *data = &w[1];
    uint64_t list_ptr_index = 1 + data_words;
    uint64_t list_ptr = w[list_ptr_index];
    if ((list_ptr & 0x3u) != 1) {
        return NULL; /* not a list pointer */
    }
    uint8_t element_size = (uint8_t)((list_ptr >> 32) & 0x7u);
    if (element_size != 7) {
        return NULL; /* not composite */
    }
    uint64_t list_body_words = (list_ptr >> 35) & 0x1FFFFFFFull;

    uint64_t tag_index = list_ptr_index + 1;
    if (tag_index >= seg_words_avail ||
        list_body_words > seg_words_avail - tag_index) {
        return NULL;
    }
    uint64_t tag = w[tag_index];
    if ((tag & 0x3u) != 0) {
        return NULL; /* tag word isn't struct-shaped */
    }
    uint32_t count = (uint32_t)((tag >> 2) & 0x3FFFFFFFu);
    uint16_t tag_data_words = (uint16_t)((tag >> 32) & 0xFFFFu);
    uint16_t tag_ptr_words = (uint16_t)((tag >> 48) & 0xFFFFu);
    if (tag_data_words != elem_data_words || tag_ptr_words != 0) {
        return NULL;
    }
    /* list_body_words must actually hold the tag word plus every
     * element -- reject rather than trust a short declaration (same
     * overflow-safe check capnp_validate.c itself uses). */
    if (elem_data_words != 0 &&
        (uint64_t)count > (list_body_words - 1) / elem_data_words) {
        return NULL;
    }
    if (1ull + (uint64_t)count * elem_data_words > list_body_words) {
        return NULL;
    }

    *out_elements = &w[tag_index + 1];
    *out_count = count;
    return data;
}
