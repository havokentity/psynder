// SPDX-License-Identifier: MIT
// Psynder editor — visual-script (PsyGraph) authoring model + serializer.
//
// This is the BROWSER-side mirror of the engine's PsyGraph VM data model
// (engine/script/psygraph/NodeTypes.h + Graph.h) and its binary serializer
// (engine/script/psygraph/Serialize.cpp). The web graph panel authors a graph
// in these structures, serializes it to the SAME little-endian blob the C++
// Compiler/VM consume, hex-encodes it, and ships it to the engine over the
// main-thread console-command IPC ("psygraph_set <entity> <hexblob>"). The
// engine validates (deserialize + compile) before storing it in the scene, so a
// graph authored here is the exact graph that runs in Play.
//
// Keeping the node catalog + blob layout here (and not generated) is the MVP
// approach: the catalog is small and frozen, mirroring how the C++ NodeTypes
// table is code-defined while the graph instance is user data.

// Stable node-type ids — MUST match engine/script/psygraph/NodeTypes.h.
export enum NodeTypeId {
    OnStart = 1,
    OnTick = 2,
    OnTrigger = 3,
    OnDamaged = 4,

    Branch = 20,
    Sequence = 21,

    Add = 40,
    Sub = 41,
    Mul = 42,
    Div = 43,
    Neg = 44,

    Equal = 60,
    Less = 61,
    Greater = 62,
    And = 63,
    Or = 64,
    Not = 65,

    GetVar = 80,
    SetVar = 81,

    LiteralFloat = 90,
    LiteralBool = 91,
    LiteralInt = 92,
    LiteralString = 93,

    Log = 100,
    SetHealth = 101,
    ApplyDamage = 102,
    SpawnEntity = 103,
    SetActive = 104,
    PlaySound = 105,
}

export const EDGE_EXEC = 0;
export const EDGE_DATA = 1;

// Pin value types — MUST match engine/script/psygraph/Value.h ValueType.
export enum PinType {
    Exec = 0,
    Bool = 1,
    Int = 2,
    Float = 3,
    Entity = 4,
    String = 5,
    Any = 6,
}

export interface PinSpec {
    name: string;
    type: PinType;
}

// How a node's inline params are edited in the panel. Mirrors the C++ param
// semantics (Node::params, interpreted per NodeTypeId).
export type ParamKind = 'none' | 'float' | 'int' | 'bool' | 'string' | 'varSlot';

export interface NodeTypeInfo {
    id: NodeTypeId;
    name: string;
    category: 'Event' | 'Flow' | 'Math' | 'Compare' | 'Variable' | 'Literal' | 'Action';
    isEvent: boolean;
    isPure: boolean;
    execIn: number;            // 0 or 1
    execOut: PinSpec[];        // named exec output pins
    dataIn: PinSpec[];
    dataOut: PinSpec[];
    param: ParamKind;          // single inline param (matches the MVP catalog)
}

const FF: PinSpec[] = [{ name: 'A', type: PinType.Float }, { name: 'B', type: PinType.Float }];
const F: PinSpec[] = [{ name: 'A', type: PinType.Float }];
const BB: PinSpec[] = [{ name: 'A', type: PinType.Bool }, { name: 'B', type: PinType.Bool }];
const B: PinSpec[] = [{ name: 'A', type: PinType.Bool }];
const FLOAT_OUT: PinSpec[] = [{ name: 'Result', type: PinType.Float }];
const BOOL_OUT: PinSpec[] = [{ name: 'Result', type: PinType.Bool }];
const EXEC_OUT: PinSpec[] = [{ name: 'Then', type: PinType.Exec }];
const BRANCH_OUT: PinSpec[] = [
    { name: 'True', type: PinType.Exec },
    { name: 'False', type: PinType.Exec },
];

// The catalog, mirroring engine/script/psygraph/NodeTypes.cpp kCatalog.
export const NODE_CATALOG: NodeTypeInfo[] = [
    { id: NodeTypeId.OnStart, name: 'OnStart', category: 'Event', isEvent: true, isPure: false, execIn: 0, execOut: EXEC_OUT, dataIn: [], dataOut: [], param: 'none' },
    { id: NodeTypeId.OnTick, name: 'OnTick', category: 'Event', isEvent: true, isPure: false, execIn: 0, execOut: EXEC_OUT, dataIn: [], dataOut: [{ name: 'DeltaTime', type: PinType.Float }], param: 'none' },
    { id: NodeTypeId.OnDamaged, name: 'OnDamaged', category: 'Event', isEvent: true, isPure: false, execIn: 0, execOut: EXEC_OUT, dataIn: [], dataOut: [{ name: 'Amount', type: PinType.Float }, { name: 'Source', type: PinType.Entity }], param: 'none' },

    { id: NodeTypeId.Branch, name: 'Branch', category: 'Flow', isEvent: false, isPure: false, execIn: 1, execOut: BRANCH_OUT, dataIn: [{ name: 'Condition', type: PinType.Bool }], dataOut: [], param: 'none' },
    { id: NodeTypeId.Sequence, name: 'Sequence', category: 'Flow', isEvent: false, isPure: false, execIn: 1, execOut: [{ name: 'Then0', type: PinType.Exec }, { name: 'Then1', type: PinType.Exec }], dataIn: [], dataOut: [], param: 'none' },

    { id: NodeTypeId.Add, name: 'Add', category: 'Math', isEvent: false, isPure: true, execIn: 0, execOut: [], dataIn: FF, dataOut: FLOAT_OUT, param: 'none' },
    { id: NodeTypeId.Sub, name: 'Sub', category: 'Math', isEvent: false, isPure: true, execIn: 0, execOut: [], dataIn: FF, dataOut: FLOAT_OUT, param: 'none' },
    { id: NodeTypeId.Mul, name: 'Mul', category: 'Math', isEvent: false, isPure: true, execIn: 0, execOut: [], dataIn: FF, dataOut: FLOAT_OUT, param: 'none' },
    { id: NodeTypeId.Div, name: 'Div', category: 'Math', isEvent: false, isPure: true, execIn: 0, execOut: [], dataIn: FF, dataOut: FLOAT_OUT, param: 'none' },
    { id: NodeTypeId.Neg, name: 'Neg', category: 'Math', isEvent: false, isPure: true, execIn: 0, execOut: [], dataIn: F, dataOut: FLOAT_OUT, param: 'none' },

    { id: NodeTypeId.Equal, name: 'Equal', category: 'Compare', isEvent: false, isPure: true, execIn: 0, execOut: [], dataIn: FF, dataOut: BOOL_OUT, param: 'none' },
    { id: NodeTypeId.Less, name: 'Less', category: 'Compare', isEvent: false, isPure: true, execIn: 0, execOut: [], dataIn: FF, dataOut: BOOL_OUT, param: 'none' },
    { id: NodeTypeId.Greater, name: 'Greater', category: 'Compare', isEvent: false, isPure: true, execIn: 0, execOut: [], dataIn: FF, dataOut: BOOL_OUT, param: 'none' },
    { id: NodeTypeId.And, name: 'And', category: 'Compare', isEvent: false, isPure: true, execIn: 0, execOut: [], dataIn: BB, dataOut: BOOL_OUT, param: 'none' },
    { id: NodeTypeId.Or, name: 'Or', category: 'Compare', isEvent: false, isPure: true, execIn: 0, execOut: [], dataIn: BB, dataOut: BOOL_OUT, param: 'none' },
    { id: NodeTypeId.Not, name: 'Not', category: 'Compare', isEvent: false, isPure: true, execIn: 0, execOut: [], dataIn: B, dataOut: BOOL_OUT, param: 'none' },

    { id: NodeTypeId.GetVar, name: 'GetVar', category: 'Variable', isEvent: false, isPure: true, execIn: 0, execOut: [], dataIn: [], dataOut: [{ name: 'Value', type: PinType.Any }], param: 'varSlot' },
    { id: NodeTypeId.SetVar, name: 'SetVar', category: 'Variable', isEvent: false, isPure: false, execIn: 1, execOut: EXEC_OUT, dataIn: [{ name: 'Value', type: PinType.Any }], dataOut: [], param: 'varSlot' },

    { id: NodeTypeId.LiteralFloat, name: 'LiteralFloat', category: 'Literal', isEvent: false, isPure: true, execIn: 0, execOut: [], dataIn: [], dataOut: [{ name: 'Value', type: PinType.Float }], param: 'float' },
    { id: NodeTypeId.LiteralBool, name: 'LiteralBool', category: 'Literal', isEvent: false, isPure: true, execIn: 0, execOut: [], dataIn: [], dataOut: [{ name: 'Value', type: PinType.Bool }], param: 'bool' },
    { id: NodeTypeId.LiteralInt, name: 'LiteralInt', category: 'Literal', isEvent: false, isPure: true, execIn: 0, execOut: [], dataIn: [], dataOut: [{ name: 'Value', type: PinType.Int }], param: 'int' },
    { id: NodeTypeId.LiteralString, name: 'LiteralString', category: 'Literal', isEvent: false, isPure: true, execIn: 0, execOut: [], dataIn: [], dataOut: [{ name: 'Value', type: PinType.String }], param: 'string' },

    { id: NodeTypeId.Log, name: 'Log', category: 'Action', isEvent: false, isPure: false, execIn: 1, execOut: EXEC_OUT, dataIn: [{ name: 'Message', type: PinType.String }], dataOut: [], param: 'none' },
    { id: NodeTypeId.SetHealth, name: 'SetHealth', category: 'Action', isEvent: false, isPure: false, execIn: 1, execOut: EXEC_OUT, dataIn: [{ name: 'Target', type: PinType.Entity }, { name: 'Health', type: PinType.Float }], dataOut: [], param: 'none' },
    { id: NodeTypeId.ApplyDamage, name: 'ApplyDamage', category: 'Action', isEvent: false, isPure: false, execIn: 1, execOut: EXEC_OUT, dataIn: [{ name: 'Target', type: PinType.Entity }, { name: 'Amount', type: PinType.Float }], dataOut: [], param: 'none' },
    { id: NodeTypeId.SpawnEntity, name: 'SpawnEntity', category: 'Action', isEvent: false, isPure: false, execIn: 1, execOut: EXEC_OUT, dataIn: [{ name: 'Prefab', type: PinType.String }], dataOut: [{ name: 'Spawned', type: PinType.Entity }], param: 'none' },
    { id: NodeTypeId.SetActive, name: 'SetActive', category: 'Action', isEvent: false, isPure: false, execIn: 1, execOut: EXEC_OUT, dataIn: [{ name: 'Target', type: PinType.Entity }, { name: 'Active', type: PinType.Bool }], dataOut: [], param: 'none' },
    { id: NodeTypeId.PlaySound, name: 'PlaySound', category: 'Action', isEvent: false, isPure: false, execIn: 1, execOut: EXEC_OUT, dataIn: [{ name: 'Sound', type: PinType.String }], dataOut: [], param: 'none' },
];

export function node_info(id: NodeTypeId): NodeTypeInfo | undefined {
    return NODE_CATALOG.find((n) => n.id === id);
}

// ─── Authoring document (panel state) ───────────────────────────────────────
// `param` holds the raw inline value as authored. For LiteralString it is the
// string itself (interned into the pool at serialize time); for varSlot/int it
// is an integer; for float a number; for bool a 0/1.

export interface GraphNode {
    uid: number;          // panel-local unique id (NOT persisted)
    type: NodeTypeId;
    x: number;
    y: number;
    param: number | string | boolean;
}

export interface GraphEdge {
    uid: number;
    kind: typeof EDGE_EXEC | typeof EDGE_DATA;
    from_node: number;    // GraphNode.uid
    from_pin: number;
    to_node: number;      // GraphNode.uid
    to_pin: number;
}

export interface GraphDoc {
    nodes: GraphNode[];
    edges: GraphEdge[];
    variable_count: number;
    next_uid: number;
}

export function empty_graph(): GraphDoc {
    return { nodes: [], edges: [], variable_count: 0, next_uid: 1 };
}

// ─── Binary serializer — byte-for-byte mirror of Serialize.cpp ──────────────
// Header: magic 'PSYG' (0x47595350 LE), version 1, variable_count, node_count,
// edge_count, string_count. Then nodes (type u16, param_count u16, params u64*),
// edges (kind u8, from u32, to u32, from_pin u16, to_pin u16), string pool
// (len u32 + raw bytes).

const GRAPH_BLOB_MAGIC = 0x47595350; // 'P','S','Y','G' little-endian
const GRAPH_BLOB_VERSION = 1;

class ByteWriter {
    bytes: number[] = [];
    u8(v: number): void {
        this.bytes.push(v & 0xff);
    }
    u16(v: number): void {
        this.u8(v);
        this.u8(v >>> 8);
    }
    u32(v: number): void {
        this.u8(v);
        this.u8(v >>> 8);
        this.u8(v >>> 16);
        this.u8(v >>> 24);
    }
    // Write a 64-bit unsigned value supplied as raw little-endian bytes.
    u64_bytes(le: Uint8Array): void {
        for (let i = 0; i < 8; ++i) this.u8(i < le.length ? le[i] : 0);
    }
}

// Reinterpret a JS double as its IEEE-754 bit pattern (matches C++ float_bits +
// LiteralFloat param storage), returned little-endian.
function f64_le_bytes(value: number): Uint8Array {
    const buf = new ArrayBuffer(8);
    new DataView(buf).setFloat64(0, value, /*littleEndian*/ true);
    return new Uint8Array(buf);
}

// Encode a signed/unsigned integer (up to 53 bits exactly) as a little-endian
// u64. Matches the C++ i64/u64 reinterpret of Node::params for LiteralInt,
// LiteralBool, GetVar/SetVar slot, and LiteralString pool index.
function u64_le_bytes(value: number): Uint8Array {
    const out = new Uint8Array(8);
    let v = Math.trunc(value);
    // Two's-complement for negatives within the safe-integer range.
    if (v < 0) v = v + 2 ** 64;
    for (let i = 0; i < 8; ++i) {
        out[i] = v % 256;
        v = Math.floor(v / 256);
    }
    return out;
}

// Single inline param value -> the u64 bit payload the VM expects, per node type.
function node_param_bytes(node: GraphNode, intern: (s: string) => number): Uint8Array | null {
    const info = node_info(node.type);
    if (!info || info.param === 'none') return null;
    switch (info.param) {
        case 'float':
            return f64_le_bytes(typeof node.param === 'number' ? node.param : Number(node.param) || 0);
        case 'int':
        case 'varSlot':
            return u64_le_bytes(typeof node.param === 'number' ? node.param : parseInt(String(node.param), 10) || 0);
        case 'bool':
            return u64_le_bytes(node.param ? 1 : 0);
        case 'string':
            return u64_le_bytes(intern(String(node.param ?? '')));
        default:
            return null;
    }
}

export interface SerializeResult {
    bytes: Uint8Array;
}

// Serialize the document to the engine blob format. Node/edge order is the
// document order; uids are remapped to dense indices (matching the C++ Graph's
// index-based edges). String literals are interned into a deduplicated pool.
export function serialize_graph(doc: GraphDoc): SerializeResult {
    // Dense index per node uid (edges reference nodes by index in the blob).
    const index_of = new Map<number, number>();
    doc.nodes.forEach((n, i) => index_of.set(n.uid, i));

    // String pool (interned, deduplicated) for LiteralString params.
    const strings: string[] = [];
    const string_index = new Map<string, number>();
    const intern = (s: string): number => {
        const existing = string_index.get(s);
        if (existing !== undefined) return existing;
        const idx = strings.length;
        strings.push(s);
        string_index.set(s, idx);
        return idx;
    };

    // Pre-compute params (this interns strings before the header is written, so
    // string_count is final).
    const params_per_node = doc.nodes.map((n) => node_param_bytes(n, intern));

    // Drop any edge that references a deleted node BEFORE the header is written,
    // so the emitted edge_count is exactly the number of edge records that follow
    // (the panel keeps edges consistent, so this is just defensive — but a header
    // count that disagreed with the body would corrupt the blob the C++ reader
    // expects). Edges are remapped to dense node indices to match the C++ Graph.
    const live_edges = doc.edges.filter(
        (e) => index_of.has(e.from_node) && index_of.has(e.to_node),
    );

    const w = new ByteWriter();
    w.u32(GRAPH_BLOB_MAGIC);
    w.u32(GRAPH_BLOB_VERSION);
    w.u32(doc.variable_count >>> 0);
    w.u32(doc.nodes.length >>> 0);
    w.u32(live_edges.length >>> 0);
    w.u32(strings.length >>> 0);

    // Nodes.
    doc.nodes.forEach((n, i) => {
        w.u16(n.type);
        const param = params_per_node[i];
        w.u16(param ? 1 : 0);
        if (param) w.u64_bytes(param);
    });

    // Edges (dense indices; count matches the header).
    for (const e of live_edges) {
        // index_of.has() was checked above, so both lookups are defined.
        const from = index_of.get(e.from_node) as number;
        const to = index_of.get(e.to_node) as number;
        w.u8(e.kind);
        w.u32(from >>> 0);
        w.u32(to >>> 0);
        w.u16(e.from_pin);
        w.u16(e.to_pin);
    }

    // String pool.
    for (const s of strings) {
        const utf8 = new TextEncoder().encode(s);
        w.u32(utf8.length >>> 0);
        for (const c of utf8) w.u8(c);
    }

    return { bytes: new Uint8Array(w.bytes) };
}

// Lowercase hex encoding for the console-command transport (decode_hex_blob in
// player/main.cpp). Binary-safe: every byte becomes two hex digits.
export function to_hex(bytes: Uint8Array): string {
    let out = '';
    for (const b of bytes) out += b.toString(16).padStart(2, '0');
    return out;
}
