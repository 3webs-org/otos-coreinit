#!/usr/bin/env python3
"""Compile coreinit's flat Cap'n Proto interfaces to relocatable C metadata.

The supported wire subset is deliberately narrow: methods with named UInt64
fields only. Each field is one data word, with no pointer section. Anything
else fails the build rather than silently producing a different validator.
Cap'n Proto interface signatures (not an extra JSON schema) are the source
of truth. This generator runs on the build host; no parser enters the image.
"""

import json
import re
import sys
from pathlib import Path

IDENT = r"[A-Za-z][A-Za-z0-9]*"
INTERFACE = re.compile(rf"interface\s+({IDENT})\s*\{{(.*?)\}}", re.S)
METHOD = re.compile(
    rf"({IDENT})\s*@([0-9]+)\s*\(([^()]*)\)\s*->\s*\(([^()]*)\)\s*;",
    re.S,
)
FIELD = re.compile(rf"({IDENT})\s*:\s*UInt64\Z")
METHOD_PREFIX = {
    "ClusterScheduler": "METHOD", "CsMutex": "MUTEX_METHOD",
    "MutexManager": "METHOD", "MemoryManager": "METHOD",
    "IrqManager": "IRQ_METHOD",
}


def fail(message):
    raise SystemExit(f"capnp schema generation: {message}")


def fields(text, interface, method, side):
    if not text.strip():
        return []
    names = []
    for item in text.split(","):
        match = FIELD.fullmatch(item.strip())
        if not match:
            fail(f"{interface}.{method} {side}: only named UInt64 fields are supported: {item.strip()!r}")
        if match.group(1) in names:
            fail(f"{interface}.{method} {side}: duplicate field {match.group(1)}")
        names.append(match.group(1))
    return names


def snake(name):
    return re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", name).lower()


def method_macro(interface, method):
    prefix = METHOD_PREFIX.get(interface, f"{snake(interface).upper()}_METHOD")
    return f"{prefix}_{snake(method).upper()}"


def parse(path):
    source = re.sub(r"#[^\n]*", "", path.read_text())
    file_id = re.match(r"\s*@0x[0-9a-fA-F]{16}\s*;", source)
    if not file_id:
        fail(f"{path}: missing 64-bit Cap'n Proto file ID")
    body = source[file_id.end():]
    interfaces = []
    cursor = 0
    for match in INTERFACE.finditer(body):
        if body[cursor:match.start()].strip():
            fail(f"{path}: unexpected text before interface {match.group(1)}")
        name = match.group(1)
        methods = []
        position = 0
        for method in METHOD.finditer(match.group(2)):
            if match.group(2)[position:method.start()].strip():
                fail(f"{name}: unsupported declaration before method {method.group(1)}")
            method_name, ordinal = method.group(1), int(method.group(2))
            params = fields(method.group(3), name, method_name, "request")
            results = fields(method.group(4), name, method_name, "response")
            if not params:
                fail(f"{name}.{method_name}: existing ABI requires at least one request word")
            methods.append(dict(name=method_name, id=ordinal, params=params, results=results))
            position = method.end()
        if match.group(2)[position:].strip():
            fail(f"{name}: trailing unsupported declaration")
        if sorted(m["id"] for m in methods) != list(range(len(methods))):
            fail(f"{name}: method ordinals must be contiguous, starting at zero")
        if len(methods) > 65535:
            fail(f"{name}: too many methods for schema_table_t")
        methods.sort(key=lambda m: m["id"])
        interfaces.append(dict(name=name, symbol=f"g_{snake(name)}_table", methods=methods))
        cursor = match.end()
    if body[cursor:].strip() or not interfaces:
        fail(f"{path}: unexpected text after interfaces or no interfaces")
    symbols = [interface["symbol"] for interface in interfaces]
    if len(set(symbols)) != len(symbols):
        fail(f"{path}: duplicate interface/table names")
    macros = [method_macro(i["name"], m["name"])
              for i in interfaces for m in i["methods"]]
    if len(set(macros)) != len(macros):
        fail(f"{path}: duplicate method ID macro names")
    return interfaces


def shapes(interfaces):
    for interface in interfaces:
        words = []
        for method in interface["methods"]:
            for side in ("params", "results"):
                count = len(method[side])
                if count and count not in words:
                    words.append(count)
                method[side + "_index"] = words.index(count) if count else None
        if len(words) > 65535 or any(count > 65535 for count in words):
            fail(f"{interface['name']}: struct shape exceeds uint16_t")
        interface["words"] = words
    return interfaces


def abi(interfaces):
    return {
        interface["name"]: [
            [method["id"], len(method["params"]), len(method["results"]) or None]
            for method in interface["methods"]
        ]
        for interface in interfaces
    }


def emit(interfaces, output):
    header = ["/* Generated from schemas/coreinit.capnp; do not edit. */",
              "#pragma once", '#include "schema.h"', ""]
    source = ["/* Generated from schemas/coreinit.capnp; do not edit. */",
              '#include "generated_schemas.h"', ""]
    for interface in interfaces:
        symbol = interface["symbol"]
        prefix = snake(interface["name"])
        header.append(f"extern const schema_table_t {symbol};")
        for method in interface["methods"]:
            macro = method_macro(interface["name"], method["name"])
            header.append(f"#define {macro} {method['id']}")
        header.append("")
        source.append(f"static const struct_schema_t {prefix}_structs[] = {{")
        source.extend(f"    {{{count}, 0, SCHEMA_IDX_NONE}}," for count in interface["words"])
        source.append("};")
        source.append(f"static const method_schema_t {prefix}_methods[] = {{")
        for method in interface["methods"]:
            param = method["params_index"]
            result = method["results_index"]
            result_code = "SCHEMA_IDX_NONE" if result is None else str(result)
            source.append(f"    {{{method['id']}, {param}, {result_code}}}, /* {method['name']} */")
        source.extend(["};", f"static const object_schema_t {prefix}_objects[] = {{",
                       f"    {{0, {len(interface['methods'])}}},", "};",
                       f"const schema_table_t {symbol} = {{",
                       f"    {prefix}_structs, {len(interface['words'])}, NULL, 0,",
                       f"    {prefix}_methods, {len(interface['methods'])}, {prefix}_objects, 1",
                       "};", ""])
    output.mkdir(parents=True, exist_ok=True)
    (output / "generated_schemas.h").write_text("\n".join(header) + "\n")
    (output / "generated_schemas.c").write_text("\n".join(source) + "\n")
    (output / "generated_schemas.json").write_text(json.dumps(abi(interfaces), indent=2) + "\n")


def main():
    if len(sys.argv) != 3:
        fail("usage: gen-capnp-schemas.py schemas/coreinit.capnp <build-dir>")
    path, output = Path(sys.argv[1]), Path(sys.argv[2])
    interfaces = shapes(parse(path))
    baseline = path.parent / "abi-baseline.json"
    if baseline.exists() and abi(interfaces) != json.loads(baseline.read_text()):
        fail("wire ABI differs from schemas/abi-baseline.json; review the protocol change")
    emit(interfaces, output)


if __name__ == "__main__":
    main()
