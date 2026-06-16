#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Psynder editor IPC IDL stub generator.

Reads engine/editor/ipc/protocol.psy and emits:
  - Protocol.gen.h  : C++ structs + msgpack encode/decode helpers
  - protocol.gen.ts : TypeScript module with the same shape

Invoked by the CMake custom-command in engine/editor/ipc/CMakeLists.txt so
schema edits flow through the build automatically.

Grammar is line-oriented; see protocol.psy for the canonical reference.
"""
from __future__ import annotations

import argparse
import os
import re
import sys
from dataclasses import dataclass, field
from typing import Optional


# ─── Lexical helpers ────────────────────────────────────────────────────────
PRIMITIVES = {
    "bool", "i8", "i16", "i32", "i64",
    "u8", "u16", "u32", "u64",
    "f32", "f64", "str", "bin",
}

CPP_TYPE = {
    "bool": "bool",
    "i8":  "::psynder::i8",  "i16": "::psynder::i16",
    "i32": "::psynder::i32", "i64": "::psynder::i64",
    "u8":  "::psynder::u8",  "u16": "::psynder::u16",
    "u32": "::psynder::u32", "u64": "::psynder::u64",
    "f32": "::psynder::f32", "f64": "::psynder::f64",
    "str": "std::string",
    "bin": "std::vector<::psynder::u8>",
}

TS_TYPE = {
    "bool": "boolean",
    "i8": "number", "i16": "number", "i32": "number",
    "u8": "number", "u16": "number", "u32": "number",
    "f32": "number", "f64": "number",
    # 64-bit ints are bigint in TS to avoid silent precision loss.
    "i64": "bigint", "u64": "bigint",
    "str": "string",
    "bin": "Uint8Array",
}


# ─── AST ────────────────────────────────────────────────────────────────────
@dataclass
class Field:
    name: str
    type: str  # raw IDL type string


@dataclass
class StructDef:
    name: str
    fields: list[Field] = field(default_factory=list)


@dataclass
class EnumDef:
    name: str
    variants: list[tuple[str, int]] = field(default_factory=list)


@dataclass
class FrameDef:
    name: str
    opcode: int
    channel: str
    body: str  # struct name


@dataclass
class Protocol:
    version: int = 1
    channels: list[str] = field(default_factory=list)
    enums: list[EnumDef] = field(default_factory=list)
    structs: list[StructDef] = field(default_factory=list)
    frames: list[FrameDef] = field(default_factory=list)


# ─── Parser ─────────────────────────────────────────────────────────────────
class ParseError(RuntimeError):
    pass


def _strip_comment(line: str) -> str:
    # comments start at '#' (no escapes in this IDL)
    if "#" in line:
        return line[: line.index("#")]
    return line


def parse(path: str) -> Protocol:
    proto = Protocol()
    cur_struct: Optional[StructDef] = None
    cur_enum: Optional[EnumDef] = None
    cur_frame: Optional[FrameDef] = None
    with open(path, "r", encoding="utf-8") as fh:
        for lineno, raw in enumerate(fh, start=1):
            line = _strip_comment(raw).rstrip()
            if not line.strip():
                continue
            indent = len(line) - len(line.lstrip())
            stripped = line.strip()

            if indent == 0:
                # any open block ends
                cur_struct = cur_enum = cur_frame = None

                if stripped.startswith("version "):
                    proto.version = int(stripped.split()[1])
                    continue
                if stripped.startswith("channel "):
                    m = re.match(r'channel\s+"([^"]+)"', stripped)
                    if not m:
                        raise ParseError(f"{path}:{lineno}: bad channel decl")
                    proto.channels.append(m.group(1))
                    continue
                if stripped.startswith("struct "):
                    m = re.match(r"struct\s+([A-Za-z_]\w*)\s*:", stripped)
                    if not m:
                        raise ParseError(f"{path}:{lineno}: bad struct decl")
                    cur_struct = StructDef(name=m.group(1))
                    proto.structs.append(cur_struct)
                    continue
                if stripped.startswith("enum "):
                    m = re.match(r"enum\s+([A-Za-z_]\w*)\s*:", stripped)
                    if not m:
                        raise ParseError(f"{path}:{lineno}: bad enum decl")
                    cur_enum = EnumDef(name=m.group(1))
                    proto.enums.append(cur_enum)
                    continue
                if stripped.startswith("frame "):
                    m = re.match(r"frame\s+([A-Za-z_]\w*)\s*:", stripped)
                    if not m:
                        raise ParseError(f"{path}:{lineno}: bad frame decl")
                    cur_frame = FrameDef(name=m.group(1), opcode=0, channel="*", body="")
                    proto.frames.append(cur_frame)
                    continue
                raise ParseError(f"{path}:{lineno}: unexpected top-level directive: {stripped!r}")

            # indented (block body)
            if cur_struct is not None:
                m = re.match(r"([A-Za-z_]\w*)\s*:\s*(.+)$", stripped)
                if not m:
                    raise ParseError(f"{path}:{lineno}: bad struct field")
                cur_struct.fields.append(Field(name=m.group(1), type=m.group(2).strip()))
                continue
            if cur_enum is not None:
                m = re.match(r"([A-Za-z_]\w*)\s*=\s*(-?\d+)$", stripped)
                if not m:
                    raise ParseError(f"{path}:{lineno}: bad enum variant")
                cur_enum.variants.append((m.group(1), int(m.group(2))))
                continue
            if cur_frame is not None:
                if "=" not in stripped:
                    raise ParseError(f"{path}:{lineno}: bad frame attr")
                key, val = [p.strip() for p in stripped.split("=", 1)]
                if key == "opcode":
                    cur_frame.opcode = int(val)
                elif key == "channel":
                    if val.startswith('"') and val.endswith('"'):
                        cur_frame.channel = val[1:-1]
                    else:
                        cur_frame.channel = val
                elif key == "body":
                    cur_frame.body = val
                else:
                    raise ParseError(f"{path}:{lineno}: unknown frame attr {key!r}")
                continue
            raise ParseError(f"{path}:{lineno}: indented line outside a block")
    return proto


# ─── Type resolution ────────────────────────────────────────────────────────
def cpp_type_for(raw: str) -> str:
    raw = raw.strip()
    if raw in PRIMITIVES:
        return CPP_TYPE[raw]
    m = re.match(r"array\s*<\s*(.+)\s*>$", raw)
    if m:
        return f"std::vector<{cpp_type_for(m.group(1))}>"
    m = re.match(r"map\s*<\s*([^,]+)\s*,\s*(.+)\s*>$", raw)
    if m:
        return f"std::unordered_map<{cpp_type_for(m.group(1))}, {cpp_type_for(m.group(2))}>"
    # else assume user-defined (struct or enum) — emit raw name
    return raw


def ts_type_for(raw: str) -> str:
    raw = raw.strip()
    if raw in PRIMITIVES:
        return TS_TYPE[raw]
    m = re.match(r"array\s*<\s*(.+)\s*>$", raw)
    if m:
        return f"Array<{ts_type_for(m.group(1))}>"
    m = re.match(r"map\s*<\s*([^,]+)\s*,\s*(.+)\s*>$", raw)
    if m:
        return f"Map<{ts_type_for(m.group(1))}, {ts_type_for(m.group(2))}>"
    return raw


# ─── Codegen — C++ ──────────────────────────────────────────────────────────
def _cpp_encode_call(field_type: str, expr: str) -> str:
    t = field_type.strip()
    if t in PRIMITIVES:
        return f"encode(out, {expr});"
    if t.startswith("array<") or t.startswith("map<"):
        return f"encode(out, {expr});"
    # struct/enum: call the struct's encode()
    return f"{t}_encode(out, {expr});"


def _cpp_decode_call(field_type: str, expr: str) -> str:
    t = field_type.strip()
    if t in PRIMITIVES:
        return f"if (!decode(in, {expr})) return false;"
    if t.startswith("array<") or t.startswith("map<"):
        return f"if (!decode(in, {expr})) return false;"
    return f"if (!{t}_decode(in, {expr})) return false;"


def emit_cpp(proto: Protocol) -> str:
    out: list[str] = []
    P = out.append
    P("// SPDX-License-Identifier: MIT")
    P("// AUTO-GENERATED — do not edit. Source: engine/editor/ipc/protocol.psy")
    P("// Regenerate via the CMake custom-command in engine/editor/ipc/CMakeLists.txt.")
    P("")
    P("#pragma once")
    P("")
    P('#include "core/Types.h"')
    P('#include "editor/ipc/internal/Msgpack.h"')
    P("")
    P("#include <cstdint>")
    P("#include <string>")
    P("#include <unordered_map>")
    P("#include <vector>")
    P("")
    P("namespace psynder::editor::ipc::proto {")
    P("")
    P(f"inline constexpr ::psynder::u32 kProtocolVersion = {proto.version};")
    P("")

    # Channels — emit as constexpr string_view list + name table
    P("// Channel ids — wire names")
    P("namespace channels {")
    for ch in proto.channels:
        ident = re.sub(r"[^A-Za-z0-9_]", "_", ch)
        P(f'    inline constexpr const char* k{ident} = "{ch}";')
    P("}  // namespace channels")
    P("")

    # Enums
    for e in proto.enums:
        P(f"enum class {e.name} : ::psynder::i32 {{")
        for n, v in e.variants:
            P(f"    {n} = {v},")
        P("};")
        P("")

    # Forward declarations for struct encode/decode
    for s in proto.structs:
        P(f"struct {s.name};")
        P(f"void {s.name}_encode(::psynder::editor::ipc::msgpack::Writer& out, const {s.name}& v);")
        P(f"bool {s.name}_decode(::psynder::editor::ipc::msgpack::Reader& in, {s.name}& v);")
    P("")

    # Struct definitions
    for s in proto.structs:
        P(f"struct {s.name} {{")
        for fld in s.fields:
            ctype = cpp_type_for(fld.type)
            # default-init primitives to zero
            if fld.type in {"bool"}:
                P(f"    {ctype} {fld.name} = false;")
            elif fld.type in {"i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64"}:
                P(f"    {ctype} {fld.name} = 0;")
            elif fld.type in {"f32", "f64"}:
                P(f"    {ctype} {fld.name} = 0;")
            else:
                P(f"    {ctype} {fld.name}{{}};")
        P("};")
        P("")

    # Struct encode/decode implementations (inline; header-only generated code)
    P("// ─── Encode/decode implementations ──────────────────────────────────")
    for s in proto.structs:
        P(f"inline void {s.name}_encode(::psynder::editor::ipc::msgpack::Writer& out, const {s.name}& v) {{")
        P(f"    using namespace ::psynder::editor::ipc::msgpack;")
        P(f"    out.array_header({len(s.fields)});")
        for fld in s.fields:
            P(f"    {_cpp_encode_call(fld.type, 'v.' + fld.name)}")
        P("}")
        P("")
        P(f"inline bool {s.name}_decode(::psynder::editor::ipc::msgpack::Reader& in, {s.name}& v) {{")
        P(f"    using namespace ::psynder::editor::ipc::msgpack;")
        P(f"    ::psynder::u32 sz = 0;")
        P(f"    if (!in.array_header(sz)) return false;")
        P(f"    if (sz != {len(s.fields)}) return false;")
        for fld in s.fields:
            P(f"    {_cpp_decode_call(fld.type, 'v.' + fld.name)}")
        P("    return true;")
        P("}")
        P("")

    # Frame opcode constants + dispatch helper
    P("// ─── Frame opcodes ──────────────────────────────────────────────────")
    P("namespace opcodes {")
    for fr in proto.frames:
        P(f"    inline constexpr ::psynder::u16 k{fr.name} = {fr.opcode};")
    P("}  // namespace opcodes")
    P("")
    P("}  // namespace psynder::editor::ipc::proto")
    return "\n".join(out) + "\n"


# ─── Codegen — TypeScript ──────────────────────────────────────────────────
def emit_ts(proto: Protocol) -> str:
    out: list[str] = []
    P = out.append
    P("// SPDX-License-Identifier: MIT")
    P("// AUTO-GENERATED — do not edit. Source: engine/editor/ipc/protocol.psy")
    P("// Regenerate via CMake or `npm run gen:ipc` in engine/editor/web.")
    P("")
    P(f"export const kProtocolVersion = {proto.version};")
    P("")
    P("export const channels = {")
    for ch in proto.channels:
        ident = re.sub(r"[^A-Za-z0-9_]", "_", ch)
        P(f'    {ident}: "{ch}",')
    P("} as const;")
    P("")
    for e in proto.enums:
        P(f"export enum {e.name} {{")
        for n, v in e.variants:
            P(f"    {n} = {v},")
        P("}")
        P("")
    for s in proto.structs:
        P(f"export interface {s.name} {{")
        for fld in s.fields:
            P(f"    {fld.name}: {ts_type_for(fld.type)};")
        P("}")
        P("")
    P("export const opcodes = {")
    for fr in proto.frames:
        P(f"    {fr.name}: {fr.opcode},")
    P("} as const;")
    P("")
    P("export type FrameName = keyof typeof opcodes;")
    return "\n".join(out) + "\n"


# ─── Entry point ────────────────────────────────────────────────────────────
def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--input", required=True, help="protocol.psy")
    ap.add_argument("--cpp-out", help="path to Protocol.gen.h")
    ap.add_argument("--ts-out", help="path to protocol.gen.ts")
    args = ap.parse_args()
    if not args.cpp_out and not args.ts_out:
        ap.error("at least one of --cpp-out or --ts-out is required")
    try:
        proto = parse(args.input)
    except ParseError as e:
        print(f"psyidl: {e}", file=sys.stderr)
        return 1

    # Only rewrite on change so CMake's custom-command doesn't churn.
    def write_if_changed(path: str, content: str) -> None:
        os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
        try:
            with open(path, "r", encoding="utf-8") as fh:
                if fh.read() == content:
                    return
        except OSError:
            pass
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(content)

    if args.cpp_out:
        write_if_changed(args.cpp_out, emit_cpp(proto))
    if args.ts_out:
        write_if_changed(args.ts_out, emit_ts(proto))
    return 0


if __name__ == "__main__":
    sys.exit(main())
