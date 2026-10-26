#include "fdt.h"

/* All multi-byte fields in an FDT blob are big-endian, per spec, regardless of
 * the host CPU's own endianness -- every field read here goes through an
 * explicit byte-swap; never a direct dereference. */

#define FDT_MAGIC 0xd00dfeedu

#define FDT_BEGIN_NODE 0x00000001u
#define FDT_END_NODE   0x00000002u
#define FDT_PROP       0x00000003u
#define FDT_NOP        0x00000004u
#define FDT_END        0x00000009u

/* Deep enough for any real tree this project will meet (a typical SoC DTB is
 * about 6 levels). Past this the parent search stops descending rather than
 * overflowing -- see fdt_parent_offset's own note. */
#define FDT_MAX_DEPTH 16

typedef struct {
    const uint8_t *base;
    const uint8_t *s;      /* structure block start */
    const uint8_t *send;   /* structure block end (exclusive) */
    const uint8_t *str;    /* strings block start */
    const uint8_t *strend; /* strings block end (exclusive) */
} ctx_t;

static uint32_t rd_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t rd_be64(const uint8_t *p)
{
    return ((uint64_t)rd_be32(p) << 32) | (uint64_t)rd_be32(p + 4);
}

/* Reads a `cells`-sized big-endian value out of a reg-style property. Only 1
 * and 2 are legal per spec for the trees this project targets (anything wider
 * is treated as 2 rather than silently reading fewer bytes than exist). */
static uint64_t rd_cells(const uint8_t *p, uint32_t cells)
{
    if (cells == 1) {
        return (uint64_t)rd_be32(p);
    }
    return rd_be64(p);
}

/* Bounds test. Every advance through the structure block goes through this
 * before a byte is read: on an ARM boot the blob arrives from firmware in a
 * register and is attacker-adjacent, so a truncated or hostile blob must yield
 * "not found", never an out-of-bounds read. */
static int ok(const ctx_t *c, const uint8_t *p, uint64_t n)
{
    return p >= c->s && p <= c->send && n <= (uint64_t)(c->send - p);
}

static int ctx_init(const void *blob, ctx_t *c)
{
    const uint8_t *b = (const uint8_t *)blob;

    if (!b) {
        return 0;
    }
    if (rd_be32(b) != FDT_MAGIC) {
        return 0;
    }
    uint32_t totalsize = rd_be32(b + 4);
    uint32_t off_struct = rd_be32(b + 8);
    uint32_t off_strings = rd_be32(b + 12);
    uint32_t size_strings = rd_be32(b + 32);
    uint32_t size_struct = rd_be32(b + 36);

    /* Every later walk trusts these four, so they are validated once here. */
    if (off_struct > totalsize || size_struct > totalsize - off_struct) {
        return 0;
    }
    if (off_strings > totalsize || size_strings > totalsize - off_strings) {
        return 0;
    }
    c->base = b;
    c->s = b + off_struct;
    c->send = c->s + size_struct;
    c->str = b + off_strings;
    c->strend = c->str + size_strings;
    return 1;
}

static const char *str_at(const ctx_t *c, uint32_t off)
{
    if (off >= (uint32_t)(c->strend - c->str)) {
        return 0;
    }
    return (const char *)(c->str + off);
}

static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static uint32_t align4(uint32_t v)
{
    return (v + 3u) & ~3u;
}

/* Returns a BOUNDED pointer to the NUL-terminated node name sitting at p, or
 * NULL if no terminator exists before the end of the structure block. The
 * length is written to *out_len (not counting the terminator). */
static const char *node_name(const ctx_t *c, const uint8_t *p, uint32_t *out_len)
{
    uint32_t n = 0;
    while (ok(c, p + n, 1) && p[n] != 0) {
        n++;
    }
    if (!ok(c, p + n, 1)) {
        return 0;
    }
    if (out_len) {
        *out_len = n;
    }
    return (const char *)p;
}

/* Returns the offset (relative to the blob) of the byte at p. */
static int off_of(const ctx_t *c, const uint8_t *p)
{
    return (int)(p - c->base);
}

/* Given a node's BEGIN_NODE offset, returns the offset just past its matching
 * END_NODE, or -1 if the blob is malformed before that point. */
static int skip_node(const ctx_t *c, int off)
{
    const uint8_t *p = c->base + off;
    int depth = 0;

    for (;;) {
        if (!ok(c, p, 4)) {
            return -1;
        }
        uint32_t token = rd_be32(p);
        p += 4;
        if (token == FDT_BEGIN_NODE) {
            uint32_t namelen = 0;
            if (!node_name(c, p, &namelen)) {
                return -1;
            }
            p += align4(namelen + 1);
            depth++;
        } else if (token == FDT_END_NODE) {
            depth--;
            if (depth <= 0) {
                return off_of(c, p);
            }
        } else if (token == FDT_PROP) {
            if (!ok(c, p, 8)) {
                return -1;
            }
            uint32_t len = rd_be32(p);
            if (!ok(c, p + 8, len)) {
                return -1;
            }
            p += 8 + align4(len);
        } else if (token == FDT_NOP) {
            /* no payload */
        } else if (token == FDT_END) {
            return -1;
        } else {
            return -1;
        }
    }
}

/* Positions just past a node's own name, i.e. at the first property or child. */
static const uint8_t *after_name(const ctx_t *c, int node_off)
{
    const uint8_t *p = c->base + node_off;
    uint32_t namelen = 0;

    if (!ok(c, p, 4) || rd_be32(p) != FDT_BEGIN_NODE) {
        return 0;
    }
    p += 4;
    if (!node_name(c, p, &namelen)) {
        return 0;
    }
    return p + align4(namelen + 1);
}

int fdt_valid(const void *blob, uint64_t blob_size)
{
    const uint8_t *b = (const uint8_t *)blob;
    ctx_t c;

    if (!b || blob_size < 40) {
        return 0;
    }
    if (rd_be32(b) != FDT_MAGIC) {
        return 0;
    }
    uint32_t totalsize = rd_be32(b + 4);
    if (totalsize > blob_size) {
        return 0;
    }
    /* ctx_init re-checks the block offsets against totalsize, which is what
     * makes every later walk safe. */
    return ctx_init(blob, &c);
}

int fdt_first_child(const void *blob, int parent_offset)
{
    ctx_t c;
    if (!ctx_init(blob, &c)) {
        return FDT_ERR_NODE;
    }
    const uint8_t *p = after_name(&c, parent_offset);
    if (!p) {
        return FDT_ERR_NODE;
    }
    for (;;) {
        if (!ok(&c, p, 4)) {
            return FDT_ERR_NODE;
        }
        uint32_t token = rd_be32(p);
        if (token == FDT_BEGIN_NODE) {
            return off_of(&c, p);
        }
        if (token == FDT_END_NODE) {
            return FDT_ERR_NODE;
        }
        if (token == FDT_PROP) {
            /* p still points AT the token here, so the header is token (4),
             * length (4) and name offset (4): data begins at p + 12. The
             * other walks in this file advance p past the token first and so
             * use 8; mixing the two conventions is exactly the off-by-four
             * this comment exists to prevent. */
            uint32_t len = rd_be32(p + 4);
            if (!ok(&c, p + 12, len)) {
                return FDT_ERR_NODE;
            }
            p += 12 + align4(len);
            continue;
        }
        if (token == FDT_NOP) {
            p += 4;
            continue;
        }
        return FDT_ERR_NODE;
    }
}

int fdt_next_sibling(const void *blob, int parent_offset, int prev_sibling)
{
    ctx_t c;
    if (!ctx_init(blob, &c)) {
        return FDT_ERR_NODE;
    }
    /* parent_offset is validated rather than merely accepted. An iterator that
     * silently walks off into an unrelated subtree would turn a caller's
     * bookkeeping bug into a wrong-but-plausible device address, and this is
     * exactly the kind of error that survives review because both the input
     * and the output look reasonable. */
    if (fdt_parent_offset(blob, prev_sibling) != parent_offset) {
        return FDT_ERR_NODE;
    }
    int nxt = skip_node(&c, prev_sibling);
    if (nxt < 0) {
        return FDT_ERR_NODE;
    }
    const uint8_t *p = c.base + nxt;
    for (;;) {
        if (!ok(&c, p, 4)) {
            return FDT_ERR_NODE;
        }
        uint32_t token = rd_be32(p);
        if (token == FDT_BEGIN_NODE) {
            return off_of(&c, p);
        }
        if (token == FDT_END_NODE) {
            return FDT_ERR_NODE;
        }
        if (token == FDT_NOP) {
            p += 4;
            continue;
        }
        return FDT_ERR_NODE;
    }
}

int fdt_parent_offset(const void *blob, int node_offset)
{
    ctx_t c;
    if (!ctx_init(blob, &c)) {
        return FDT_ERR_NODE;
    }
    /* Walks from the root keeping the most recently seen node at each depth;
     * when the target is reached, the node one level up is its parent. Depth is
     * capped rather than unbounded, so a pathological tree degrades to "no
     * parent found" instead of overflowing a stack. */
    int stack[FDT_MAX_DEPTH];
    const uint8_t *p = c.s;
    int depth = -1;

    for (;;) {
        if (!ok(&c, p, 4)) {
            return FDT_ERR_NODE;
        }
        int here = off_of(&c, p);
        uint32_t token = rd_be32(p);
        p += 4;

        if (token == FDT_BEGIN_NODE) {
            uint32_t namelen = 0;
            if (!node_name(&c, p, &namelen)) {
                return FDT_ERR_NODE;
            }
            p += align4(namelen + 1);
            depth++;
            if (depth < FDT_MAX_DEPTH) {
                stack[depth] = here;
            }
            if (here == node_offset) {
                if (depth <= 0 || depth - 1 >= FDT_MAX_DEPTH) {
                    return FDT_ERR_NODE;
                }
                return stack[depth - 1];
            }
        } else if (token == FDT_END_NODE) {
            depth--;
            if (depth < 0) {
                return FDT_ERR_NODE;
            }
        } else if (token == FDT_PROP) {
            uint32_t len = rd_be32(p);
            if (!ok(&c, p + 8, len)) {
                return FDT_ERR_NODE;
            }
            p += 8 + align4(len);
        } else if (token == FDT_NOP) {
            /* no payload */
        } else {
            return FDT_ERR_NODE;
        }
    }
}

const void *fdt_getprop(const void *blob, int node_offset, const char *name,
                        uint32_t *out_len)
{
    ctx_t c;
    if (!name || !ctx_init(blob, &c)) {
        return 0;
    }
    const uint8_t *p = after_name(&c, node_offset);
    if (!p) {
        return 0;
    }
    for (;;) {
        if (!ok(&c, p, 4)) {
            return 0;
        }
        uint32_t token = rd_be32(p);
        if (token == FDT_BEGIN_NODE || token == FDT_END_NODE) {
            return 0;
        }
        if (token == FDT_NOP) {
            p += 4;
            continue;
        }
        if (token != FDT_PROP || !ok(&c, p, 8)) {
            return 0;
        }
        uint32_t len = rd_be32(p + 4);
        uint32_t nameoff = rd_be32(p + 8);
        if (!ok(&c, p + 12, len)) {
            return 0;
        }
        const char *pn = str_at(&c, nameoff);
        if (!pn) {
            return 0;
        }
        if (str_eq(pn, name)) {
            if (out_len) {
                *out_len = len;
            }
            return p + 12;
        }
        p += 12 + align4(len);
    }
}

int fdt_path_offset(const void *blob, const char *path)
{
    ctx_t c;
    if (!ctx_init(blob, &c)) {
        return FDT_ERR_NODE;
    }
    if (!path || path[0] != '/') {
        return FDT_ERR_NODE;
    }
    /* The structure block's first token is the root node's BEGIN_NODE, so the
     * root's offset is the structure block's own start. */
    int cur = off_of(&c, c.s);
    if (!ok(&c, c.s, 4) || rd_be32(c.s) != FDT_BEGIN_NODE) {
        return FDT_ERR_NODE;
    }
    const char *p = path;
    while (*p == '/') {
        p++;
    }
    if (*p == 0) {
        return cur;
    }
    while (*p) {
        const char *start = p;
        while (*p && *p != '/') {
            p++;
        }
        size_t len = (size_t)(p - start);

        int found = FDT_ERR_NODE;
        for (int ch = fdt_first_child(blob, cur); ch != FDT_ERR_NODE;
             ch = fdt_next_sibling(blob, cur, ch)) {
            const uint8_t *np = c.base + ch;
            uint32_t namelen = 0;
            const char *n = node_name(&c, np + 4, &namelen);
            if (!n) {
                break;
            }
            if ((size_t)namelen == len) {
                size_t i = 0;
                while (i < len && n[i] == start[i]) {
                    i++;
                }
                if (i == len) {
                    found = ch;
                    break;
                }
            }
        }
        if (found == FDT_ERR_NODE) {
            return FDT_ERR_NODE;
        }
        cur = found;
        while (*p == '/') {
            p++;
        }
    }
    return cur;
}

/* Does a NUL-separated compatible list contain `want`? Matching only the first
 * entry is the classic DTB bug this project refuses to inherit. */
static int compatible_match(const char *list, uint32_t len, const char *want)
{
    uint32_t i = 0;
    while (i < len) {
        const char *entry = list + i;
        uint32_t j = 0;
        while (i + j < len && entry[j]) {
            j++;
        }
        if (i + j < len && str_eq(entry, want)) {
            return 1;
        }
        i += j + 1;
    }
    return 0;
}

int fdt_next_node_by_compatible_offset(const void *blob,
                                       const char *compatible,
                                       int previous_offset)
{
    ctx_t c;
    if (!compatible || !ctx_init(blob, &c)) {
        return FDT_ERR_NODE;
    }
    /* Depth-first from the root. Each candidate is tested before descending. */
    int stack[FDT_MAX_DEPTH];
    int depth = 0;
    stack[0] = off_of(&c, c.s);
    if (!ok(&c, c.s, 4) || rd_be32(c.s) != FDT_BEGIN_NODE) {
        return FDT_ERR_NODE;
    }

    for (;;) {
        int cur = stack[depth];
        uint32_t len = 0;
        const char *cp = (const char *)fdt_getprop(blob, cur, "compatible", &len);
        if (cur > previous_offset && cp && compatible_match(cp, len, compatible)) {
            return cur;
        }
        int ch = fdt_first_child(blob, cur);
        if (ch != FDT_ERR_NODE) {
            if (depth + 1 >= FDT_MAX_DEPTH) {
                /* Too deep to descend safely; skip this subtree rather than
                 * misbehave, and keep scanning siblings at known depths. */
                int sib = fdt_next_sibling(blob, cur, ch);
                while (sib != FDT_ERR_NODE) {
                    sib = fdt_next_sibling(blob, cur, sib);
                }
                if (depth == 0) {
                    return FDT_ERR_NODE;
                }
                depth--;
                continue;
            }
            depth++;
            stack[depth] = ch;
            continue;
        }
        /* No children: advance to a sibling, climbing as needed. */
        for (;;) {
            if (depth == 0) {
                return FDT_ERR_NODE;
            }
            int sib = fdt_next_sibling(blob, stack[depth - 1], stack[depth]);
            if (sib != FDT_ERR_NODE) {
                stack[depth] = sib;
                break;
            }
            depth--;
        }
    }
}

int fdt_node_by_compatible_offset(const void *blob, const char *compatible)
{
    return fdt_next_node_by_compatible_offset(blob, compatible, FDT_ERR_NODE);
}

/* Reads one of a node's own cell-count properties, or returns -1 if absent. */
static int own_cells(const ctx_t *c, const void *blob, int node_off,
                     const char *name, uint32_t *out)
{
    uint32_t len = 0;
    const uint8_t *v = (const uint8_t *)fdt_getprop(blob, node_off, name, &len);
    (void)c;
    if (!v || len < 4) {
        return -1;
    }
    *out = rd_be32(v);
    return 0;
}

int fdt_parent_cells(const void *blob, int node_offset, uint32_t *addr_cells,
                     uint32_t *size_cells)
{
    ctx_t c;
    if (!ctx_init(blob, &c)) {
        return 0;
    }
    /* Per spec a node's reg entries are sized by its PARENT's cell counts, so
     * this starts at the parent and inherits upward from the nearest ancestor
     * that declares them. Defaulting to 2/2 (the 64-bit convention) only when
     * nothing up the chain sets them at all. */
    uint32_t ac = 2, sc = 2;
    uint32_t found_ac = 0, found_sc = 0;
    int cur = fdt_parent_offset(blob, node_offset);

    while (cur >= 0) {
        uint32_t v = 0;
        if (!found_ac && own_cells(&c, blob, cur, "#address-cells", &v) == 0) {
            ac = v;
            found_ac = 1;
        }
        if (!found_sc && own_cells(&c, blob, cur, "#size-cells", &v) == 0) {
            sc = v;
            found_sc = 1;
        }
        if (found_ac && found_sc) {
            break;
        }
        cur = fdt_parent_offset(blob, cur);
    }
    if (addr_cells) {
        *addr_cells = ac;
    }
    if (size_cells) {
        *size_cells = sc;
    }
    return 1;
}

size_t fdt_reserved_ranges(const void *blob, fdt_range_t *out, size_t max)
{
    ctx_t c;
    if (!out || !ctx_init(blob, &c)) {
        return 0;
    }
    uint32_t off = rd_be32(c.base + 16); /* off_mem_rsvmap */
    uint32_t totalsize = rd_be32(c.base + 4);
    size_t count = 0;

    while (count < max) {
        if (off > totalsize || totalsize - off < 16) {
            break;
        }
        uint64_t addr = rd_be64(c.base + off);
        uint64_t size = rd_be64(c.base + off + 8);
        if (addr == 0 && size == 0) {
            break; /* terminator, per spec */
        }
        out[count].base = addr;
        out[count].size = size;
        count++;
        off += 16;
    }
    return count;
}

int fdt_reg_to_cpu_addr(const void *blob, int node_offset, uint32_t index,
                        fdt_range_t *out)
{
    ctx_t c;
    if (!out || !ctx_init(blob, &c)) {
        return 0;
    }
    uint32_t ac = 0, sc = 0;
    if (!fdt_parent_cells(blob, node_offset, &ac, &sc)) {
        return 0;
    }
    uint32_t len = 0;
    const uint8_t *reg = (const uint8_t *)fdt_getprop(blob, node_offset, "reg",
                                                      &len);
    if (!reg || ac == 0) {
        return 0;
    }
    uint32_t entry = (ac + sc) * 4;
    if (entry == 0 || (uint64_t)(index + 1) * entry > len) {
        return 0;
    }
    uint64_t base = rd_cells(reg + (uint64_t)index * entry, ac);
    uint64_t size = sc ? rd_cells(reg + (uint64_t)index * entry + ac * 4, sc) : 0;

    /* Walk up applying each bus's ranges. An empty ranges property is an
     * identity mapping (the bus maps 1:1); an absent one means the parent
     * does not map this bus, and the address is passed through unchanged
     * rather than invented. */
    int cur = node_offset;
    for (;;) {
        int parent = fdt_parent_offset(blob, cur);
        if (parent < 0) {
            break;
        }
        uint32_t rlen = 0;
        const uint8_t *ranges = (const uint8_t *)fdt_getprop(blob, cur, "ranges",
                                                             &rlen);
        if (!ranges) {
            cur = parent;
            continue;
        }
        if (rlen == 0) {
            /* identity */
            cur = parent;
            continue;
        }
        /* Entry layout, per spec: child address (the bus node's own
         * #address-cells), parent address (its parent's #address-cells), and
         * length (the bus node's own #size-cells). */
        uint32_t bus_ac = 0, bus_sc = 0;
        if (!fdt_parent_cells(blob, cur, &bus_ac, &bus_sc)) {
            return 0;
        }
        /* fdt_parent_cells describes cur's reg, i.e. cur's PARENT's counts;
         * for ranges the child addresses are sized by cur's OWN counts. */
        uint32_t own_ac = 2, own_sc = 2;
        (void)own_cells(&c, blob, cur, "#address-cells", &own_ac);
        (void)own_cells(&c, blob, cur, "#size-cells", &own_sc);
        if (own_ac == 0) {
            own_ac = 2;
        }
        uint32_t ra = bus_ac ? bus_ac : 2; /* parent address cells */
        uint32_t rs = own_sc ? own_sc : 2; /* length cells */
        uint32_t rentry = (own_ac + ra + rs) * 4;
        if (rentry == 0) {
            return 0;
        }
        uint32_t n = rlen / rentry;
        int mapped = 0;
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t *e = ranges + (uint64_t)i * rentry;
            uint64_t cbase = rd_cells(e, own_ac);
            uint64_t pbase = rd_cells(e + own_ac * 4, ra);
            uint64_t rsize = rd_cells(e + (own_ac + ra) * 4, rs);
            if (base >= cbase && base < cbase + rsize) {
                base = pbase + (base - cbase);
                mapped = 1;
                break;
            }
        }
        if (!mapped) {
            /* Outside every window of this bus: the address is not reachable
             * from the parent, so report failure rather than a wrong value. */
            return 0;
        }
        cur = parent;
        if (cur == 0) {
            break;
        }
    }
    out->base = base;
    out->size = size;
    return 1;
}

size_t fdt_memory_ranges(const void *blob, fdt_range_t *out, size_t max)
{
    ctx_t c;
    if (!out || !ctx_init(blob, &c)) {
        return 0;
    }
    int root = off_of(&c, c.s);
    if (!ok(&c, c.s, 4) || rd_be32(c.s) != FDT_BEGIN_NODE) {
        return 0;
    }
    size_t count = 0;

    /* /memory and /memory@... are always direct children of the root, per
     * spec. Matched by node name, which every real DTB (including QEMU's own
     * -machine virt output) sets, rather than by scanning for a device_type
     * property as a second pass. */
    for (int ch = fdt_first_child(blob, root); ch != FDT_ERR_NODE;
         ch = fdt_next_sibling(blob, root, ch)) {
        uint32_t namelen = 0;
        const char *n = node_name(&c, c.base + ch + 4, &namelen);
        if (!n || namelen < 6) {
            continue;
        }
        if (!(n[0] == 'm' && n[1] == 'e' && n[2] == 'm' && n[3] == 'o' &&
              n[4] == 'r' && n[5] == 'y' && (namelen == 6 || n[6] == '@'))) {
            continue;
        }
        uint32_t ac = 2, sc = 2;
        if (!fdt_parent_cells(blob, ch, &ac, &sc)) {
            continue;
        }
        uint32_t len = 0;
        const uint8_t *reg = (const uint8_t *)fdt_getprop(blob, ch, "reg", &len);
        if (!reg || ac == 0) {
            continue;
        }
        uint32_t entry = (ac + sc) * 4;
        if (entry == 0) {
            continue;
        }
        uint32_t n_entries = len / entry;
        for (uint32_t i = 0; i < n_entries && count < max; i++) {
            const uint8_t *e = reg + (uint64_t)i * entry;
            out[count].base = rd_cells(e, ac);
            out[count].size = sc ? rd_cells(e + ac * 4, sc) : 0;
            count++;
        }
    }
    return count;
}


int fdt_next_child(const void *blob, int parent_offset, int *cursor)
{
    if (!cursor) {
        return 0;
    }
    int c = (*cursor == 0) ? fdt_first_child(blob, parent_offset)
                           : fdt_next_sibling(blob, parent_offset, *cursor);
    if (c == FDT_ERR_NODE) {
        *cursor = 0;
        return 0;
    }
    *cursor = c;
    return c;
}

int fdt_node_by_compatible(const void *blob, const char *compatible,
                           int *out_offset)
{
    int off = fdt_node_by_compatible_offset(blob, compatible);
    if (off == FDT_ERR_NODE) {
        return 0;
    }
    if (out_offset) {
        *out_offset = off;
    }
    return 1;
}

int fdt_is_enabled(const void *blob, int node_offset)
{
    uint32_t len = 0;
    const char *st = (const char *)fdt_getprop(blob, node_offset, "status", &len);

    if (!st || len == 0) {
        return 1; /* absent means enabled, per spec */
    }
    /* The value is a NUL-terminated string within a property whose length
     * includes that terminator; comparing against the bounded length avoids
     * reading past it into the next property. */
    if (len >= 5 && st[0] == 'o' && st[1] == 'k' && st[2] == 'a' && st[3] == 'y') {
        return 1;
    }
    if (len >= 3 && st[0] == 'o' && st[1] == 'k' && (st[2] == 0 || len == 3)) {
        return 1;
    }
    return 0;
}

int fdt_cell_counts(const void *blob, int node_offset, int *out_address_cells,
                    int *out_size_cells)
{
    uint32_t ac = 0, sc = 0;
    if (!fdt_parent_cells(blob, node_offset, &ac, &sc)) {
        return 0;
    }
    if (out_address_cells) {
        *out_address_cells = (int)ac;
    }
    if (out_size_cells) {
        *out_size_cells = (int)sc;
    }
    return 1;
}

int fdt_reg_to_addr(const void *blob, int node_offset, uint64_t *out_base,
                    uint64_t *out_size)
{
    fdt_range_t r;
    if (!fdt_reg_to_cpu_addr(blob, node_offset, 0, &r)) {
        return 0;
    }
    if (out_base) {
        *out_base = r.base;
    }
    if (out_size) {
        *out_size = r.size;
    }
    return 1;
}


int fdt_own_cell_counts(const void *blob, int node_offset, int *out_address_cells,
                        int *out_size_cells)
{
    ctx_t c;
    uint32_t v = 0;
    int found = 0;

    if (!ctx_init(blob, &c)) {
        return 0;
    }
    /* Deliberately NOT walked up the chain: the whole point of this entry
     * point is the node's own declaration. Returning an inherited value here
     * would make it indistinguishable from fdt_cell_counts, and callers use
     * the difference to detect a malformed tree. */
    if (own_cells(&c, blob, node_offset, "#address-cells", &v) == 0) {
        if (out_address_cells) {
            *out_address_cells = (int)v;
        }
        found = 1;
    }
    if (own_cells(&c, blob, node_offset, "#size-cells", &v) == 0) {
        if (out_size_cells) {
            *out_size_cells = (int)v;
        }
        found = 1;
    }
    return found;
}
