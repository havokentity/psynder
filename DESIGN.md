# Psynder — Cache, Scanline, Renderer (PCSR)

> *"Everything the GPU can do, the CPU can do — just lovingly, in cache, with SIMD."*

**Name:** **Psynder** — short for **P**synder **C**ache **S**canline **R**enderer (PCSR). The expanded form encodes three of the engine's defining ideas: the stylized retro-software-rendering identity (*Psynder*), the obsessive cache-coherency that runs through every subsystem (*Cache*), and the tiled scanline rasterizer at its heart (*Scanline Renderer*).

The **C** in PCSR is the spirit of the engine: Psynder is **cache-coherent and data-oriented where it matters**. Hot subsystems (renderer, ECS, physics, audio mixer) are strict DOTS because we are crazy-high-perf. The rest of the engine — tools, editors, loaders, platform shims — uses whatever style is clearest, including normal OOP. The **public game/script API** users build on is strict DOTS by design: the API is shaped so users *cannot* write per-entity OOP code even if they try. See §3 for the full split.

**Status:** Design doc, **v1.0 — handoff** (2026-05-19). All architectural decisions called; ready for implementation kickoff. See §19 for kickoff notes.
**License:** MIT (engine code and tools). Assets under CC-BY-4.0.
**Language:** C++23
**Targets:** Windows (x86-64), Linux (x86-64), macOS (Apple Silicon, arm64)
**Borrowed code:** Only the RNG and a couple of conventions are reused from the author's existing **dmonte path tracer**. dmonte's runtime RT is built on Intel Embree (which we explicitly do *not* take a dependency on — see ADR-007 in §16), so the BVH8, intersect kernels, refit, denoiser, and tile-pipeline integration are all written from scratch for Psynder. We are building a *software raytracer for shadows and a few bounces*, not a path tracer.

---

## 1. Vision

Psynder is an **open-source, fully CPU-driven game engine and software renderer** that recaptures the feel of the late-90s / early-2000s software-rendered era — the worlds of *Quake II*'s software path, *NFS II SE / III: Hot Pursuit / IV: High Stakes*, and *Project IGI* / *Delta Force*-style large-map tactical FPS — and pushes them forward with everything modern CPUs allow: wide SIMD, dozens of threads, gigabytes of RAM, and *real* raytraced lighting on top of a perspective-correct rasterizer.

There is **zero GPU code**. No D3D, no Vulkan, no Metal, no OpenGL. The graphics API does exactly one thing: hand us a window and a framebuffer. Every pixel is computed by the CPU.

### Design pillars

1. **Pure CPU.** The renderer never depends on a GPU API. The framebuffer is presented via the cheapest possible blit (DXGI flip / X11 SHM / Wayland dmabuf / `IOSurface` on macOS).
2. **Hardcore performance.** C++23 with explicit SIMD (SSE2/AVX2/AVX-512 on x86-64, NEON on Apple Silicon), data-oriented design, lock-free job system, cache-line-aware data layouts. Goal: 1080p60 on a modern desktop with full features.
3. **Authentic + modern.** The look-and-feel of software-era games (affine option, dithered fog, paletted textures, scanline artifacts as opt-ins) **plus** mipmapping, bilinear/trilinear/anisotropic filtering, baked radiosity lightmaps, and **raytraced dynamic shadows + point lights** — a tight, real-time-tuned software raytracer, not a path tracer.
4. **Generic enough for the games we care about.** One engine, two genre presets — indoor FPS (Quake-style BSP) and outdoor heightmap (NFS-style racing tracks *and* Project IGI / Delta Force-style large tactical-FPS maps). Both share the renderer, scene graph, and job system.
5. **Hackable & teachable.** Every subsystem documented, every magic constant explained. The codebase reads as well as it runs.
6. **Three platforms, first-class.** Windows / Linux / macOS Apple Silicon all build from one source. Apple Silicon is *not* an afterthought — NEON paths are hand-written.

### Non-goals

- **No GPU fallback.** Ever. Not even "optional Vulkan."
- **No browser/WASM target in v1.** WASM SIMD is interesting but blocks threading + intrinsics work; revisit post-1.0.
- **No mobile.** Battery and thermal envelopes don't match the design.
- **No photorealism arms race.** Target is "1999 with raytracing," not Cyberpunk.
- **No streamed open world.** Maps are level-scope, loaded as a unit. No GTA-style city streaming.
- **No planet-scale terrain.** Maps are flat (Mercator-flat, not curved-earth). Capped at 16 km × 16 km.
- **No full flight simulation.** Aircraft (helicopters, planes) exist as kinematic / scripted movers, not as flyable physics.
- **No dynamic resolution scaling.** Internal render resolution is fixed at startup; window resizing scales the blit, never the framebuffer. See §7.9.

### Reference games (the looks we're chasing)

| Game | Year | Genre | What we steal |
|---|---|---|---|
| *Quake II* (software) | 1997 | Indoor FPS | BSP/PVS, lightmaps, surface caching, palette dither |
| *Unreal* (software) | 1998 | FPS | Volumetric fog, colored lighting |
| *NFS II SE* | 1997 | Arcade racer | Sprite-card crowds, environment LOD, road segments |
| *NFS III: Hot Pursuit* | 1998 | Arcade racer | Env-mapped car bodies, weather |
| *NFS IV: High Stakes* | 1999 | Arcade racer | Damage models, dynamic headlights at night, rain |
| *Thief: The Dark Project* | 1998 | Stealth FPS | Light/dark gameplay → real shadow rays |
| *Project IGI* | 2000 | Tactical FPS | Kilometre-scale outdoor maps, heightmap terrain, long view distances |
| *Delta Force (1 / 2)* | 1998/99 | Tactical FPS | Vast outdoor heightmap landscapes, voxel-influenced terrain, scattered objectives |
| *Outcast* | 1999 | Adventure | Voxel terrain — inspiration for an optional voxel mode |

---

## 2. Technology choices

### 2.1 Language: C++23

A tight, modern subset:

- **Yes:** `constexpr`, concepts, `std::span`, `std::expected`, `std::flat_map`, `if consteval`, `[[likely]]`/`[[unlikely]]`, `<bit>`, eventually `std::simd`.
- **No (in hot paths):** exceptions, RTTI, virtual calls in inner loops, `std::shared_ptr` in renderer code, hidden allocations in the frame loop.
- **Compilers:** Clang ≥ 17 (primary on all three platforms), GCC ≥ 13 (Linux secondary), MSVC 19.40+ (Windows secondary). Apple Silicon uses Apple Clang; Homebrew LLVM for tuning experiments.

### 2.2 Build system

- **CMake ≥ 3.28** with presets per (OS × arch × config).
- **Ninja** generator.
- **vcpkg** manifest mode for third-party deps; everything else vendored under `third_party/`.
- **clang-format** + **clang-tidy** + **include-what-you-use** in CI.

### 2.3 SIMD strategy

A thin abstraction `simd::f32x4`, `simd::f32x8`, `simd::i32x8`, `simd::mask8` etc., mapping to:

- **x86-64 baseline:** SSE4.2.
- **x86-64 fast path:** AVX2 + FMA (runtime dispatch via `Sys::CpuFeatures`).
- **x86-64 wide path:** AVX-512F/BW/VL — used opportunistically in rasterizer cover and BVH packet traversal.
- **Apple Silicon:** ARM NEON (128-bit). We do *not* fake 256-bit via two NEONs in hot loops — we re-tile to 4-wide on arm64.
- One binary, per-TU `__attribute__((target("avx2")))`, dispatcher picks the widest available kernel at startup.

### 2.4 Threading & parallelism — abuse every core

Psynder is **embarrassingly parallel by construction**. The single-thread fast path exists for debugging; everything else assumes N cores and feeds them.

- **Job system:** Lock-free work-stealing deque per worker (Chase-Lev), one worker per physical core (logical pinning opt-in), main thread reserved for OS/input. Job submission is wait-free; work-stealing handles imbalance automatically.
- **Job graph, not job queue.** Frames are described as a DAG of jobs with explicit dependencies. The scheduler walks the DAG, dispatches ready nodes, and reuses node memory across frames. Triple-buffered so frame N+1 setup overlaps frame N rendering and frame N-1 present.
- **Fibers** (Windows fibers / ucontext on POSIX) for cooperative long-running tasks (lightmap bake, BVH refit) — they can yield mid-bounce without stalling a worker.
- **Per-subsystem parallelism (all of these run on the job pool, not threads-of-their-own):**

| Subsystem | Parallel grain | Per-frame jobs (typical, 16-core box) |
|---|---|---|
| Vertex transform | 256 verts / job | 100–500 |
| Frustum/PVS cull | 1 partition / job | 16–64 |
| Triangle setup / bin | 1k tris / job | 50–200 |
| Tile rasterize+shade | 1 64×64 tile / job | 510 @ 1080p |
| BVH refit (dynamic) | 1 BLAS / job | 1 per dynamic mesh |
| Raytrace shadow rays | 1 tile / job (fused with shade) | 510 |
| Denoise | 1 tile / job | 510 |
| Physics broadphase | 1 SAP axis-pass / job | 3 |
| Physics narrowphase | 1 island / job | 50–500 |
| Physics solver | 1 island / job (Gauss-Seidel inside) | 50–500 |
| Audio mixer | 1 voice / job, sum SIMD-merged | 32 |
| AI (peds/traffic) | 1 cell / job | 16–256 |
| Asset stream / decompress | 1 file / job | as needed |
| Lightmap bake (offline) | 1 lumel batch / job | thousands |

- **Data layout supports parallelism.** All hot data is SoA, cache-line aligned, partitioned by spatial/island boundaries so jobs never write the same cache line. Where a sum is required (audio mix, lighting accumulation), we use lane-aligned reductions, not locks.
- **Zero global mutexes in the frame loop.** Allocations on the hot path go through per-worker linear arenas reset at frame end. Logging is lock-free (per-worker ring → main-thread drainer).
- **Heterogeneous cores:** on Apple Silicon and Intel Core Ultra we group P-cores and E-cores into separate worker pools; latency-sensitive jobs (raster, raytrace) prefer P-cores, throughput jobs (asset decompress, audio) are happy on E-cores.
- **Scaling target:** linear speedup to 16 cores, ≥ 80% efficiency to 32 cores, ≥ 60% efficiency to 64 cores. CI runs the benchmark suite on 4, 8, and 16-core machines and graphs the regression.

---

## 3. Architecture mandate — DOTS for users, pragmatic-DOTS for the engine

Psynder lives at two altitudes:

- **The engine itself** (this repo) — written by us, for absolute performance. We use **DOTS as far as possible** because we are *crazy high perf*, but we are not religious about it. OOP is welcome where it doesn't hurt the cache, and the hot paths are non-negotiably data-oriented because that's where the perf budget actually lives.
- **The user-facing API** — game devs, scripters, and modders who build on Psynder — get a **strict DOTS contract**. Their code drives our hot loops, so we shape the API so the only way to write fast code is also the easy way, and the OOP-style "tick every object" path simply doesn't exist for them to reach for.

This split is the whole reason the engine can be both fast *and* pleasant to extend: we eat the discipline cost in the parts that matter, and elsewhere we write whatever is clearest.

### 3.1 What DOTS means here

> *Organize code around how data is laid out in memory and how it flows through cache lines, not around taxonomies of "objects."*

Concretely:

- **Entities are integer handles** — `u32` IDs into component arrays. No `class Entity` with virtual methods.
- **Components are POD** — trivially copyable, no constructors that do work, no destructors with logic.
- **Storage is SoA arrays partitioned into archetypes** — the set of component types an entity has — laid out in cache-line-aligned chunks.
- **Logic lives in systems** — free functions (or stateless structs with `run()`) that take component arrays and operate on them in bulk. Systems run as jobs.
- **No virtual dispatch on the hot path** — polymorphism is data-driven (tag enums + table dispatch, `if constexpr`), not v-tables.
- **No hidden allocations per frame** — explicit allocators, frame arenas, ECS-owned long-lived storage.

### 3.2 Inside the engine — pragmatic-DOTS

What we **do** require of ourselves:

- **Hot subsystems are strict DOTS:** `engine/render/`, `engine/scene/ecs/`, `engine/physics/`, `engine/audio/mixer/`. No `virtual` in their hot loops, POD components, SoA layout, no `shared_ptr`, no per-frame heap allocation. The bench gate enforces this — regress a hot loop and CI says no.
- **Hot loops are SoA, branch-free, SIMD-friendly.** This is the rule, full stop.

What we **allow** ourselves, freely, where it doesn't hurt the cache:

- **Real classes with constructors, destructors, and RAII** in offline tools (`lm_cook`, `lm_bake`, `lm_qbsp`, `lm_pak`, `lm_mapimport`), platform shims (`engine/platform/`), asset loaders, the network protocol layer, the file-format parsers, the Lua binding glue, and the editor IPC server (`engine/editor/ipc/`). (The in-engine editor *core* in `engine/editor/core/` follows the DOTS rules of the engine proper since it manipulates the live ECS. The React/TypeScript code in `engine/editor/web/` is outside the C++ DOTS contract entirely — it's a separate codebase that talks to the engine over IPC.)
- **`virtual` for plugin-style extension points** — asset-loader registry, platform backends, editor tools. These dispatch once per asset / once at startup, not once per pixel.
- **`std::unique_ptr`** anywhere for clear ownership; **`std::shared_ptr`** only in tools and the editor (still banned in the engine runtime).
- **STL containers** where appropriate outside hot paths.
- **Inheritance** where it actually models the problem (e.g., a `AssetLoader` base with `PngLoader`/`WavLoader`/`GltfLoader` impls).
- **Service-locator globals** for a small named list — `EcsRegistry*`, `JobSystem*`, `Allocator*`, `Log*`, `Clock*`, `Config*` — grabbed once at frame start. New singletons require an ADR.

The principle is: **pay the DOTS tax where it earns its keep, write normal C++ everywhere else.** A two-line helper class for `ScopedTimer` is fine. A `class Entity { virtual void Tick(); }` in the renderer is not.

### 3.3 Outside the engine — strict DOTS for users

When a game developer, scripter, or modder builds on Psynder, the contract is strict, and the API is shaped to make it the path of least resistance:

- **No `Entity:tick()` style.** The C++ and Lua APIs both expose **systems and queries**, not entity objects with methods. There is no `entity.update()` to override because there is no `entity` object.
- **Components are POD structs.** Users declare them with `PSYNDER_COMPONENT(Position) { float x, y, z; };` — a macro that registers the type and asserts triviality at compile time.
- **Systems are functions.** Users register them with read/write component sets: `world.register_system<reads<Position>, writes<Velocity>>(integrate_motion);`. The scheduler uses the declared access to parallelize automatically.
- **Modders can ship components and systems, not classes.** The plugin ABI has slots for components and systems; there is no slot for "a new GameObject subclass."
- **The Lua binding mirrors this exactly:** `world:register_system({reads={Position}, writes={Velocity}}, function(positions, velocities) ... end)`. Lua scripts run on a dedicated game thread; engine workers never enter the VM.

The result: users *cannot* write code that ticks one entity at a time. They get cache-coherent batch processing whether they understand DOTS or not.

### 3.4 Forbidden patterns (anywhere)

These trip clang-tidy and fail CI everywhere in the repo, engine internals included:

| Pattern | Why | Replacement |
|---|---|---|
| `virtual` inside `render/`, `scene/ecs/`, `physics/`, `audio/mixer/` hot paths | Indirect calls in inner loops | Tag-dispatch, `if constexpr`, templates |
| `std::shared_ptr` in `engine/` runtime (tools/editor OK) | Atomic refcount, hidden lifetime | ECS handle or `unique_ptr` |
| Per-frame `new` / `delete` outside `engine/core/alloc/` | Hides allocation cost | Frame arenas, pools |
| Per-entity update methods exposed to users | One indirect call per entity | System over component array |
| New singletons not on the allow-list | Hidden globals, hidden lifetime | Add an ADR, or pass explicitly |

### 3.5 The ECS in one paragraph

Psynder's ECS (`engine/scene/ecs`) is an **archetype-chunked** ECS in the Unity DOTS / EnTT / flecs lineage. Chunks are 16 KB, sized to fit comfortably in L1 with room for system locals. Component types are registered at startup; archetypes are derived. Queries cache their matched archetype list and only re-walk when structural changes occur. Adding/removing components on an entity moves it between archetypes; this is the only operation with non-trivial cost, and structural changes are batched to frame boundaries by default.

### 3.6 Enforcement

- **clang-tidy** with a custom check set: bans `virtual` in flagged directories, bans `std::shared_ptr` in `engine/` runtime, warns on `new`/`delete` outside `engine/core/alloc/`.
- **Code review checklist:** every PR touching `render/`, `scene/ecs/`, `physics/`, `audio/mixer/` answers "is the hot loop branch-free and SoA?"
- **Bench gate:** any PR that regresses the per-tile rasterize or per-island physics benchmark by more than 2% must justify it in the PR or fail CI.
- **API shape gate:** any change to the public game/script API is reviewed against "can a user write per-entity OOP code with this?" If yes, the API change is rejected.
- **Docs:** `docs/00-dots-primer.md` for users learning the model; `docs/00-engine-style.md` for engine contributors covering the internal pragmatic-DOTS rules.

---

## 4. Memory management — `psynder_mem`, cache-coherent everywhere

Memory is the bottleneck. CPUs are fast; DRAM is roughly 100× slower; the cache hierarchy is the bridge. Every Psynder allocation strategy is in service of one thing: **keep the hot working set in L1/L2, keep cores from fighting over the same cache lines, and make every byte we touch one the prefetcher already moved for us.**

Psynder ships its own memory subsystem (`engine/core/alloc`, namespace `psynder::mem`). It is opinionated, lifetime-scoped, NUMA-aware, hugepage-friendly, and instrumented end-to-end.

### 4.1 Principles

- **Zero `malloc` / `new` in the frame loop.** Every byte allocated during a frame comes from an allocator whose memory was reserved up front. The runtime allocator footprint is bounded and known.
- **Every allocation has a tagged lifetime scope.** App, level, frame, job, or named scope. The scope determines which allocator services it, and the allocator's reset cost is O(1).
- **Per-worker first, shared last.** Each worker thread owns its own arenas, pools, and scratch buffers. Cross-thread allocation goes through lock-free queues only when necessary.
- **Aligned to the hardware.** 64-byte cache lines on x86 and Apple Silicon, 4 KB OS pages, 2 MB hugepages where the OS lets us.
- **Owners are explicit.** No `shared_ptr` on the hot path. Lifetimes are encoded in the allocator, not in the type.

### 4.2 Allocator hierarchy

| Allocator | Lifetime | Reset cost | Thread-safety | Used by |
|---|---|---|---|---|
| `LinearArena` | Per-frame, per-job, per-scope | O(1) — bump the pointer | One instance per worker | Frame scratch, vertex transforms, draw commands, BVH refit working set |
| `StackArena` | Scope (LIFO) | O(1) | Single-threaded inside one job | Recursion stacks, narrow temporaries |
| `TypedPool<T>` | App or level | O(1) free per element | Per-worker free-list + lock-free overflow queue | ECS chunks, audio voices, physics bodies, contact points |
| `BuddyAllocator` | App or level | O(log n) | Mutex (cold path only) | Variable-size persistent blobs: lightmap atlas pages, deeper terrain LODs, BVH nodes |
| `PageAllocator` | App | OS-mediated | OS-level | Backing for everything above — direct `mmap` / `VirtualAlloc` with hugepage hints |
| `TLSFAllocator` (mimalloc-backed) | App or tools | O(1) typical | Lock-free | Tool-side, editor, asset cooker, anywhere outside the engine runtime |

The runtime never asks the OS for memory at frame time — page allocations happen at level load or startup. Frame-local data lives in linear arenas that are **reset, not freed**, at the frame boundary.

### 4.3 Cache coherency strategy

- **64-byte cache-line alignment** on every type that participates in a hot loop. The build asserts at compile time (`static_assert(alignof(T) % 64 == 0)`) on components iterated by the renderer or physics solver.
- **No false sharing.** Atomics that different workers hit are padded to their own cache line. Per-worker counters, queues, and arenas are aligned and padded.
- **2 MB hugepages** for large arenas:
  - **Linux:** `madvise(MADV_HUGEPAGE)` on the arena backing region; transparent hugepages opt-in by default.
  - **Windows:** `VirtualAlloc` with `MEM_LARGE_PAGES` (requires the `SeLockMemoryPrivilege`, prompted at install).
  - **macOS Apple Silicon:** `VM_FLAGS_SUPERPAGE_SIZE_2MB` via `mach_vm_allocate`. The M-series TLB benefits a lot from this.
- **NUMA awareness** on multi-socket systems: each worker pool is bound to one NUMA node and allocates from that node's pages. Cross-node references are rare and flagged by perf counters.
- **Software prefetch** in tight loops: `__builtin_prefetch` / `_mm_prefetch` inserted manually in BVH traversal, ECS iteration, raster gradient evaluation, and physics narrowphase. Distance hand-tuned per platform.
- **Streaming stores** (`_mm_stream_*` on x86, `stnp` on arm64) when writing the framebuffer and other write-once-read-elsewhere destinations — bypassing the cache to avoid polluting it with data we won't read back.
- **Read-only data is shared** across workers and marked `const`; writable per-worker data lives in per-worker arenas so coherency traffic stays local.

### 4.4 ECS chunk layout

The ECS storage layer is the single largest consumer of memory and the highest-bandwidth reader. Chunk design:

- **16 KB chunks**, sized to fit comfortably in L1 (typical L1D is 32–48 KB) with room for system locals and prefetched-ahead chunks.
- **Page-aligned**, allocated from a `TypedPool<Chunk>` backed by hugepages.
- **SoA columns per component**, each column 64-byte aligned, contiguous within the chunk.
- **Chunk header (one cache line)** holds: archetype id, row count, version stamps per column, per-chunk dirty mask.
- **Iteration is column-at-a-time** — a system reading `Position` and writing `Velocity` walks two contiguous arrays per chunk; the prefetcher loves it.
- **Structural changes batch to frame boundaries** by default, so chunks don't shuffle under iterating systems.

### 4.5 Lifetimes and scopes

The allocator stack is explicit:

```
App scope            — global subsystems, asset cache, ECS world
 └── Level scope     — streamed/loaded content, lightmaps, BVH
      └── Frame scope         — per-frame scratch, draw cmds, transient
           └── Job scope      — per-job scratch (one arena per worker)
                └── Scoped    — RAII scope guards inside a job (LIFO)
```

A system asks for a `FrameAllocator&` or `JobAllocator&` and must declare it in its system signature. Allocating from the wrong scope (frame data from an app-scope path, or vice versa) is a clang-tidy lint error.

### 4.6 Streaming and async I/O

Psynder maps are **level-scope, loaded as a unit** — no GTA-style cell streaming, no planet-scale tile pyramids. What we do stream:

- **Asset loads at level transition.** Async I/O worker reads compressed `.lmpak` entries into a **staging arena**, decompresses on a job, then **atomically publishes** the result. The level-load screen masks the cost.
- **Deeper terrain LODs on demand.** When the player approaches a distant region in a large outdoor map (IGI / Delta Force scale), the next LOD level for that heightmap chunk is fetched / generated from the level-scope buddy allocator on a worker. The current frame keeps drawing the coarser LOD; the finer one swaps in atomically when ready.
- **Hot-reload in dev builds.** The asset VFS watches source files; the engine swaps assets atomically between frames.

No mid-frame allocations from the OS, no cell-eviction housekeeping in the frame loop.

### 4.7 Tracking, budgets, and debugging

- **Every allocation is tagged** with a category: `render`, `physics`, `audio`, `ecs`, `asset`, `streaming`, `scripts`, `tools`, `misc`. The tag is part of the allocator API; you cannot allocate untagged.
- **Per-category budgets** are configured per build target. Crossing a budget logs a warning at frame end; crossing 1.5× the budget fails the bench in CI.
- **Live heatmap** in the editor: bar chart of category × current/peak/budget, updated each frame.
- **Allocation flight recorder** (debug builds): every alloc/free recorded with a small stack hash; leak detection at scope exit prints the offending site.
- **Per-frame memory map dump** on demand (e.g., `F8` in dev builds) — emits JSON of arena watermarks for offline analysis.
- **AddressSanitizer / Valgrind / Apple's Guard Malloc** all run clean in CI; an ASan-instrumented build is part of the matrix.
- **Lockstep determinism check:** allocator behavior is deterministic for a given seed and input stream, so multiplayer lockstep divergences are never allocator-induced.

### 4.8 Why a custom allocator (and where mimalloc fits)

General-purpose allocators (jemalloc, mimalloc, tcmalloc) are excellent — we use **mimalloc** as the backing for tools, the editor, and the `TLSFAllocator` slot. They are *not* what the engine runtime needs because:

1. They optimize for general-purpose `malloc`/`free` patterns — Psynder's frame loop has zero free calls.
2. Lifetime-scoped arenas reset in O(1); a general allocator can't beat that.
3. We want explicit, named scopes for budgeting and debugging; a general heap flattens everything together.
4. NUMA-binding and hugepage placement are first-class in our hierarchy, opt-in or absent in theirs.

`psynder::mem` is the engine runtime's contract with the cache hierarchy. mimalloc handles the rest.

---

## 5. Relationship to the dmonte path tracer

Psynder is a **software raytracer**, not a path tracer — we trace a small, bounded number of rays per pixel for shadows and (offline) lightmap bakes. We don't need most of what a path tracer does.

dmonte's runtime raytracer is built on **Intel Embree**, which is a hard dependency to ship (long build, opaque CMake, weak Apple Silicon NEON support, conflicts with our DOTS / own-allocator stance). So we can't directly lift dmonte's BVH or intersect kernels — those are Embree calls under the hood. We write our own raytracing core from scratch (see ADR-007 in §16).

What's portable from dmonte is small:

| Borrowed | Use in Psynder | Why |
|---|---|---|
| RNG (PCG) | Stratified sample seeds for shadow rays + bake | Small, standalone, deterministic, well-understood |
| Material / BRDF conventions | Inspires the lightmap baker's `psynder_bake` BRDF set | Author's prior tuning, useful starting point |
| Scene description conventions | Reference for how we lay out scenes for the optional `dmonte_pt` tool | Not on the engine's critical path |

The runtime raytracing core (BVH8 build, SAH heuristic, 8-wide packet traversal, refit, denoiser, tile-pipeline integration, dynamic-light tile list) is **all new code**, written for real-time CPU use and informed by the open literature (Wald, Havran, Hapala/Havran, pbrt-v4, the original Larrabee papers).

Optionally, `tools/dmonte_pt/` ships as a standalone reference renderer for **golden-image tests** and as a curiosity for offline stills. It still uses Embree internally; the engine does not. If `dmonte_pt` bit-rots we cut it without touching the engine.

---

## 6. Architecture overview

```
+--------------------------------------------------------+
|                       Game layer                       |
|   (indoor FPS rules, racing rules, outdoor FPS AI)     |
+----------------+----------------+----------------------+
                 |                |
+----------------v---+  +---------v------------------------+
|  Scene / ECS       |  |   Asset / VFS / Streaming        |
|  (entities, world) |  |   (.lmpak archives, tile cache)  |
+----------------+---+  +----+-----------------------------+
                 |           |
+----------------v-----------v------------------------+
|                Renderer (CPU only)                  |
|  +-----------+ +---------+ +---------+ +---------+  |
|  | Vertex    | | Bin/    | | Tile    | | Resolve |  |
|  | pipeline  | | Cull    | | raster  | | + post  |  |
|  +-----------+ +---------+ +---------+ +---------+  |
|         ^             ^         ^           ^       |
|  +------+-----+ +-----+-----+ +-+--------+  |       |
|  | Lightmap   | | Software  | | Texture  |  |       |
|  | atlas      | | RT core   | | sampler  |  |       |
|  +------------+ +-----------+ +----------+  |       |
+---------------------------------------------+-------+
                                              |
+---------------------------------------------v-------+
|     Platform abstraction (Win32/Wayland/Cocoa)      |
|     Window, framebuffer present, input, audio       |
+-----------------------------------------------------+
```

Engine code is small static libraries depending strictly upward: `lm_core` → `lm_math` → `lm_simd` → `lm_jobs` → `lm_asset` → `lm_scene` → `lm_render` → `lm_audio` → `lm_game`. Tools live under `tools/` and link the same libraries.

---

## 7. The renderer

A **tiled sort-middle rasterizer with a separate hybrid lighting stage**.

### 7.1 Pipeline

```
For each frame:
  1. Cull (frustum, PVS indoor, distance/tile bands outdoor, occluder volumes for dense urban-ish indoor-outdoor blends)
  2. Vertex shade (transform + light eval) in SIMD lanes
  3. Triangle setup (edge equations, gradients) -> draw cmd
  4. Binning: each tri assigned to overlapping 64x64 tiles
  5. Per-tile rasterize (parallel): coverage, depth, attribute interp
  6. Per-tile shade: texture sample (filtered), lightmap, fog, fx
  7. Hybrid pass: trace dynamic shadow rays per pixel-light (dmonte core)
  8. Resolve: tile -> backbuffer, tone map / dither
  9. Post: bloom (separable), motion blur (optional), scanline filter
 10. Present (platform layer)
```

Steps 4–7 fuse into one job per tile so the framebuffer slice stays in L2.

### 7.2 Vertex pipeline

- **SoA** scratch buffers (positions, normals, UVs, lightmap UVs, color separate streams). On disk AoS — we transpose at load.
- Transforms run 8 vertices at a time (AVX2) or 4 (NEON / SSE).
- **Hybrid skinning** for vehicles and aircraft (see ADR-003 in §16):
  - **Chassis (and helicopter / plane body for scripted aircraft):** skinned to a per-vehicle skeleton — the root bone is driven by the physics rigid body, and additional bones handle cosmetic deformation (damage poses, hood crumple, panel flex, body roll on suspension).
  - **Wheels, hubcaps, spoilers, mirrors, antennas, rotor blades, control surfaces (cosmetic only — aircraft are kinematic, not flight-simmed):** rendered as rigid meshes parented to skeleton bones. Vehicle wheels read their transforms directly from the physics solver (each wheel is a constrained rigid body in `psynder_phys`, one-to-one with the rendered mesh). Aircraft surfaces are driven by script / animation.
  - **Severe damage:** a rigid sub-mesh (door, bumper) detaches and becomes its own standalone physics rigid body, flying off with its own rigid mesh.
  - **Drivers:** standard skinned character pipeline with their own skeleton.
- LBS (linear blend skinning) is the default; DQS (dual-quaternion) available per-material where candy-wrapper artifacts show. FPS characters use the same pipeline.

### 7.3 Triangle setup & binning

- Edge functions in fixed-point (Q24.8) for exact coverage, sub-pixel precision 1/256 px.
- A draw command carries 3 transformed verts, gradient pack, material id, lightmap id, flags — 128 bytes, cache-line aligned.
- 64×64 bins by default (see ADR-002 in §16). The rasterizer is **templated over `<TILE_W, TILE_H>`** and ships specializations for 32×32, 64×64, and 128×128 — the compiler bakes tile dimensions as constants into the inner loop, so there's zero runtime cost vs hard-coding. Tile size is selectable via `r_tile_size` and changeable mid-run with the same machinery as a resolution change (drain in-flight frames, reallocate the tile pool, swap the function pointer, resume).
- Morton-ordered tile-to-worker assignment for neighborhood coherence.
- Hierarchical Z (HiZ) buffer at 8×8 per tile for early reject.

### 7.4 Rasterization

- **Coverage:** 2×2 quad rasterizer (mip LOD finite-diffs free), edge-equation walking inside the tile. Optional 4×4 SIMD coverage on AVX-512.
- **Depth:** 24-bit float Z + 8-bit stencil, early-Z when alpha test is off.
- **Attributes:** perspective-correct via `1/w`, `u/w`, `v/w` linearly interpolated then divided at the quad. *Affine* mode opt-in for retro.
- **Sub-texel precision:** 1/256 — kills the texture swimming common in old renderers.

### 7.5 Texture sampling — full feature set

| Filter | Cost | Notes |
|---|---|---|
| Nearest (point) | 1 fetch | The retro path |
| Bilinear | 4 fetches + 3 lerps | Default for color/lightmap |
| Trilinear | 8 fetches | Across mip levels |
| Anisotropic 2×/4×/8×/16× | up to N × bilinear | EWA approximation |
| Bicubic (UI) | 16 fetches | For HUD upscales |

- **Mipmaps** generated offline (Kaiser).
- **Texture format:** custom `.lmt` container. Paletted (8 bpp + palette), 16-bit (RGB565 / RGBA4444), 32-bit (RGBA8). Optional BC1/BC3-style block compression decoded into a SLAB cache.
- Clamp/wrap/mirror modes; **sRGB-correct** sampling default.
- The pixel-shading inner loop is **on-the-fly** by default (sample texture, sample lightmap, multiply). A second kernel — the classic Quake-style surface cache — is applied automatically per surface when eligible. See §7.6.

### 7.6 Surface cache — automatic per-surface optimization

Psynder's pixel-shading inner loop is **on-the-fly by default**: sample base texture, sample lightmap, multiply. Every surface every frame, no caching, no special cases. Plays cleanly with every modern feature in §7.5.

The renderer also ships a second pixel-shading kernel — the classic **Quake-style surface cache** — that pre-multiplies a surface's base texture and lightmap into a single combined chunk and samples it once per pixel. We do **not** expose this as a user toggle. Instead, the renderer **applies it automatically per surface, per frame, whenever the surface is eligible.**

**A surface is eligible iff all of these hold for the current frame:**

| Condition | Why |
|---|---|
| Lightmap filter mode is nearest | Filtered samples depend on sub-texel position, breaking the cache key |
| No dynamic lights overlap its bounds | Dynamic lights add per-pixel, per-frame contribution the cache can't represent |
| No normal map, specular, or env cubemap on its material | Those need per-pixel view/normal math the pre-shade can't bake |
| Surface is at a stable mip level (no transition this frame) | Mip transitions are part of the cache key; thrashing eats the win |
| Lightmap is LDR | HDR lightmaps are sampled directly through the modern path |

**Dispatch.** Each visible surface gets a `ShadingPath` tag (`SurfaceCached` or `OnTheFly`) during per-surface culling — one byte on the draw command. The per-tile rasterizer dispatches the right kernel **per-draw**, never per-pixel, so SIMD lanes stay coherent.

**Cache.** 4–8 MB slab in the level-scope allocator, keyed by `(surface_id, lightmap_version, mip_level)`, LRU-evicted. Entries are not evicted when a surface flips to `OnTheFly` — they sit warm in case the surface becomes eligible again next frame.

**Hysteresis.** A surface that's been ineligible must be eligible for 4 consecutive frames before re-classification, so a passing muzzle flash doesn't thrash 200 cache entries that would be eligible again the next frame.

**Outcome by visual preset:**

| Preset | Filtering | Dynamic lights | Normal maps | Result |
|---|---|---|---|---|
| Modern | bilinear / trilinear / aniso | many | yes | ~5% of surfaces cacheable; on-the-fly wins almost everywhere |
| Retro | nearest | few | no | ~95% of surfaces cacheable; renderer behaves as a surface-cached renderer |
| Mixed | per-material | per-region | per-material | engine picks per-surface; cache pays for itself on the simple geometry |

The retro look is reached by **turning off filtering and modern features in settings** — surface cache then engages automatically for the resulting eligible surfaces as a perf win. There is no separate "retro mode" pipeline; visual preset and pipeline path are the same lever, pulled from one end.

**Debug knob.** `r_force_shading_path` (console var) forces every surface to one kernel or the other for A/B comparison and profiling. Not exposed in shipping settings.

### 7.7 Blending, fog, special effects

- Alpha test, alpha blend (premultiplied), additive, multiplicative, subtractive — SIMD blend kernels.
- **Fog:** linear, exp, exp²; dithered for paletted output. Long-distance fog doubles as a draw-distance hider for large outdoor maps.
- **Specular:** Blinn-Phong with tangent-space normal maps.
- **Decals** via depth-equal pass.
- **Environment cubemaps** for car/aircraft bodies (sampled in the same SIMD path).
- **Skybox:** projected cubemap, drawn first into untouched tiles. Cheap analytic Rayleigh sky shader optional for outdoor maps.

### 7.8 Performance budget (1080p60 *internal*, desktop)

The budget below is for the **internal render resolution**, not the window size. See §7.9 — the window can be anywhere from 640×480 to 8K and the renderer doesn't care; only the internal resolution moves the numbers.

| Stage | Budget (ms) |
|---|---|
| Cull + setup | 1.5 |
| Vertex transform | 1.5 |
| Bin + raster + shade | 9 |
| Hybrid raytrace shadows | 3 |
| Resolve + post | 1 |
| Slack | 0.66 |
| **Total** | **16.66 (60 FPS)** |

Lower-end target: 720p60 internal with bilinear + baked-only lighting on an M1 Air or a 4-core Ryzen 3.

### 7.9 Resolution model — fixed internal, scaled present

Psynder renders at a **fixed internal resolution** chosen at startup (config file, command line, or in-game menu). The CPU framebuffer stays at that resolution for the life of the run. **Window resizing — including maximize and fullscreen — never changes the render resolution.** The platform present blits the framebuffer texture and scales it to whatever the window currently is.

Why this matters:

- **Predictable performance.** The renderer's cost is a function of internal resolution alone. A maximized 4K window costs no more than a tiny one. Frame budgets are stable across user setups.
- **Consistent aesthetic.** A game authored for 640×480 stays 640×480 on every monitor — the player sees a sharper or chunkier version but the same scene, same look, same fog distance, same pixel sizes for HUD elements.
- **Faithful to the era.** Quake, NFS, GTA III all worked this way. The "software look" depends on a fixed pixel grid; resolution scaling would break it.

**Configuration knobs:**

| Setting | Meaning |
|---|---|
| `render_width`, `render_height` | The only authoritative render resolution. Presets: 320×240, 640×480, 800×600, 1280×720, 1920×1080, 2560×1440, 3840×2160. |
| `window_width`, `window_height` | Initial window size; can differ from internal. |
| `scale_mode` | `nearest` (crispy / pixel-perfect retro), `linear` (smooth), `integer` (snap to integer multiples for pixel-perfect upscale). |
| `aspect_mode` | `letterbox` (default — preserves framebuffer aspect with black bars), `stretch` (fill the window, distorts), `crop` (fill window, clips edges). |

**Implementation:**

- The framebuffer lives in CPU memory at `render_width × render_height`, allocated once at startup or on resolution change.
- The platform present uploads (or maps) the framebuffer into a single GPU-side texture, then draws **one passthrough textured quad** sized to the window with the chosen filter and aspect handling.
- Windows: DXGI flip-model with a stretch blit; Linux: Wayland viewporter / X Render scaling; macOS: `CAMetalLayer` single-quad pass.
- The GPU does *only* this scaled blit. No engine shader runs on it. This is the entire GPU contact surface.
- Resolution changes mid-game are supported: framebuffer is reallocated from the level-scope allocator and the renderer state resets between frames.

**Integer scaling.** For retro presets we recommend `scale_mode = integer` — the framebuffer scales by the largest integer factor that fits the window, with letterboxing around it. Gives the cleanest pixel-art / chunky-software look at any window size.

**HiDPI / Retina.** The OS may report a logical window size that differs from physical pixels. We use the physical pixel size for the present surface; the framebuffer never changes. A Retina Mac running 1920×1080 internal in a 2880×1800 physical window simply gets a 1.5× scaled blit. The CPU never knows the display is HiDPI.

**Why not dynamic resolution.** It would defeat the perf-predictability argument, break the aesthetic, and add code complexity for no gameplay benefit. If a user wants a smaller workload they pick a lower `render_*` value; the engine doesn't second-guess them.

---

## 8. Lighting — hybrid baked + software raytraced

### 8.1 Baked lightmaps (radiosity / path-traced)

- Offline tool `lm_bake` runs **path-traced radiosity** on the static scene (this is the one place we actually do many-bounce path tracing — offline, where we can afford it):
  - Atlas packing (xatlas-style charts).
  - Direct lighting from emissive surfaces and static lights.
  - 2–4 bounces of indirect (configurable).
  - 16-bit half-float RGB lightmaps; per-surface resolution.
- Atlas streamed at load; one texture array in RAM, addressed per-surface.

### 8.2 Dynamic raytraced shadows

- **What gets traced:** shadows from dynamic lights (headlights, muzzle flashes, flashlights, dynamic point lights). Lightmaps handle the static world.
- **Acceleration structure:** SAH-built **BVH8** (8-wide nodes), refittable for animated meshes. Static TLAS built once; dynamic instances refit per frame. Written fresh; the SAH builder skeleton is informed by dmonte but BVH8 layout and refit are new.
- **Tracing:** packet rays of 8 (AVX2) or 4 (NEON). One shadow ray per pixel-per-active-light per frame; pixels with no nearby dynamic lights skip the trace entirely.
- **Heightmap shadow path** (active on maps using the raymarched terrain backend, §9.2): instead of a BVH trace, march the heightmap directly along the shadow ray — uniform 2D grid, cache-friendly, no node tests, no tree traversal. Cheaper than BVH and produces crisp terrain shadows on every dynamic light (helicopter spotlight throwing a ridge shadow over a soldier patrol, etc.). Polygon meshes still cast via BVH; the two occlusion results are MAX-combined per pixel.
- **Soft shadows:** PCSS-style — distance-to-occluder informs filter radius; low-discrepancy 4-sample stratified pattern across frames + temporal accumulation.
- **Denoising:** edge-aware à-trous filter, 2 passes, guided by depth + normal.

### 8.3 Dynamic light support

- Up to 8 dynamic lights per pixel via a **tiled light list** (forward+ style, on CPU): per-64×64 tile holds a culled list of light indices; the shading stage loops only over those.
- Headlights: spot with cookie texture. Brake lights: emissive + small point light. Muzzle flashes: short-lived point light (2 frames). Compound / camp lighting on outdoor maps: hundreds of static-with-flicker emissives baked into lightmap + a few dozen dynamic ones at any time.

### 8.4 Volumetric / atmospheric

- Low-res 3D froxel grid (160×90×64) for volumetric fog; in-scattering CPU side, ray-marched at resolve. ~1.5 ms cost.
- Rain: instanced streaks as alpha-blended quads, lit by the dynamic-light list — gives NFS:HS its night-rain look.
- Long-distance haze / draw-distance fog for outdoor maps (IGI / Delta Force-style); cheap analytic Rayleigh-only approximation, no precomputed LUTs needed at our scale.

---

## 9. World representation — two genre presets, one engine

Psynder carries **two world systems**, both producing the same `DrawItem` stream for the renderer.

### 9.1 Indoor: BSP + PVS + portals (Quake-style)

- **BSP** compiled offline by `lm_qbsp` (id-inspired).
- **PVS** baked per-leaf — culling is "find leaf, fetch bit vector, iterate."
- **Portals** for tighter cross-leaf culling.
- Brush-based geometry: authored in the **in-engine editor** (§10.8) using CSG primitives. A `.map` (TrenchBroom) importer is available as a one-way courtesy bridge for existing Quake-style maps. (See ADR-004 in §16.)

### 9.2 Outdoor: heightmap with two terrain backends

Psynder's outdoor world is heightmap-based, but the renderer ships **two terrain backends** (per-map selectable), because what's right for a racing track is wrong for a 4 km × 4 km tactical landscape. See ADR-008 in §16.

**Play styles covered:**

- **Racing tracks (NFS-style):** spline-defined road geometry — centerline + width + banking → extruded textured strip, driveable surface flagged for physics. Environment cards / impostors for trees and crowds; LOD bands switch mesh ↔ impostor.
- **Large outdoor FPS maps (Project IGI / Delta Force-style):** kilometre-scale view distances, sparse population, scattered structures (compounds, watchtowers, vehicles), draw-distance fog as a design choice. Vehicles, foot soldiers, and scripted aircraft (helicopters etc.) as kinematic movers — no full flight sim.

**Backend A — polygon CDLOD mesh** (default for racing tracks):

- 16-bit heightmap, chunked at 64×64, **CDLOD** seamless LOD, material per-vertex (4-weight splatmap).
- Best when you have detailed local geometry: spline roads with banking, sharp curbs, modeled rocks, asphalt detail textures.
- Polygon meshes integrate with the rasterizer the obvious way.

**Backend B — heightmap raymarcher** (default for tactical FPS maps, *NovaLogic Voxel Space-inspired*):

- The same 16-bit heightmap, but rendered by **per-column ray-marching** through the height field instead of being tessellated into triangles. (NovaLogic's "Voxel Space" was always a heightmap raymarcher despite the name — no actual 3D cells. We adopt the technique, not the misnomer.)
- Each pixel column casts a ray, steps forward (logarithmic-distance steps), looks up the heightmap at each step, and paints vertical strips into the framebuffer wherever the height projects above the column's running horizon.
- View distance is essentially free — it's a ray-step budget, not a polygon budget.
- LOD is automatic via increasing step size with distance. No CDLOD chunks, no tessellation seams.
- Each tile's columns are independent ray marches → drops straight into our tile job system, 8 columns at once on AVX2, 4 on NEON.
- Authentic to the genre — players who grew up on Comanche and Delta Force know exactly what this looks like.

**How both backends compose with the rasterizer:**

Both backends write the **same tile framebuffer and Z-buffer**. The Z-buffer is the glue:

```
For each tile (in parallel):
  1. Terrain pass — either:
       (a) CDLOD mesh: standard rasterizer, terrain triangles, depth-tested
       (b) Heightmap raymarch: per-column march, fills color + depth
  2. Polygon pass: rasterize meshes (soldiers, vehicles, buildings, props, debris)
     — standard rasterizer with depth test, respects the terrain's Z values
  3. Hybrid shadow rays (§8.2): BVH for polygon meshes; for backend (b) maps,
     a second shadow path raymarches the heightmap directly (cheaper than BVH)
  4. Resolve, post, present
```

Polygon meshes land on terrain seamlessly in both backends because they share depth space.

**Limitations of backend (b):**

- **No overhangs or caves.** A single Y per (X, Z). Cave / tunnel sections must be modeled as polygon meshes and composited in via Z-buffer, or as BSP indoor segments transitioning to outdoor.
- **Lightmapping convention differs.** Static lighting is baked into a parallel "light channel" of the colormap (each terrain texel stores both color and baked illumination), since there are no UVs in the polygon-surface sense. `lm_bake` outputs both channels for raymarched terrain.

**Shared tech (both backends):**

- **Map scale capped at 16 km × 16 km** — float32 world coordinates with per-frame render-relative origin re-centering (see ADR-005 in §16).
- **Vegetation / scatter:** instanced billboards and meshes seeded from density maps; placed deterministically so multiplayer stays in sync. Renders via the polygon rasterizer regardless of terrain backend.
- **Physics:** `heightfield` collision primitive (§10.1) collides soldiers and vehicles against the same heightmap data both backends consume.

**Per-map runtime config:** `terrain_backend = mesh | raymarch`. Backend is fixed for a given map (no mid-map switching — different LOD pipelines, different lightmap layout).

### 9.3 Unified scene graph

- One transform hierarchy. Top-level partitions: `WorldStatic`, `WorldDynamic`, `Effects`, `UI`. The renderer doesn't care which world system fed a draw; it gets the same `DrawItem` either way.
- Per-entity bounding boxes and leaf/cell memberships are tracked centrally; when an entity moves, the scene graph notifies every spatial index that contains it. See §9.4 for the index hybrid.
- Runtime app code should submit entities to a `scene::Scene`, not
  hand-write raster or RT frame loops. `Scene::create_entity()`
  creates the ECS entity plus its default transform/node binding, and a
  `RenderableComponent` links geometry to a shared `render::MaterialId`.
  Raster and RT renderers consume the same gathered scene render items.
- Materials are data, not shader objects: a cache-coherent
  `render::MaterialLibrary` stores SoA columns for albedo/texture,
  winding (`CCW`, `CW`, or two-sided), blend mode, reflectivity, and
  raster/RT visibility and shadow flags. The future editor panel edits
  this library over the existing local WebSocket IPC, but the runtime
  contract works without any editor dependency.

### 9.4 Spatial query routing — multiple structures, one scene

There is no single "best" spatial structure for every query (see the prior-art literature and §17), so Psynder carries several at once. Each one is a **view** of the same scene optimized for a specific question; the engine maintains them in parallel and the query router picks per call.

**Query → structure routing:**

| Query | Structure | Where it lives |
|---|---|---|
| Raycast / shadow ray / probe | **BVH8** | TLAS over all geometry, per-instance BLAS |
| Frustum cull, dynamic actors | **BVH** | Refit per frame |
| Visibility, static indoor | **BSP + PVS** bit vector | Per-leaf, precomputed offline |
| Physics broadphase | **SAP** (sweep & prune) | Falls back to **hashed grid** at very high body counts |
| Terrain shadow occlusion (raymarch backend) | **Direct heightmap raymarch** | Uniform 2D grid; cache-friendly |
| AI / audio nearest-neighbor | **Hashed grid** | Per-region, sparse |

**Maintenance policy per structure:**

| Structure | Build | Refit | Rebuild trigger |
|---|---|---|---|
| BVH (static world) | Once at level load | — | On level reload |
| BVH (dynamic actors) | Once at startup | Every frame, O(n) | SAH cost / original > 1.3 → async rebuild |
| BSP + PVS | Offline (`lm_qbsp`) | — | Offline only |
| SAP | At startup | Incremental per frame | Pair count > N → switch to hashed grid |
| Hashed grid | Lazy | — | Bucket overflow → resize |

**Dynamic adaptation — the mix isn't static, it's measured and tuned at runtime:**

- **BVH quality degrades as actors move.** We measure the refit SAH cost every frame and compare to the as-built cost; when it exceeds 1.3× (configurable), we kick off an async rebuild on a worker. When the new tree is ready, it atomically replaces the old. The frame using the stale tree pays a small quality cost (looser bounds = more node tests) but never a correctness cost.
- **Broadphase auto-switches by load.** SAP is the default; when a region's active body count crosses a threshold (~10k) we promote that region to hashed-grid broadphase. The threshold is hysteretic so we don't thrash.
- **Per-region structure overrides.** A dense combat area can tag itself as "wants tighter BVH"; a quiet hallway falls back to a linear scan over a tiny entity list. Region-level config drives this; no per-frame heuristic needed.
- **Cost budgets are enforced.** Each spatial-index maintenance task has a per-frame budget. If BVH refit blows budget, we skip it for that frame and use the stale tree — correctness is preserved, only quality dips momentarily.
- **Hot-swappable.** Adding or removing a structure at runtime (e.g., promoting a cell to its own BVH) uses the same drain-and-reallocate pattern as resolution / tile-size changes. One frame stall, no rebuild.

**One scene, many indices.** When an entity moves, the scene graph is the single source of truth — it pushes the update to every index that contains the entity. Most entities live in exactly one index (a dynamic enemy lives in the dynamic BVH; a static wall lives in the BSP). Membership is tracked centrally so a `WorldStatic` indoor wall isn't pretending to also live in the dynamic BVH.

**The mental model:** spatial structures aren't a choice, they're a *toolbox*. Every query asks "what am I looking for?" and the router hands it the cheapest tool for that question. The engine measures whether the tools are paying for themselves and adapts when they aren't.

---

## 10. Subsystems

### 10.1 Physics — our own engine, `psynder_phys`

No third-party physics dependency. Psynder ships its **own physics engine** because (a) we want full control of determinism and SIMD layout, (b) we want it to fit the same job-graph parallelism model as everything else, and (c) we want vehicles and characters to be first-class, not bolted on top of someone else's constraint solver.

**Core architecture:**

- **Fixed 120 Hz sim tick**, render interpolates between sim states. Bit-deterministic across platforms when the same compile flags are used (we avoid `-ffast-math` in physics TUs and pin FP rounding to round-to-nearest).
- **Broadphase:** parallel sweep-and-prune on 3 axes (one job per axis), pairs merged into a global candidate list via a lock-free SPMC queue. Falls back to a hashed grid at very high body counts.
- **Narrowphase:** GJK + EPA for convex pairs, special-cased kernels for sphere-sphere, sphere-capsule, capsule-capsule, AABB-AABB. All vectorized (`f32x8` on AVX2, `f32x4` on NEON) so 8 (or 4) pairs collide at once.
- **Island detection:** union-find over the contact graph; each island becomes an independent job.
- **Solver:** projected Gauss-Seidel (sequential-impulse) **per island, in parallel across islands**, and — as of ADR-013 (§16) — **graph-coloured for parallelism *within* a large island** (a colour's contacts touch disjoint dynamic bodies → race-free `parallel_for`; colours run sequentially so it stays Gauss-Seidel across colours). 8 velocity iterations + 3 position iterations by default. Warm-started across frames. Deterministic run-to-run (the colouring is a pure deterministic function of the contacts), though the exact bit-values differ from the pre-ADR-013 plain-index order by design.
- **Continuous collision (CCD):** for fast bullets and small fast vehicles — conservative-advancement TOI computed per pair when relative speed crosses a threshold.

**Shape primitives:** sphere, capsule, box, convex hull (≤ 64 verts), compound (hierarchy of primitives), heightfield (terrain), triangle-mesh (static world).

**Specializations:**

- **Vehicle module (`psynder_phys::vehicle`):** raycast or sphere-cast suspension at 4+ corners (motorcycles supported with 2, tracked vehicles like APCs / tanks supported via per-track friction strips), Pacejka-lite tire model with combined slip, drivetrain (engine torque curve → clutch → gearbox → differential → wheels). Aero drag + downforce. **Damage model:** per-panel scalar + per-component health (engine, suspension, brakes) for NFS:HS-style attrition.
- **Character controller (`psynder_phys::character`):** capsule-based kinematic controller for FPS / soldier use; sweep-step-and-slide algorithm with crouch, prone, ladder, water states.
- **Aircraft as kinematic movers:** helicopters and planes are rigid bodies driven by script / animation curves, not by aerodynamic simulation. Bullets penetrate, debris falls when shot down. (A real flight module is post-1.0; out of scope.)

**Parallelism inside the physics tick:**

```
phys_tick():
  parallel: integrate forces                  [N bodies / job]
  parallel: broadphase SAP                    [3 axis jobs]
  parallel: narrowphase                       [contact pairs in batches]
  serial:   island detection (union-find)     [fast, single-thread]
  parallel: per-island solver                 [1 island / job; large islands
                                               also graph-coloured -> parallel
                                               within the island (ADR-013)]
  parallel: integrate velocities & positions  [N bodies / job]
  parallel: CCD second pass for flagged pairs
```

All jobs run on the same work-stealing pool as the renderer — physics doesn't get its own thread.

**Why we can write this and ship on time:** sim physics is well-trodden territory; the math is in *Erin Catto's GDC slides* and *Erleben's thesis*. The novel work is fitting it cleanly to our SIMD/job model. We do **not** attempt soft bodies, cloth, or fluids in v1.0 — those are post-1.0 modules.

### 10.2 Audio

- Fully **CPU** mixer (consistent with the engine ethos): 32-channel software mixer with HRTF (vendored minimal HRIR set) for FPS / first-person; positional stereo+EQ path for racing chase cameras.
- Reverb: FFT convolution (indoor), FDN algorithmic (outdoor).
- Backends: WASAPI / ALSA+PipeWire / CoreAudio.

### 10.3 Input

- Keyboard + mouse + gamepad (XInput / evdev / GameController.framework). Force feedback racing wheels via the platform layer.

### 10.4 Networking

- Reliable-UDP layer (sequence + sliding window + selective acks). Lockstep for racing (8 players), client-server snapshot interpolation for FPS / tactical FPS (16–32 players, area-of-interest filtering for large outdoor maps). Public relay later.

### 10.5 Scripting

- **Lua 5.4** for gameplay; runs on game threads, not engine workers.
- Scripts must follow the DOTS rules in §3.3 — register systems, declare component reads/writes, no per-entity "object" methods. The binding layer makes this the path of least resistance.
- A **live REPL** is bound into the in-engine editor (§10.8), so scripters can define new systems, register components, and hot-spawn entities mid-session with no recompile.

### 10.6 UI — three surfaces, three approaches

Psynder's UI splits across **three surfaces, each using the right tool for its job** (see ADR-009 in §16):

| Surface | Library | Where it runs | Authored by |
|---|---|---|---|
| Editor panels — inspector, hierarchy, asset browser, prop spawn menu, console, profiler, scripting REPL | **React + TypeScript, in Chrome app windows** | Out-of-process, separate OS windows, IPC to engine via local WebSocket | Engineers in TSX, designers in CSS |
| In-viewport overlays — perf graphs, allocator heatmap, hitbox / wireframe / BVH viz, manipulator gizmos, selection highlights, brush previews | **Immediate-mode** (Dear ImGui-style, our own implementation, minimal) | In-engine, drawn into the same software framebuffer as the game | Engineers in C++ |
| Player HUD + menus — health, ammo, minimap, main menu, options, mission briefings, scoreboards, cockpits | **RmlUi** (HTML/CSS subset) | In-engine, drawn into the same software framebuffer as the game | Designers in `.rml` / `.rcss` |

**Editor panels — React + Chrome, out-of-process:**

The editor isn't built as a monolithic in-engine UI. Each editor panel is its own **React + TypeScript application running in a separate Chrome app window**, which gives us:

- **Multi-monitor docking native to the OS.** Drag the inspector to a second monitor, the asset browser to a third — each panel is a real OS window managed by the system window manager. No engine-side dock-layout code, no window-in-window hacks. Survives display hotplug, scales per-monitor DPI correctly.
- **Real Chrome DevTools.** When editor UI misbehaves, open DevTools in the panel and inspect React state, network traffic, the DOM. Debugging superpower no in-engine UI can match.
- **The React ecosystem.** Designer-friendly component libraries (shadcn/ui, Radix, TanStack Table, etc.), real CSS, real animations, real form handling, real input widgets — without rolling our own.
- **Hot-reload at web speeds.** Vite HMR means editor UI changes appear sub-second, no engine recompile.
- **Cross-platform free.** Chrome runs identically on Windows, Linux, and macOS Apple Silicon.

**Editor IPC architecture:**

The engine runs as the main process. On startup (in editor mode) it spins up a **local-only HTTP + WebSocket server** (default `127.0.0.1:7654`, configurable, session-token gated) under `engine/editor/ipc/`. When the user opens a panel, the engine launches Chrome with `--app=http://127.0.0.1:7654/panels/<name>?token=...`.

- **Transport:** WebSocket for state updates and commands (bi-directional, sub-millisecond on localhost). HTTP for the initial page load + static React bundle.
- **Protocol:** msgpack over the WebSocket. Schema-versioned. Defined in `engine/editor/ipc/protocol.psy` — an IDL from which we generate both C++ and TypeScript stubs at build.
- **Auth:** session token generated at engine startup, passed via URL fragment, validated by the engine. Localhost-only, never bound to external interfaces.
- **State sync:** engine pushes "scene state deltas" at frame boundaries; panels subscribe to the slices they care about (e.g., the inspector subscribes only to the selected entity's components, the profiler subscribes only to the perf stream). The engine never blocks waiting on a panel.
- **Component schemas auto-bind to React forms.** Each `PSYNDER_COMPONENT(...)` (§3.3) emits a schema that the React inspector reads; field types map to form widgets automatically. New components show up in the inspector with no manual UI binding.

**In-viewport immediate-mode UI — kept minimal:**

Some overlays must draw *into the game framebuffer*, composited with the rendered scene — perf graphs over the game, hitbox visualization on top of an enemy, the brush preview during heightmap sculpting, manipulator gizmos in editor mode. Putting these in an external Chrome window would mean a transparent OS window overlay on the game window, which is fragile across compositors and effectively impossible on Wayland. So we keep a **small** immediate-mode UI (~few hundred lines of widget code) for in-viewport overlays only — not for editor chrome.

**Player UI stays RmlUi:**

Shipped games must run without Chrome installed. Player HUDs, menus, cockpits, and scoreboards are RmlUi-rendered (MIT-licensed HTML/CSS subset, vendored under `third_party/rmlui/`) into the same framebuffer as the game. Designers author them in `.rml` / `.rcss`, hot-reloaded via the asset VFS (§4.6). FreeType (MIT, vendored) handles font glyph rasterization. RmlUi's `RenderInterface` plugs into our software rasterizer like any other triangle source.

**ECS data binding (DOTS-compatible across all three surfaces):**

Engine state flows *one-directionally* into UI:

- **React panels:** subscribe to msgpack-encoded state deltas over WebSocket; commands flow back as RPCs.
- **Immediate-mode overlays:** read engine state directly each frame (in-process, same memory).
- **RmlUi player HUDs:** ECS systems write a small "UI state" struct each frame; a binding adapter copies it into RmlUi's data model.

In all three cases, no UI code looks up game state ad-hoc — the data flow is push, not pull. DOTS contract preserved.

**Scripting.** RmlUi binds to our Lua VM (§10.5); the React editor binds to the engine's Lua REPL through the WebSocket IPC. Either way, scripters can author UI handlers in Lua.

### 10.7 Asset pipeline

- **`lm_pak`:** archive format, FNV-1a indexed, optional zstd compression.
- **Cooker:** converts `.obj`/`.gltf`/`.png`/`.wav` → `.lmm` meshes, `.lmt` textures with mipchain, `.lma` audio.
- **Hot reload:** dev builds watch source files; engine swaps assets atomically between frames.

### 10.8 Editor / sandbox mode — Garry's Mod-style, in-engine

Psynder doesn't ship a separate editor binary. The **editor is a mode of the engine itself**, layered onto the same renderer, physics, scripting, networking, and ECS that the game uses. Press `~` or `F2` to flip the running session from "play" to "editor" — nothing reloads, nothing restarts, the scene you were just shooting at is now editable.

This is one of the more distinctive features of the engine, and the reason we don't take a TrenchBroom dependency (see ADR-004 in §16).

**Two intertwined use cases, one toolset:**

1. **Sandbox / play-mode editing (Garry's Mod-inspired):** in any running level, spawn props from a searchable menu, grab them with a physgun-style cursor, weld / constrain / unfreeze them, glue rockets to crates, set up scenarios. Save the result as a new level or a portable "contraption" file. Excellent for testing — designers and engineers can build the world state they want in seconds, without authoring a full level offline.
2. **Level authoring (Hammer / TrenchBroom-inspired):** brush-based CSG primitives sculpt indoor BSP geometry; terrain painting tools raise / lower / smooth / material-paint outdoor heightmaps (both backends in §9.2); entity placement with a property inspector; light authoring; lightmap bake trigger. Saves to the Psynder level format (`.psylevel`), which is the same format the runtime loads.

Both modes share the same primitives. "Spawn a crate" in sandbox is the same operation as "place a crate prop" in authoring — only the surrounding UI chrome and the physics-simulation-paused flag differ.

**Key features:**

- **Brush CSG.** Convex primitives (box, wedge, prism, cylinder), boolean combine, snap-to-grid, smart-extrude. Indoor geometry compiles to a runtime BSP via the same `lm_qbsp` machinery used for offline maps.
- **Heightmap sculpt.** Raise / lower / smooth / flatten with falloff brushes; paint splat materials up to 4 weights per vertex. Works for both terrain backends (mesh CDLOD and heightmap raymarch).
- **Physgun.** Pick up any rigid body with the mouse, drag through 3D space, freeze in place, rotate, scale, weld to another body. Maps directly to `psynder_phys` constraints.
- **Constraints.** Weld, axis, slider, ball-socket, rope, elastic. Each is a runtime physics constraint; saving the contraption serializes its constraint graph.
- **Prop spawn menu.** Searchable browser over all assets in the loaded `.lmpak` archives, with category tags, thumbnails, and favorites. Filters by genre preset (indoor FPS / racing / tactical).
- **Entity inspector.** Click an entity, edit its components in a property grid. Components are POD structs registered via `PSYNDER_COMPONENT(...)` (§3.3), so the editor enumerates them automatically — no manual UI binding code.
- **Live Lua REPL.** Console bound to the running engine state. Define new systems, register new components, hot-spawn entities, write debug helpers — all at runtime, no recompile, no restart.
- **Undo / redo.** Edits emit deltas onto a stack. Undo is O(1) per step.
- **Multi-user co-op editing.** The editor is just another game state, so the networking layer (§10.4) handles multi-user editing natively — multiple developers in one level building together, the same way Gmod's multiplayer sandbox works.
- **Save / load.** Levels save to `.psylevel` (binary, zstd-compressed). Contraptions save to `.psyc` (smaller, just an entity graph + constraint set). Both load back into editor or play mode.

**Implementation notes:**

- The editor mode lives in `engine/editor/` and depends on `render`, `scene`, `physics`, `script`, `net`, `ui`. It is part of the engine library, not a separate binary; the main game executable boots into editor mode via `--editor` flag or runtime hotkey.
- **Editor panels are React + TypeScript apps in separate Chrome windows** (see §10.6). They communicate with the engine over a local WebSocket on `127.0.0.1:7654`. The React app source lives under `engine/editor/web/`, built with Vite, bundled into the engine binary as static assets at compile time (served from filesystem in dev for HMR).
- **In-viewport overlays** (manipulator gizmos, selection highlights, brush previews, debug HUD when the editor is open) draw via the in-engine immediate-mode UI (§10.6) into the game framebuffer.
- **Player HUDs** in shipped maps use the RmlUi side of §10.6 — designers author those in HTML/CSS independently of the editor.
- The editor follows the same DOTS contract as user code (§3.3): C++ tools that move many entities at once register as systems with declared component reads/writes; the scheduler parallelizes them automatically. (The React code in `engine/editor/web/` is TypeScript and exempt from the C++ DOTS rules — it talks to the engine over IPC, not in-process.)
- Editor-only logic (including the WebSocket server, the panel route handlers, the React static bundle) is fenced behind `#if PSYNDER_EDITOR` so retail / multiplayer shipping builds strip it for a smaller, cleaner binary.

**What we still ship for compatibility:** a `.map` (TrenchBroom format) importer is a small convenience tool under `tools/lm_mapimport/`. Anyone with existing Quake-style maps can drop them into Psynder. That's not a dependency, just a one-way bridge.

---

## 11. Platform layer

### 11.1 Windows
- Win32 directly, no SDL dep.
- Framebuffer: DXGI flip-model swap chain. The CPU framebuffer is uploaded to a fixed-size texture each frame; a single passthrough quad does the scaled blit to the window (see §7.9). Window resize = scale change only, never a render-resolution change. GPU used only as a scanout / stretch device.
- Audio: WASAPI shared mode (event-driven).

### 11.2 Linux
- **Wayland** primary (xdg-shell + `wp_viewporter` for the scaled present; dmabuf where the compositor supports it; otherwise wl_shm).
- **X11** fallback via XShm + X Render for the scaling blit.
- Internal framebuffer stays at the configured `render_width × render_height`; window resize is handled entirely by the present's viewport scale (see §7.9).
- Audio: PipeWire first, ALSA fallback.

### 11.3 macOS Apple Silicon
- AppKit + `CAMetalLayer` as a *presentable surface only* — single passthrough textured quad shows our CPU framebuffer, scaled to the window (see §7.9). HiDPI / Retina handled by the present scale; the framebuffer never changes size. Build flag for `IOSurface` + Core Animation directly with zero Metal.
- NEON SIMD first-class; M1/M2/M3 P-cores explicitly benchmarked in CI.
- Audio: CoreAudio AUHAL.

### 11.4 Build matrix (CI)

| OS | Arch | Compiler | SIMD | Notes |
|---|---|---|---|---|
| Windows 11 | x86-64 | Clang 17, MSVC | SSE4.2 / AVX2 / AVX-512 | DXGI present |
| Ubuntu 24.04 | x86-64 | Clang 17, GCC 13 | SSE4.2 / AVX2 / AVX-512 | Wayland + X11 |
| macOS 14+ | arm64 | Apple Clang | NEON | Apple Silicon only |

---

## 12. Repository layout

```
psynder/
├── CMakeLists.txt
├── CMakePresets.json
├── vcpkg.json
├── README.md
├── DESIGN.md                ← this doc
├── LICENSE
├── .clang-format
├── .clang-tidy
├── .github/workflows/       ← CI (3 OSes)
├── docs/
│   ├── 01-getting-started.md
│   ├── 02-rendering.md
│   ├── 03-lighting.md
│   ├── 04-world-formats.md
│   └── adr/                 ← architecture decision records
├── engine/
│   ├── core/                ← lm_core: psynder_mem allocators, log, containers
│   ├── math/                ← lm_math (incl. fixed-precision big-coords)
│   ├── simd/                ← lm_simd: x86 + neon kernels
│   ├── jobs/                ← lm_jobs
│   ├── asset/               ← lm_asset: VFS, cooker formats, hot reload, streaming
│   ├── scene/               ← lm_scene: ECS, transforms, partitions
│   ├── render/
│   │   ├── raster/          ← tiled rasterizer
│   │   ├── rt/              ← software raytracer: BVH8, intersect, denoiser
│   │   └── post/
│   ├── world/
│   │   ├── bsp/             ← indoor (Quake-style)
│   │   └── outdoor/         ← heightmap + spline tracks (NFS) + large maps (IGI / Delta Force)
│   ├── audio/
│   ├── physics/             ← psynder_phys: our own physics + vehicle/aircraft modules
│   ├── net/
│   ├── script/
│   ├── ui/
│   │   ├── imm/             ← minimal immediate-mode UI (ImGui-style, our own) — in-viewport overlays only
│   │   └── rml/             ← RmlUi binding + RenderInterface impl — player HUDs / menus / cockpits
│   ├── editor/              ← in-engine editor / sandbox mode (Garry's Mod-style)
│   │   ├── core/            ← engine-side editor logic: brush CSG, heightmap sculpt, physgun, constraints, REPL host
│   │   ├── ipc/             ← local WebSocket + HTTP server, msgpack protocol, IDL stub generator
│   │   └── web/             ← React + TypeScript editor panels (Vite-built, bundled as static assets)
│   └── platform/
│       ├── win32/
│       ├── linux/
│       └── macos/
├── tools/
│   ├── lm_bake/             ← lightmap baker (offline path tracer, uses render/rt)
│   ├── lm_qbsp/             ← BSP compiler
│   ├── lm_cook/             ← asset cooker
│   ├── lm_pak/              ← archive tool
│   ├── lm_mapimport/        ← one-way TrenchBroom .map → .psylevel importer (courtesy bridge)
│   └── dmonte_pt/           ← optional: standalone reference path tracer for golden tests
├── samples/
│   ├── 01_triangle/
│   ├── 02_textured_quad/
│   ├── 03_quake_room/
│   ├── 04_nfs_track/
│   ├── 05_rt_shadow_packets/
│   └── 06_tactical_map/     ← large outdoor IGI / Delta Force-style map demo
├── third_party/             ← xatlas, zstd, miniaudio (reference only), Lua, RmlUi, FreeType, etc.
└── tests/
    ├── unit/
    ├── golden/              ← golden-image renderer tests (dmonte = oracle)
    └── bench/
```

---

## 13. Milestone roadmap

A demo at every milestone — no engine work without a screenshot.

### M0 — Bring-up (weeks 1–4)
- Repo, CI, three-OS build of an empty window.
- `lm_core` + `lm_math` + `lm_simd` skeleton.
- **Demo:** clear-color animation on Win/Linux/macOS.

### M1 — First triangle (weeks 5–8)
- Job system, vertex pipeline, single-threaded scanline rasterizer.
- Nearest-filter textures, no Z.
- **Demo:** rotating textured triangle (sample 01).

### M2 — Tiled rasterizer + Z + bilinear (weeks 9–14)
- Tile binning, multithreaded raster, depth buffer.
- Bilinear + mipmaps + perspective-correct.
- **Demo:** rotating crate room (sample 02).

### M3 — BSP + lightmaps + editor v0 (weeks 15–22)
- BSP loader, PVS culling, lightmap atlas, surface cache.
- **dmonte integrated** as the offline lightmap baker (direct only first, multi-bounce next).
- **Editor IPC bring-up:** local WebSocket + HTTP server on `127.0.0.1:7654`, msgpack protocol, session-token auth, IDL stub generator (emits C++ and TypeScript).
- **First React panel:** the **Inspector** — Chrome window launched via `--app=`, subscribes to selected-entity component schemas, auto-renders form fields from `PSYNDER_COMPONENT(...)` schemas (§3.3).
- **Minimal immediate-mode UI** for in-viewport overlays (gizmos, debug HUD).
- **Editor v0:** in-engine mode toggle; spawn / move / delete entities in the loaded BSP scene via the React Inspector. Enough to make test scenarios in seconds.
- **Demo:** walk through a small Quake-style room (sample 03).

### M4 — Outdoor track + vehicles + editor sculpt (weeks 23–32)
- Heightmap terrain + CDLOD (backend A from §9.2), spline tracks, skyboxes, billboards.
- **`psynder_phys` v0:** rigid bodies, broadphase SAP, GJK/EPA narrowphase, parallel island solver.
- **`psynder_phys::vehicle` v0:** suspension + Pacejka tires + drivetrain, drivable demo.
- **Editor:** heightmap sculpt tools (raise/lower/smooth/paint), brush CSG for placing simple buildings, spline track editor.
- **Demo:** one lap of a test track (sample 04).

### M5 — RT shadow packets (weeks 31–40)
- BVH8 build + packet traversal (intersect kernel + RNG borrowed from dmonte; everything else fresh).
- Dynamic shadow rays + denoiser.
- Tiled light list, headlights.
- **Demo:** night scene with colored dynamic lights and 8-wide raytraced
  shadow packets (sample 05).

### M6 — Large outdoor FPS maps + sandbox editor (weeks 41–48)
- Extended heightmap world: kilometre-scale view distances, sparse population, scattered structures.
- **Heightmap raymarcher (backend B from §9.2):** per-column ray-marching, heightmap shadow path, tile-parallel.
- Soldier character controller, NPC AI v0, scripted helicopter / vehicle movers.
- Long-distance haze, sky model, distant impostors.
- **Editor:** physgun, weld / axis / slider / ball-socket constraints, prop spawn menu, save/load `.psylevel` + `.psyc` (contraption). Multi-user co-op editing via the net layer.
- **Demo:** patrol a 4 km × 4 km tactical map at dawn (sample 06), then enter sandbox mode and weld a rocket to a watchtower.

### M7 — Feature parity push (weeks 49–56)
- Anisotropic filtering, normal maps, specular, decals, env cubemaps.
- Volumetric fog, rain, bloom.
- Audio: mixer + HRTF + reverb.
- Networking: rUDP + lockstep racing demo + tactical-FPS multiplayer.
- **RmlUi integration:** HTML/CSS player UI, FreeType font rasterization, asset-VFS hot-reload of `.rml` / `.rcss`, Lua bindings for handlers. First HUDs, menus, and a racing dashboard ship.

### M8 — Editor polish & 1.0 (weeks 57+)
- In-engine editor stable across both world modes; full feature set in §10.8.
- Multi-user co-op editing tested with 4+ editors in one session.
- `.map` (TrenchBroom) importer ships as a courtesy bridge.
- `lm_bake` ships full path-traced multi-bounce.
- Asset hot reload battle-tested.
- Documentation pass, contributor guide, public release.

Total: ~18 months for a small team, ~30 months solo. Pace adjusts cheerfully.

---

## 14. Coding standards (excerpt)

**Architecture rules (see §3 for full policy):**
- **Hot subsystems** (`render/`, `scene/ecs/`, `physics/`, `audio/mixer/`): strict DOTS — POD components, SoA storage, free-function systems, no `virtual`, no `shared_ptr`, no per-frame heap allocation.
- **Cold engine code** (tools, editor, asset loaders, platform shim, network protocol, file-format parsers): normal C++ — classes, RAII, `virtual` for plugin slots, STL containers — whatever is clearest.
- **Public game/script API** (the surface users build on): strict DOTS, no exceptions. Systems and queries only, no entity-object methods.
- New singletons require an ADR; the allow-list is `EcsRegistry*`, `JobSystem*`, `Allocator*`, `Log*`, `Clock*`, `Config*`.

**Style:**
- 4-space indent, 100-col soft limit.
- `snake_case` for functions/locals, `PascalCase` for types, `kCamelCase` for constants.
- Pointer ownership: raw = non-owning; `std::unique_ptr` = owning; no `shared_ptr` outside scripting/tools.
- No `using namespace` in headers. Single namespace `psynder::` for engine (`psynder::render::`, `psynder::math::` nested).
- Every public header: `// SPDX-License-Identifier: MIT` + one-paragraph header.

**Testing:**
- Required for: math, simd kernels, ECS chunk integrity, BVH traversal, BSP loader, asset cooker round-trips.
- Golden-image tests with 0.5% per-pixel tolerance for renderer regressions; the optional `dmonte_pt` reference renderer can serve as one of the oracles for lighting if we keep it around.
- Bench gate: PRs that regress the per-tile rasterize or per-island physics benchmark by > 2% must justify in description or fail CI.

---

## 15. Contribution model

- `main` always shippable; feature work in branches; PRs squash-merge.
- ADRs in `docs/adr/`, required for cross-system changes.
- "Good first issues" seeded each milestone — asset loaders, math identities, SIMD kernels for arches we don't own (RISC-V Vector eventually).
- Code of Conduct: Contributor Covenant 2.1.

---

## 16. Risks & open questions

| Risk | Severity | Mitigation |
|---|---|---|
| Software raytracing too slow at 1080p | High | Tile-level early-out; coarse shadow buffers; aggressive temporal reuse |
| AVX-512 availability uneven | Medium | AVX2 is baseline fast path; AVX-512 opportunistic |
| macOS no-GPU present is fiddly | Medium | Metal-as-scanout acceptable; engine stays GPU-free |
| Asset pipeline scope creep | High | Cooker takes glTF/obj/png/wav, period. Authoring stays external. |
| Writing our own physics is more work than vendoring | High | Scope ruthlessly: rigid bodies + vehicles + characters only. No cloth/soft/fluid/full-flight in v1. Lean on Catto/Erleben proven algorithms. |
| Chrome required as a dev dependency for editor panels | Medium | Bundle Chromium portable build optionally; document install for Win/Linux/Mac; editor functions only when Chrome is launchable. Shipped games don't need it. |
| JS toolchain (Node, npm, Vite) adds build complexity | Low | Toolchain is editor-only; lives under `engine/editor/web/`. Retail builds with `PSYNDER_EDITOR=0` skip the web bundle entirely. |
| IPC race conditions between React panels and engine | Medium | One-directional state push from engine, request/response RPCs for commands. Schema-versioned msgpack catches drift at runtime. |
| Determinism breaks across compilers/arches | Medium | `-fno-fast-math` in physics TUs, integer accumulators where possible, golden tests in CI on all three platforms |
| dmonte reference renderer (`dmonte_pt`) bit-rots if kept around | Low | Acceptable — it's optional. If it stops compiling we drop it; the engine doesn't depend on it. |
| C++23 compiler portability gaps | Low | Feature-detect, fallback when needed |

ADR log (decisions and open questions — each will get its own file under `docs/adr/` as it's settled):
- **ADR-001:** ✅ **Decided.** On-the-fly is the default pixel-shading kernel. Surface cache is applied *automatically* per surface when eligible (see §7.6); not exposed as a user toggle. Retro look is reached by turning off filtering and modern features in settings — the engine then auto-engages surface cache for the resulting eligible surfaces. Single lever, both ends; no separate retro pipeline.
- **ADR-002:** ✅ **Decided.** 64×64 is the default tile size. The rasterizer is templated over `<TILE_W, TILE_H>` and ships three specializations — 32×32, 64×64, 128×128 — selectable via the `r_tile_size` console var. Tile size is **runtime-changeable mid-run** with the same semantics as resolution change (see §7.9): the renderer drains in-flight frames, reallocates the tile pool from the level-scope allocator, swaps the specialization function pointer, and resumes — one visible hitch, ~30–50 ms. CI benchmarks all three sizes on every commit. Per-platform defaults may diverge once we have data: Apple Silicon's larger L1 (128–192 KB on M1/M2/M3 P-cores) may justify 128 there, but we ship 64 everywhere until measurements say otherwise.
- **ADR-003:** ✅ **Decided.** Hybrid skinning. Chassis / fuselage is skinned (root bone driven by physics, cosmetic bones for damage / flex / crumple); wheels, control surfaces, landing gear, hubcaps, mirrors, and other detachable/articulating parts are rigid meshes parented to skeleton bones, with transforms read directly from the physics solver (one-to-one with the physics rigid bodies). Severe damage detaches a rigid sub-mesh into its own standalone physics body. LBS default; DQS per-material where needed. See §7.2.
- **ADR-004:** ✅ **Decided.** Bespoke **in-engine editor** (`engine/editor/`), Garry's Mod-style. The editor is a *mode* of the running game, not a separate binary — press `~` to flip play ↔ edit. Covers both sandbox / play-time prop manipulation (physgun, welding, contraptions, save/load) and level authoring (brush CSG for indoor BSP, heightmap sculpt for outdoor, entity placement, light authoring). Tight integration with our ECS, physics, scripting, and networking — multi-user co-op editing falls out for free. A `.map` (TrenchBroom) importer ships as a courtesy one-way bridge under `tools/lm_mapimport/`, not as a dependency. See §10.8.
- **ADR-005:** ~~Open-world coords — double precision everywhere, or fixed-point integer grid?~~ **Not applicable.** Maps are capped at 16 km × 16 km (§9.2). float32 with per-frame render-relative origin re-centering is sufficient; no double-precision world bookkeeping needed.
- **ADR-006:** ~~Planet tile data source — purely procedural, or import real-world terrain (SRTM)?~~ **Not applicable.** No planet-scale terrain. Outdoor worlds are flat heightmaps.
- **ADR-007:** ✅ **Decided.** No Embree, anywhere in the engine runtime. We roll our own software raytracing core — BVH8 builder (SAH), 8-wide packet traversal (AVX2 / NEON), refit, denoiser, tile-pipeline integration — all written from scratch. Embree's build complexity, weak Apple Silicon NEON support, and incompatibility with our DOTS / own-allocator stance ruled it out. (The standalone `tools/dmonte_pt/` reference renderer keeps its Embree dependency since it's optional and offline-only.)
- **ADR-008:** ✅ **Decided.** Outdoor terrain has **two backends, selectable per-map**: **polygon CDLOD mesh** (default for racing tracks; integrates cleanly with spline road geometry and detail textures) and **heightmap raymarcher** (default for IGI / Delta Force-style tactical FPS maps; NovaLogic Voxel Space-inspired per-column raymarch — view distance is free, perfect fit for our tile-parallel architecture). Both backends populate the same tile Z-buffer, so polygon meshes (soldiers, vehicles, buildings, props) composite identically over both. Shadow rays against raymarched terrain use a second occlusion path: direct heightmap raymarch, cheaper than BVH (§8.2). Per-map config `terrain_backend = mesh | raymarch` is fixed at map load. See §9.2.
- **ADR-009:** ✅ **Decided.** Three-surface UI architecture: **(a) React + TypeScript in Chrome app windows** (out-of-process, multi-monitor dockable via OS window management, real Chrome DevTools, hot-reload via Vite) for editor panels — connects to the engine via local WebSocket + msgpack on `127.0.0.1:7654`. **(b) Minimal immediate-mode UI** (Dear ImGui-style, our own, ~few hundred LoC) in-engine, *only* for in-viewport overlays that must draw into the game framebuffer (perf graphs, hitbox viz, manipulator gizmos, brush previews). **(c) RmlUi** (MIT-licensed HTML/CSS subset, vendored) in-engine for player-facing HUDs, menus, cockpits, scoreboards — designer-authored, hot-reload via asset VFS, FreeType for fonts. React panels live in `engine/editor/web/` (Vite-built TypeScript), bundled as static assets at build. Component schemas auto-bind to React inspector forms via `PSYNDER_COMPONENT(...)` (§3.3). Data flow is one-directional engine → UI across all three surfaces. See §10.6 and §10.8.
- **ADR-010:** ✅ **Decided (2026-05-27).** Physics `World` is **instance-owned, not a process-global singleton**. `physics::World` is a constructible PIMPL (`std::unique_ptr<detail::WorldImpl>`) whose `WorldImpl` aggregates ALL simulation state — rigid-body `WorldState`, the vehicle world, and the character world (previously three separate file-static singletons). The editor's `PlayRuntime` owns one `World` and re-constructs it on each Play-begin, so play sessions are isolated, tests are deterministic, and multiple simulations can coexist (a two-instance isolation test guards this). Public instance methods (`create_body`/`step`/`get_*`/…) are unchanged; the `vehicle::`/`character::` free functions gained a trailing `World& world = World::Get()` so legacy callers compile untouched. `World::Get()` is **retained as a documented back-compat default-world accessor** (the reference samples + physics bench/tests still use it) — NOT shared mutable state across distinct instances. Chosen as "Option B now, shaped to become C later": the owner can move from `PlayRuntime` → a per-scene simulation-resource object without further API churn, which is the path to multi-scene editing + server-authoritative networking. Generation-safe body/character/vehicle/joint handles (also landed) make stale handles across instances / play restarts safe. See §10.1.

- **ADR-012:** ✅ **Decided (2026-05-30).** **Indoor visibility ships a RUNTIME PVS *builder* (leaf-portal flood) alongside the offline-baked PVS path.** Until now the only source of a Potentially-Visible-Set was the offline `lm_qbsp` compiler (lane 24): it emits per-cluster PVS bit-vectors into the `.psybsp` blob, and `world::bsp::walk_visible_leaves` consumes them at runtime. That covers shipped, compiled maps but leaves a hole for any BSP *assembled in memory* — a procedural arena, an editor preview, or a code-authored demo level (the `psynder_duke_demo` reference game) — which has a `BspMap` + `BspPortalSet` but no PVS rows. We add `world::bsp::build_pvs` (`engine/world/bsp/PvsBuild.{h,cpp}`) which derives the per-cluster PVS table from the leaf-portal adjacency graph by **flood-fill**: for each cluster, BFS the portal graph from its leaves and mark every portal-reachable cluster visible. This is the well-known *coarse* "base PVS" first stage of Quake VIS (Seth Teller, *Visibility Computations in Densely Occluded Polyhedral Environments*, 1992; John Carmack's `qvis`), which itself computes a portal flood before the expensive separating-plane antipenumbra refinement. We deliberately stop at the flood stage because the engine already owns the refinement at *runtime*: `PortalClip.cpp`'s `walk_portal_visible_leaves` does per-frame portal-frustum clipping, so coarse-flood PVS + runtime portal clip together approximate full Quake VIS without the offline cost. **Key properties:** the flood is integer-only (BFS over leaf indices, no floating point), so output bit-vectors are **bit-identical across runs/arches**; it is **alloc-free** in the steady state via a caller-owned `PvsBuildScratch` (CSR adjacency + BFS queue + visited flags reserved once, reused on every rebuild — so even a mid-game portal change like opening a door rebuilds the PVS with zero heap traffic); and it is **conservative** (over-include is correct, under-include is a render bug — the cardinal PVS safety rule). The output `pvs_row_bytes`/row-major layout exactly matches the on-disk convention, so a built table drops straight into `BspMap::pvs` and is consumed by the same `walk_visible_leaves` as a compiled map. Rejected alternative: only ever bake PVS offline and forbid in-memory BSPs — that blocks procedural/editor/preview levels and the demo milestone for no benefit, since the flood is ~50 lines and fully deterministic. Guarded by `tests/unit/world_bsp_pvs_build.cpp` (flood correctness, drop-in `walk_visible_leaves` compatibility, determinism-on-reuse, portal-free island case) and exercised end-to-end by `games/duke_demo` (a sealed vault cluster is PVS-culled until a HUB trigger opens its portal and rebuilds the PVS). See §10.8.

- **ADR-011:** ✅ **Decided (2026-05-30).** **Box-box / capsule-box speculative anti-tunnelling uses a GJK *distance* sub-algorithm gated by a near-contact band.** The closed-form separation kernels (sphere-sphere, plane-shape) returned `+inf` for any convex pair without a closed-form gap (box-box, capsule-box), so those pairs got NO speculative contact and a fast box could tunnel through another box in one sub-tick. We add a from-scratch GJK distance routine (`gjk_distance`, Gilbert-Johnson-Keerthi 1988 / Ericson RTCD §9.5 — closest-point-to-origin on the Minkowski-difference simplex, reusing the existing `GjkSupport`/`mink_support` so the boolean/EPA path and the distance path see byte-identical supports). It is deterministic + alloc-free (fixed 4-vertex simplex, bounded 32-iteration march, no heap/RNG/clock). **Key decision: a 10 cm near-contact band.** GJK's witness-point normal *degenerates* for nearly-parallel faces (two large flat boxes meeting face-to-face have an under-determined closest feature), and feeding that jittery normal to the speculative one-sided speed-limit lets a *settling* box creep through its support (observed: a parented dynamic box sinking to y≈-83 instead of resting). So `kernel_pair_separation` returns the GJK gap only when `distance >= kGjkContactBand (0.1 m)`; a smaller gap is handed to the proven EPA overlap path, whose face-contact normal is stable. The band exceeds the per-tick approach of a normal-speed landing (so EPA reliably catches it within a tick), while genuinely tunnelling pairs are caught far earlier — at gaps ~`closing*dt` that dwarf the band — where the GJK normal is well-conditioned. Touching/penetrating pairs return `+inf` → the unchanged overlap path, so resting stacks are byte-identical (regression-tested by memcmp). Rejected alternative: making `gjk_distance` itself robustly detect origin-containment + emit stable face normals (full EPA-grade contact manifold) — more code + risk for no gameplay gain, since EPA already owns the at-contact regime. Guarded by `tests/unit/physics_gjk_speculative.cpp` (distance correctness, speculative gating, end-to-end no-tunnel, resting no-regression). See §10.1.

- **ADR-012:** *(reserved — graph-coloring storage / SoA contact batching lane.)*

- **ADR-013:** ✅ **Decided (2026-05-30).** **The per-island constraint solve is parallelised by deterministic graph colouring, not by SIMD-ing the existing sequential-impulse loop.** *Context.* The legacy per-island solver is sequential-impulse / projected Gauss-Seidel (PGS): contact *i* reads the body velocities contact *i-1* just wrote. That read-after-write chain is what gives PGS its good convergence, but it also makes the inner velocity-iteration loop *strictly serial* — there is no way to SIMD-batch or thread it while reproducing the exact same numbers, because every constraint depends on its predecessor's writes. (This is why the earlier "solver SIMD" task was honestly deferred — see the Wave-8 ledger.) The only real win requires a **deliberate re-ordering** of the solve, which changes the exact trajectory bit-values *by design* while staying physically correct and deterministic. *Decision.* Within an island we build a deterministic **k-colouring of the constraint graph**: one vertex per contact, an edge between two contacts iff they share a *dynamic* body (static bodies, `inv_mass == 0`, are never written so they are not conflicts). A proper colouring guarantees every contact of one colour touches pairwise-disjoint dynamic bodies, so a colour's impulse applications can run in any order — including fully in parallel via the job system's `parallel_for` — and the body-velocity / body-position writes **never race, by construction (not merely by ASan luck)**. Colours are then solved **sequentially** (colour 0 fully, then colour 1, …): each colour sees the velocities the previous colour wrote, so the scheme stays Gauss-Seidel *across* colours (good convergence, unlike pure Jacobi) while being Jacobi *within* a colour. This is the standard batched-constraint technique (Tonge, "Solving Rigid Body Contacts", GDC 2012; Coumans' Bullet parallel constraint batching; the general "graph colouring for parallel impulse solvers" result). *Determinism (hard requirement, §10.1).* The colouring is a pure deterministic function of the input contact array — greedy in **ascending contact-index** order, each contact takes the smallest colour not used by a lower-index contact sharing a dynamic body; within a colour the contacts are emitted in ascending index order (a stable counting-sort into CSR buckets). No RNG, no clock, no address-derived ordering, no atomics in the colouring. The `parallel_for` within a colour is race-free, so its result is **independent of how the range is partitioned and of thread scheduling** — therefore the serial dispatcher and the multicore dispatcher produce **bit-identical** body state (the bench asserts this by `memcmp`), and two runs reproduce bit-for-bit. **The exact trajectory bit-values DIFFER from the pre-ADR-013 plain-index PGS** because the solve order changed — this is intended; the run-vs-run determinism test (`physics_deterministic.cpp`) compares the new solver against itself and still passes, and the physical-behaviour tests (resting stacks settle, governors hold, vehicles drive, speculative anti-tunnelling holds) assert tolerances, not old bits, and remain green. *Mechanics.* `kernel_color_island` + scratch live in `engine/physics/internal/SolverColoring.h`; the colored PGS core (`solver_solve_island_core`) and the two entry points — serial `kernel_solve_island` (tests + small-island fallback) and `kernel_solve_island_colored` (production, takes a per-colour `ColorBatchDispatch`) — live in `engine/physics/internal/Kernels.h`. All scratch (constraint cache, per-body colour-usage bitset with an O(1) stamp-clear, CSR colour buckets) is **caller-pooled on the `WorldState` (`island_solver_scratch`, one `ColoredIslandScratch` per island) — zero per-frame heap on the hot path**. `World.cpp::run_island_solve` (the only function touched there) binds the dispatcher to the job system's `parallel_for` for islands at or above `kColoredParallelThreshold (2048 contacts)` and to the serial dispatcher below it; *inside* the parallel dispatcher a colour smaller than `kColoredColorMinParallel (512)` runs inline (no task submit) and parallelised colours use a large `kColoredParallelGrain (512)`. **Why those gates matter (an honest perf finding):** the per-contact solve is *light* (tens of ns), so an optimised serial colour walk is already fast and a `parallel_for` fork/join (~3-4 us measured) only pays off when a colour is big. Without the size gates the parallel path *loses* in release (over-submitting tiny colours every iteration); with them it wins on large islands and typical game-sized islands simply stay on the cheap serial-colored path. Bench (`tests/bench/physics_solver.cpp`, a 29 800-contact / 10 001-body single island on 15 workers): **release ≈ 1.7x (min) / 1.08x (mean — the parallel path is scheduler-jittery); debug+ASan ≈ 3.6x** (ASan slows the serial arithmetic, inflating the apparent win) — and serial == parallel **bit-for-bit** in every case (the bench asserts it). The modest release number is the truthful one: graph-colouring buys real but bounded multicore scaling here because the constraint kernel is memory-light, not because the parallelism is broken. *Rejected alternatives.* (a) Bit-identical SIMD of the existing loop — impossible (the RAW dependency above). (b) Pure Jacobi (all contacts in parallel, no colours) — trivially parallel but converges poorly and visibly softens stacks at the same iteration budget. (c) Per-island job parallelism only (the prior state) — leaves a single large island fully serial, which is exactly the worst case this ADR targets; colouring adds intra-island parallelism on top. See §10.1.

- **ADR-014:** ✅ **Decided (2026-05-30).** **The indoor BSP asset pipeline ships end-to-end: a `.rooms` level source is compiled OFFLINE by `lm_qbsp` into the engine `.psybsp` (PBSP v1) format *with a baked PVS*, and `psynder_duke_demo` loads that on-disk map at runtime instead of authoring an in-memory `BspMap`.** *Context.* Two `.psybsp` dialects had silently diverged. The offline compiler `tools/lm_qbsp` emitted its own `PSBP` v2 blob (planes / nodes / leaves / brushes / portals from a Quake brush `.map`) — but with **no faces, no vertices, and no baked PVS**. The runtime loader `world::bsp::Bsp::load` reads a *different* layout — `PBSP` v1 (`engine/world/bsp/BspFormat.h`): nodes / leaves / faces / vertices / indices **plus a baked per-cluster PVS bit-vector table**. So the brush path could not feed the runtime (wrong magic, wrong chunks, no PVS), and in fact **no game ever loaded a `.psybsp` from disk** — `duke_demo` assembled a `BspMap` + `BspPortalSet` in code and flooded the PVS at runtime (ADR-012). The asset pipeline existed only on paper. *Decision.* Close the loop with a **minimal additive extension to `lm_qbsp`** (the brush path and its `PSBP` v2 format are untouched): (1) a small deterministic **`.rooms` source format** — axis-aligned room/corridor volumes + explicit portals — which is the on-disk authoring of exactly what `duke_demo` used to build in code (`assets/maps/duke_e1m1.rooms`); (2) `compile_rooms`, which builds a leaf-per-room BSP with a **median-split kd-tree of nodes** (so `Bsp::locate` descends to the correct room) and a portal table from the explicit connections; and (3) `write_psybsp_engine`, which **bakes the PVS by reusing the engine's own `world::bsp::build_pvs`** (the same Quake-style leaf-portal flood from ADR-012 — no duplicated algorithm to drift) and serialises the loader's `PBSP` v1 layout. **Faces / vertices / indices are emitted empty on purpose:** the runtime renders its own scene-mesh boxes and uses the BSP purely for leaf / cluster / PVS visibility culling (mirroring the in-memory demo, whose `BspMap` also carried zero faces). *Build wiring.* The bake runs **at build time** via an `add_custom_command` that invokes `lm_qbsp --rooms` and writes the `.psybsp` under `${CMAKE_BINARY_DIR}/assets/maps/` (following the existing `tools/lm_pak` fixture-cook convention — derived binaries live in the build tree, not checked in); `duke_demo` mounts that directory (and a source-tree fallback) and loads `maps/duke_e1m1.psybsp` at startup. *Properties.* The flood is integer-only so the baked PVS is **bit-identical across runs / arches**; the demo's headless `--smoke-frames` gate is now `(map_loaded && pvs_culled && kills>0)` and observes the loaded map's leaf/cluster counts + a genuine PVS cull (visible 5 / 7 leaves: the `.rooms` source intentionally **seals the VAULT chain** so clusters 4–5 are PVS islands and the baked table culls them). *Deferrals.* The on-disk PVS is **static**, so `duke_demo`'s in-game "vault door opens" event (which in the in-memory path re-floods the PVS to reveal the sealed vault, ADR-012) is HUD-only when the map is loaded — dynamic re-baking of a loaded PVS row (or shipping a second post-door PVS cluster set) is left for a later wave. The demo keeps the full in-memory authoring path as an automatic fallback when the asset is missing. *Rejected alternatives.* (a) Make the loader read the `PSBP` v2 brush format — would force the runtime to grow a brush→face tessellator + an online PVS bake, i.e. move the offline compiler's job into the engine, the opposite of the BSP-asset goal. (b) Check the baked `.psybsp` into the repo — no precedent for committed binary assets here, and a build-time bake stays reproducible from source. (c) Author via the brush `.map` path — its centroid-approximate classifier produces an unpredictable leaf set with infinite bounds, unsuited to a clean multi-room PVS showcase; the `.rooms` format gives finite per-room bounds + one cluster per room deterministically. Guarded by `tests/unit/tools_lm_qbsp_rooms.cpp` (parse, duplicate-cluster rejection, compile topology, PBSP v1 header + baked-PVS dimensions, and a full **lm_qbsp → write → Vfs-mount → `world::bsp::load` → `locate` → `walk_visible_leaves`** round-trip asserting the sealed room is culled) and exercised end-to-end by the `psynder_duke_demo` smoke. See §10.8.

---

## 17. Prior art & inspiration

- Michael Abrash, **Graphics Programming Black Book**.
- Fabian Giesen, **A trip through the graphics pipeline** + **Optimizing the basic rasterizer**.
- The **Quake / Quake II / Quake III source releases** (GPLv2) — under `third_party/quake-refs/` for study, never linked.
- **Larrabee** papers (Abrash, Forsyth, Seiler) — sort-middle tile rasterization on CPU.
- **pbrt-v4** book — BVH and sampling primitives.
- **Akenine-Möller, Real-Time Rendering, 4th ed.** — general reference.
- **NFS3 file format wikis** (community FCE/FSH reverse-engineering).
- **Project IGI** / **Delta Force** retrospectives — kilometre-scale heightmap landscapes on 1999-era hardware.
- **dmonte path tracer** — author's prior CPU path tracer; a handful of low-level kernels (intersect, RNG, SAH skeleton) are reused.

---

## 18. What "done" looks like

When Psynder hits 1.0 we should be able to record a 90-second sizzle reel:

1. A driver pulling out of a tunnel into rain at night, headlights raytracing crisp soft shadows onto wet asphalt.
2. Cut to a dim indoor map, flashlight beam casting per-pixel shadows from a single moving light, lightmaps holding the bounce.
3. Cut to a Project IGI / Delta Force-style outdoor map at dawn, kilometre view distance, soldier patrolling a ridge, scripted helicopter buzzing overhead, long shadows from a low sun.
4. Press `~` mid-frame, flip into editor mode without a reload, spawn a watchtower, weld a rocket to it, watch the physics fling debris across the heightmap as the raytracer keeps the shadows crisp.
5. End on three terminals side-by-side — a Ryzen 9, an Intel Core Ultra, an M3 Max — all running the same binary, all at 60 FPS, all without a GPU shader.

That's the bar.

---

## 19. Implementation handoff

This doc is complete enough to start building from. If you're an implementer (human or agent) picking this up cold, here's how to use it.

### 19.1 What to read first

1. **§1 Vision** — design pillars + non-goals. Internalize the non-goals especially; many later decisions only make sense in light of what we explicitly chose not to build.
2. **§3 DOTS mandate** — the architectural contract. Every subsystem follows the split: strict DOTS in `render/`, `scene/ecs/`, `physics/`, `audio/mixer/`; pragmatic-DOTS everywhere else; strict DOTS for the public game/script API.
3. **§16 ADR log** — the ADR slots are the load-bearing answers. Each one closes off a question a contributor would otherwise relitigate. Twelve decided (1, 2, 3, 4, 7, 8, 9, 10, 11, 12, 13, 14), two not-applicable (5, 6).
4. **§13 Milestone roadmap** — the implementation order. Demos at every milestone; no engine work without a screenshot to show for it.

### 19.2 What to start with — M0 bring-up

The first task is concretely:

1. Create the repo scaffold per the layout in §12 (`engine/`, `tools/`, `samples/`, `tests/`, `third_party/`, `docs/`).
2. CMakeLists.txt + CMakePresets.json for all three OS × arch combos. Clang ≥ 17 primary on all three.
3. `.clang-format`, `.clang-tidy` with the custom DOTS check set (§3.6 enforcement).
4. CI matrix in `.github/workflows/` (Windows / Linux / macOS Apple Silicon).
5. `engine/core/` + `engine/math/` + `engine/simd/` skeletons (empty headers, asserting compile-time invariants).
6. Open a window on each platform (Win32 / Wayland-or-X11 / AppKit) and clear it to an animated color via the framebuffer scaling-blit path described in §11.

When `sample 00` renders an animated clear color on all three OSes from one binary, M0 is done.

### 19.3 What to *not* build first

These are tempting rabbit holes that don't pay off until later milestones — defer:

- Raytracing (M5). Don't start the BVH8 builder until the rasterizer is stable in M2.
- Open-world / planet / flight-sim scaffolding. These are explicit non-goals (§1); ignore any past mentions you may find in branches.
- A "wrapper" abstraction over Vulkan / Metal / D3D. We don't render with GPU APIs. The platform-layer surface is the framebuffer present, nothing else.
- A general-purpose physics engine before vehicle physics. The rigid-body core is necessary; soft bodies / cloth / fluids are post-1.0.
- Asset pipeline grandeur. `cinder_cook` accepts `.obj`/`.gltf`/`.png`/`.wav`. Resist the urge to support FBX, USD, OpenEXR, etc.

### 19.4 Hard rules

These are enforced by clang-tidy, code review, and CI. They're listed across the doc but worth reasserting in one place for implementers:

- No GPU code, ever, anywhere in `engine/` runtime. (See §1, §11, ADR-007.)
- No `malloc` / `new` / `delete` in the frame loop. (See §4.1.)
- No `virtual` in `engine/render/`, `engine/scene/ecs/`, `engine/physics/`, `engine/audio/mixer/`. (See §3.4 forbidden patterns.)
- No `std::shared_ptr` in `engine/` runtime. (Tools and editor only.)
- Per-tile budget regressions > 2% must justify or fail CI. (See §13 coding standards.)
- Bench gate runs all three tile-size specializations (32×32, 64×64, 128×128). (See ADR-002.)
- ASan-instrumented build is in the CI matrix. (See §4.7.)

### 19.5 Order of subsystem dependencies

Subsystems depend strictly upward:

```
core → math → simd → jobs → asset → scene → render → audio → game
                                       ↑
                                   physics
                                       ↑
                                       ui (imm + rml)
                                       ↑
                                    editor
```

Bring up bottom-up. A subsystem can be stubbed (return canned values) before its dependents land, but the API shape should be settled before serious implementation begins on anything above it.

### 19.6 If something contradicts itself

If two parts of this doc disagree, the **later-numbered ADR wins** and the design doc has drifted — open an issue and update the doc before writing code. Don't paper over contradictions in implementation; they'll bite later.

### 19.7 Open questions deliberately left for implementation

The doc commits to *what* and *why*; some *how* details are intentionally not specified:

- Exact lock-free queue implementation (Chase-Lev work-stealing deque — pick a known-good reference impl).
- Exact ImGui-style API shape (look at Dear ImGui as the reference; the in-viewport overlay surface needs maybe 20 widgets, not the full library).
- Exact RmlUi version pin (latest stable at integration time; vendor as a submodule).
- Exact Lua binding library (sol2 or LuaBridge; pick the one with the cleanest C++23 support).
- Exact React component library (shadcn/ui or Radix or HeadlessUI; pick what the editor designer prefers — it's TypeScript and easy to swap).
- Exact WebSocket library on the C++ side (uWebSockets, websocketpp; pick the smallest dep with C++20+ support).
- Exact msgpack library (msgpack-c on C++ side, @msgpack/msgpack on TypeScript side).

These are micro-decisions that don't deserve ADRs — make them with good judgment.

### 19.8 Dev environment prerequisites

For the editor to work, the contributor's dev machine needs:

- A C++23-capable compiler (Clang ≥ 17 primary).
- CMake ≥ 3.28, Ninja, vcpkg.
- Node.js ≥ 20 + pnpm (for the React panels under `engine/editor/web/`).
- Chrome or Chromium installed and on PATH (the engine launches it via `--app=` for each panel).
- Standard platform SDKs (Win SDK on Windows, the Wayland/X11 headers on Linux, Xcode on macOS).

For *shipping* a game built on Psynder, none of the editor / Node / Chrome prerequisites apply — just the engine + game executable + assets.

---

*— end of design v1.0 (handoff)*
