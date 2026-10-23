# Coreinit wire schemas

`coreinit.capnp` defines all ten coreinit IPC interfaces and their method IDs.
`build_coreinit.sh` compiles it with `tools/gen-capnp-schemas.py` on **every**
x86_64 and aarch64 build, producing `build/generated_schemas.{c,h}`. Only
const struct/method/object arrays and static per-instance descriptor bridges
are linked into coreinit; no schema text, Cap'n Proto compiler, heap
allocation, or runtime parser is needed in the image.

The generator accepts the existing flat-wire subset: named `UInt64` fields
only. Each is one data word; an empty method result retains the existing
`SCHEMA_IDX_NONE` validation opt-out. Capability handles are integer words,
not Cap'n Proto capability pointers. The disk sector is 64 inline words to
preserve its 512-byte response. Unsupported field types fail the build
instead of silently changing the wire layout. `coreinit_schemas_init()` wires
only the descriptor pointers to THIS copy's const arrays before any object is
created; AP Mach-O relocation does not fix embedded absolute pointer data.

`abi-baseline.json` records the previously shipped request/result word counts
for all 31 methods. A protocol change requires reviewing and deliberately
updating that snapshot. `python3 tools/test-capnp-schemas.py` checks the
snapshot, tests rejection of incompatible edits, compiles the generated C,
and verifies the resulting tables' method IDs and shapes at runtime.
