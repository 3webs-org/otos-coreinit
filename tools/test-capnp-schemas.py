#!/usr/bin/env python3
"""Build-time ABI regression for the Cap'n Proto source-to-const-C compiler."""

import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SASKI_KERNEL = Path(os.environ.get("SASKI_ROOT", str(ROOT.parent / "saski"))) / "kernel"
SCHEMA = ROOT / "schemas/coreinit.capnp"
BASELINE = ROOT / "schemas/abi-baseline.json"
GENERATOR = ROOT / "tools/gen-capnp-schemas.py"


def run(*args, expected=0):
    result = subprocess.run(args, text=True, capture_output=True)
    if (result.returncode == 0) != (expected == 0):
        raise AssertionError(f"unexpected exit {result.returncode}: {args}\n{result.stderr}")
    return result


with tempfile.TemporaryDirectory(prefix="coreinit-capnp-") as tmp:
    output = Path(tmp)
    run(sys.executable, str(GENERATOR), str(SCHEMA), str(output))
    actual = json.loads((output / "generated_schemas.json").read_text())
    baseline = json.loads(BASELINE.read_text())
    assert actual == baseline
    assert len(actual) == 12 and sum(len(methods) for methods in actual.values()) == 33

    compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
    if not compiler:
        raise AssertionError("a C compiler is required to validate generated tables")
    verifier = [
        '#include "generated_schemas.h"',
        '#include <stdio.h>',
        'static int check(const schema_table_t *t, int n, const int *p, const int *r) {',
        '  if (t->object_count != 1 || t->objects[0].method_count != n ||',
        '      t->objects[0].first_method != 0 || t->method_count != n ||',
        '      t->pointer_count != 0) return 0;',
        '  for (int i = 0; i < n; i++) {',
        '    const method_schema_t *m = schema_method(t, i);',
        '    const struct_schema_t *request = schema_struct(t, m->param_schema);',
        '    const struct_schema_t *response = schema_struct(t, m->result_schema);',
        '    if (m->method_id != (unsigned)i || !request ||',
        '        request->data_words != p[i] || request->pointer_count != 0 ||',
        '        (r[i] < 0 ? m->result_schema != SCHEMA_IDX_NONE :',
        '         !response || response->data_words != r[i] ||',
        '         response->pointer_count != 0)) return 0;',
        '  }',
        '  return 1;',
        '}',
        'int main(void) {',
        '  if (g_resource_table.structs != NULL) return 2;',
        '  coreinit_schemas_init();',
    ]
    for interface, methods in baseline.items():
        names = f"g_{''.join('_' + c.lower() if c.isupper() else c for c in interface).lstrip('_')}_table"
        params = ', '.join(str(param) for _, param, _ in methods)
        results = ', '.join(str(result if result is not None else -1) for _, _, result in methods)
        verifier.append(f'  if (!check(&{names}, {len(methods)}, (int[]){{{params}}},'
                        f' (int[]){{{results}}})) return 1;')
    verifier.extend(['  puts("generated schema tables match ABI");', '  return 0;', '}'])
    check_c = output / "check.c"
    check_c.write_text("\n".join(verifier) + "\n")
    executable = output / "check"
    run(compiler, "-std=c11", f"-I{output}", f"-I{SASKI_KERNEL}",
        str(output / "generated_schemas.c"), str(check_c), "-o", str(executable))
    run(str(executable))

    bad_dir = output / "bad"
    bad_dir.mkdir()
    changed = SCHEMA.read_text().replace(
        "update @0 (value :UInt64) -> ();", "update @0 (value :UInt64) -> (ok :UInt64);", 1
    )
    bad_schema = bad_dir / "coreinit.capnp"
    bad_schema.write_text(changed)
    (bad_dir / "abi-baseline.json").write_text(BASELINE.read_text())
    assert "wire ABI differs" in run(sys.executable, str(GENERATOR),
                                     str(bad_schema), str(bad_dir), expected=1).stderr
    bad_schema.write_text(SCHEMA.read_text().replace(
        "update @0 (value :UInt64)", "update @0 (value :Text)", 1
    ))
    assert "only named UInt64" in run(sys.executable, str(GENERATOR),
                                      str(bad_schema), str(bad_dir), expected=1).stderr

with tempfile.TemporaryDirectory(prefix="securinit-capnp-") as tmp:
    output = Path(tmp)
    run(sys.executable, str(GENERATOR), str(ROOT / "schemas/securinit.capnp"), str(output))
    assert json.loads((output / "generated_schemas.json").read_text()) == json.loads(
        (ROOT / "schemas/securinit-abi-baseline.json").read_text()
    )
    check_c = output / "check.c"
    check_c.write_text('''#include "generated_schemas.h"
int main(void) {
    if (g_status_table.structs || g_gate_table.structs || g_admin_table.structs ||
        g_wrapper_factory_table.structs || g_wrapped_object_table.structs) return 1;
    securinit_schemas_init();
    const method_schema_t *s = schema_method(&g_status_table, 0);
    const method_schema_t *gate_call = schema_method(&g_gate_table, 0);
    const method_schema_t *gate_stats = schema_method(&g_gate_table, 1);
    const method_schema_t *admin = schema_method(&g_admin_table, 0);
    const method_schema_t *factory = schema_method(&g_wrapper_factory_table, 0);
    const method_schema_t *wrapped = schema_method(&g_wrapped_object_table, 0);
    return !s || !gate_call || !gate_stats || !admin || !factory || !wrapped ||
           schema_struct(&g_status_table, s->param_schema)->data_words != 1 ||
           s->result_schema != SCHEMA_IDX_NONE ||
           g_gate_table.objects[0].method_count != 2 ||
           schema_struct(&g_gate_table, gate_call->result_schema)->data_words != 1 ||
           schema_struct(&g_gate_table, gate_stats->result_schema)->data_words != 2 ||
           schema_struct(&g_admin_table, admin->result_schema)->data_words != 1 ||
           schema_struct(&g_wrapper_factory_table, factory->param_schema)->data_words != 1 ||
           schema_struct(&g_wrapper_factory_table, factory->result_schema)->data_words != 1 ||
           schema_struct(&g_wrapped_object_table, wrapped->result_schema)->data_words != 1;
}
''')
    executable = output / "check"
    run(compiler, "-std=c11", f"-I{output}", f"-I{SASKI_KERNEL}",
        str(output / "generated_schemas.c"), str(check_c), "-o", str(executable))
    run(str(executable))
    ipc_check = output / "check-user-ipc"
    run(compiler, "-std=c11", f"-I{output}", f"-I{SASKI_KERNEL}",
        f"-I{ROOT / 'securinit'}", f"-I{ROOT / 'coreinit'}",
        str(ROOT / "securinit/user_ipc.c"), str(ROOT / "tools/test-user-ipc.c"),
        "-o", str(ipc_check))
    run(str(ipc_check))

print("Cap'n Proto source → 17 generated object tables / 40 methods: ABI PASS")
