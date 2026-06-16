// SPDX-License-Identifier: MIT
// Psynder — PsyGraph graph serialization. Lane 15 owns.
//
// Little-endian, length-prefixed sections. See Serialize.h for the layout.
// The writer/reader are symmetric so a save/load/save cycle is byte-stable.

#include "Serialize.h"

#include "NodeTypes.h"

#include <string>
#include <utility>

namespace psynder::script::psygraph {

namespace {

void put_u8(std::vector<u8>& out, u8 v) { out.push_back(v); }

void put_u16(std::vector<u8>& out, u16 v) {
    out.push_back(static_cast<u8>(v & 0xFF));
    out.push_back(static_cast<u8>((v >> 8) & 0xFF));
}

void put_u32(std::vector<u8>& out, u32 v) {
    out.push_back(static_cast<u8>(v & 0xFF));
    out.push_back(static_cast<u8>((v >> 8) & 0xFF));
    out.push_back(static_cast<u8>((v >> 16) & 0xFF));
    out.push_back(static_cast<u8>((v >> 24) & 0xFF));
}

void put_u64(std::vector<u8>& out, u64 v) {
    for (int i = 0; i < 8; ++i)
        out.push_back(static_cast<u8>((v >> (8 * i)) & 0xFF));
}

struct Reader {
    std::span<const u8> data;
    usize pos = 0;
    bool ok = true;

    bool need(usize n) {
        if (pos + n > data.size()) {
            ok = false;
            return false;
        }
        return true;
    }
    u8 u8v() {
        if (!need(1))
            return 0;
        return data[pos++];
    }
    u16 u16v() {
        if (!need(2))
            return 0;
        u16 v = static_cast<u16>(static_cast<u16>(data[pos]) |
                                 static_cast<u16>(static_cast<u16>(data[pos + 1]) << 8));
        pos += 2;
        return v;
    }
    u32 u32v() {
        if (!need(4))
            return 0;
        u32 v = static_cast<u32>(data[pos]) | (static_cast<u32>(data[pos + 1]) << 8) |
                (static_cast<u32>(data[pos + 2]) << 16) | (static_cast<u32>(data[pos + 3]) << 24);
        pos += 4;
        return v;
    }
    u64 u64v() {
        if (!need(8))
            return 0;
        u64 v = 0;
        for (u32 i = 0; i < 8; ++i)
            v |= static_cast<u64>(data[pos + i]) << (8u * i);
        pos += 8;
        return v;
    }
};

}  // namespace

void serialize_graph(const Graph& graph, std::vector<u8>& out) {
    // Header.
    put_u32(out, kGraphBlobMagic);
    put_u32(out, kGraphBlobVersion);
    put_u32(out, graph.variable_count);
    put_u32(out, static_cast<u32>(graph.nodes.size()));
    put_u32(out, static_cast<u32>(graph.edges.size()));
    put_u32(out, static_cast<u32>(graph.strings.size()));

    // Nodes.
    for (const Node& n : graph.nodes) {
        put_u16(out, static_cast<u16>(n.type));
        put_u16(out, static_cast<u16>(n.params.size()));
        for (u64 p : n.params)
            put_u64(out, p);
    }

    // Edges.
    for (const Edge& e : graph.edges) {
        put_u8(out, static_cast<u8>(e.kind));
        put_u32(out, e.from_node);
        put_u32(out, e.to_node);
        put_u16(out, e.from_pin);
        put_u16(out, e.to_pin);
    }

    // String pool.
    for (const std::string& s : graph.strings) {
        put_u32(out, static_cast<u32>(s.size()));
        for (char c : s)
            out.push_back(static_cast<u8>(c));
    }
}

bool deserialize_graph(std::span<const u8> blob, Graph& out, std::string& error) {
    Reader r{blob};
    const u32 magic = r.u32v();
    if (!r.ok || magic != kGraphBlobMagic) {
        error = "bad magic";
        return false;
    }
    const u32 version = r.u32v();
    if (!r.ok || version != kGraphBlobVersion) {
        error = "unsupported version";
        return false;
    }

    out = Graph{};
    out.variable_count = r.u32v();
    const u32 node_count = r.u32v();
    const u32 edge_count = r.u32v();
    const u32 string_count = r.u32v();
    if (!r.ok) {
        error = "truncated header";
        return false;
    }

    out.nodes.reserve(node_count);
    for (u32 i = 0; i < node_count; ++i) {
        Node n;
        n.type = static_cast<NodeTypeId>(r.u16v());
        const u16 pc = r.u16v();
        n.params.reserve(pc);
        for (u16 p = 0; p < pc; ++p)
            n.params.push_back(r.u64v());
        if (!r.ok) {
            error = "truncated nodes";
            return false;
        }
        out.nodes.push_back(std::move(n));
    }

    out.edges.reserve(edge_count);
    for (u32 i = 0; i < edge_count; ++i) {
        Edge e;
        e.kind = static_cast<EdgeKind>(r.u8v());
        e.from_node = r.u32v();
        e.to_node = r.u32v();
        e.from_pin = r.u16v();
        e.to_pin = r.u16v();
        if (!r.ok) {
            error = "truncated edges";
            return false;
        }
        out.edges.push_back(e);
    }

    out.strings.reserve(string_count);
    for (u32 i = 0; i < string_count; ++i) {
        const u32 len = r.u32v();
        if (!r.need(len)) {
            error = "truncated strings";
            return false;
        }
        std::string s;
        s.resize(len);
        for (u32 k = 0; k < len; ++k)
            s[k] = static_cast<char>(r.u8v());
        out.strings.push_back(std::move(s));
    }

    error.clear();
    return true;
}

}  // namespace psynder::script::psygraph
