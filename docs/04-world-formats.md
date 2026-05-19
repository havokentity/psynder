# World formats — `.lmpak`, `.psybsp`, `.lmm`, `.lmt`, `.lma`

Psynder ships its own asset formats. All are little-endian, all are
designed for mmap-able zero-copy reads.

## `.lmpak` — archive

A package of cooked assets. FNV-1a-64 path hash index; optional zstd
per-entry compression.

```
struct LmpakHeader {
    char     magic[4];      // "LMPK"
    u32      version;       // currently 1
    u32      entry_count;
    u64      entry_table_offset;
    u64      name_table_offset;
    u64      payload_section_offset;
    u32      flags;
    u32      reserved;
};

struct LmpakEntry {
    u64      path_hash;     // FNV-1a-64 of the lowercased path
    u32      name_offset;   // into name_table
    u32      name_len;
    u64      payload_offset;
    u64      payload_compressed_size;
    u64      payload_uncompressed_size;
    u32      flags;          // bit 0 = zstd compressed
    u32      reserved;
};
```

Entry table is **sorted by `path_hash`** so the reader does a binary
search on lookup. The name table is a flat byte buffer of
NUL-terminated paths; entries reference into it by offset+length.

Path lookup is **case-insensitive** — the cooker lowercases before
hashing.

## `.psybsp` — BSP map

Produced by `lm_qbsp` from a `.map` brush list. Version 2 adds
portals.

```
struct PsybspHeader {
    char     magic[4];      // "PSBP"
    u32      version;       // 1 or 2
    u32      node_count;
    u32      leaf_count;
    u32      face_count;
    u32      vertex_count;
    u32      index_count;
    u32      pvs_byte_count;
    u32      portal_count;  // 0 in v1
    // ... chunk offsets follow
};
```

Six chunks: nodes / leaves / faces / vertices / indices / PVS bytes.
V2 adds a seventh: **portals** — 4-vertex square windings on splitter
planes connecting two non-solid leaves, for tighter cross-leaf culling.

PVS is a flat bit vector — one row per cluster, one bit per cluster.
"Cluster `i` can see cluster `j`" = `pvs[i * row_bytes + j/8] & (1 << (j%8))`.

## `.lmm` — mesh

Position + normal + UV + (optional) lightmap UV + (optional)
skinning weights/indices. SoA-friendly layout.

```
struct LmmHeader {
    char     magic[4];      // "LMM1"
    u32      version;
    u32      vertex_count;
    u32      index_count;
    u32      submesh_count;
    u32      attributes;     // bit-mask: POSITION | NORMAL | UV | LIGHTMAP_UV | SKIN
    u32      flags;
    // submesh table, vertex streams, index stream follow
};
```

Each submesh has a material id (resolved at load time against the
asset catalog) and a (first_index, index_count) range.

## `.lmt` — texture

Paletted / 16-bit / 32-bit with optional mipchain. The texture is
otherwise just a flat byte array per mip.

```
struct LmtHeader {
    char     magic[4];      // "LMT1"
    u32      version;
    u32      width;
    u32      height;
    u32      format;         // PALETTED8 | RGB565 | RGBA4444 | RGBA8 | BC1 | BC3
    u32      mip_count;
    u32      palette_offset; // if PALETTED8
    u32      data_offset;
    u32      flags;
};
```

Mipchains use a Kaiser filter generated offline. Each mip's offset
is computed from the previous mip size — no per-mip header.

## `.lma` — audio

Mono / stereo, 16-bit PCM or float, optional zstd compression.

```
struct LmaHeader {
    char     magic[4];      // "LMA1"
    u32      version;
    u32      sample_rate;
    u32      channels;
    u32      sample_format;  // PCM16 | F32
    u32      frame_count;
    u32      data_offset;
    u32      flags;          // bit 0 = zstd compressed
};
```

## `.psylevel` — editor level save

Binary, zstd-compressed; chunks for entities + brushes + heightmap +
splat weights + constraints + lights. Saved by `editor::save_level`,
loaded by `editor::load_level` or at game start.

## `.psyc` — contraption

Subset of `.psylevel` — entity graph + constraint set, no terrain.
Used by the sandbox-mode physgun for "save this thing I built" flow.

## Cooking + packing pipeline

```
my_textures/*.png  ──┐
my_meshes/*.obj    ──┼──► lm_cook ──► *.lmt / *.lmm / *.lma
my_audio/*.wav     ──┘                           │
                                                 ▼
                                              lm_pak ──► demo.lmpak
                                                 │
                                                 ▼
                                              Vfs::mount_pak
```

At runtime the engine mounts one or more `.lmpak` archives via
`psynder::asset::Vfs::Get().mount_pak(path)`. Multiple mounts are
searched in reverse order (later mounts shadow earlier) for the
developer-override pattern.

Sample of mount + read:

```cpp
auto& vfs = psynder::asset::Vfs::Get();
vfs.mount_pak("demo.lmpak");

psynder::asset::Blob crate = vfs.read("textures/crate.lmt");
// crate.data is an mmap view into the archive; no copy.
```

## Hot reload

Dev builds watch source files via stat-polling at ~4 Hz. The watcher
fires `on_changed(virtual_path)` callbacks between frames. Subsystems
that opt in (rmlui documents, lua scripts) re-parse atomically. See
`engine/asset/Vfs.h::watch`.

## Determinism

Asset files are **byte-stable** for a given input + cooker version.
We rely on this for golden-image testing: the cooker's per-texel
jitter is deterministic (no shared RNG state), the lightmap bake uses
stratified samples from PCG seeded per-lumel, and the BSP compiler
sorts faces by hash so the leaf ordering is stable across runs.
