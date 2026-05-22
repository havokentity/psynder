# Psynder Math Kernels

This document captures the current plan for high-performance authored math:
gameplay math, visual graph math, scripted math, and cooked native math all use
the same IR and execution pipeline.

## Goals

- Let app developers write math once, in a small expression-recording API, then
  run it over packed SoA streams.
- Keep live editor authoring instant without requiring a compiler.
- Let shipping builds reach handwritten SoA performance through generated C++.
- Keep runtime JIT optional. Psynder should not require bundled Clang/LLVM.
- Preserve DOTS/ECS data flow: streams are explicit, cache-linear, and bound at
  execution time.

## Current Runtime Shape

`MathLogicKernelBuilder` records a small IR at init time:

```cpp
MathLogicKernelBuilder builder;
builder.begin_record();

auto position = builder.vec3_stream(0);
auto movement = builder.vec3_stream(1);
auto accel = builder.vec3_stream(2);
auto dt = builder.f32_uniform(0);
auto mass = builder.f32_uniform(1);

movement = movement + accel * dt * mass;
position = position + movement * dt;

MathLogicKernel kernel = builder.end_record();
```

At runtime the caller binds real SoA streams:

```cpp
kernel.set_f32_uniform(0, frame_dt);
kernel.set_f32_uniform(1, ball_mass);

std::array streams{
    positions.mutable_view(),
    movements.mutable_view(),
    accelerations.mutable_view(),
};
kernel.execute(streams);
```

The kernel does not loop over entities in user code. It executes across the
bound stream count.

## Frontends

All frontends should lower to the same math IR:

| Frontend | Purpose |
| --- | --- |
| C++ recording API | Native engine/game systems and tests. |
| Web visual graph | Hand-authored editor graph for designers and developers. |
| Script/VM graph | Runtime/editor experiments and hot reload. |
| Cooked graph assets | Serialized graph IR for projects and packages. |

The visual graph must not be a separate executor. It is an authoring frontend
for the same IR used by C++ recording.

## Execution Backends

| Backend | Compiler required? | Purpose |
| --- | ---: | --- |
| VM / bytecode | No | Instant live editing, dynamic graphs, debug fallback. |
| Pattern-lowered function pointers | No | Fast editor/runtime path for common graph shapes. |
| Generated C++ SoA | Yes, at project build/cook time | Shipping/highest-performance path. |
| Hot-loaded native plugin | Yes, installed locally | Optional editor native hot reload. |
| Runtime LLVM/Clang JIT | Avoid for now | Only if future requirements justify it. |

The intended default is VM for immediate feedback and generated C++ for cooked
or shipping builds.

## Compiler Pipeline

```text
Frontend
  -> Math IR graph
  -> Validation
  -> Optimizer / lowerer
       - stream alias checks
       - uniform folding
       - temp elimination
       - madd fusion
       - chained update fusion
       - pattern recognition
  -> ExecutionPlan
       - generated/template native kernel
       - function-pointer lowered kernel
       - bytecode fallback
```

The current runtime already has an interpreted/fused `MathLogicKernel` path and
a pattern for:

```text
movement += accel * dt * mass
position += movement * dt
```

That pattern lowers to one chained stream loop.

## Generated C++ Path

The build/cook compiler should emit direct SoA loops that the normal C++
compiler can optimize:

```cpp
extern "C" void psy_kernel_execute(PsyKernelBindings* bindings, usize count) {
    const float accel_step = bindings->f32_uniforms[0] * bindings->f32_uniforms[1];

    for (usize i = 0; i < count; ++i) {
        const float mx = bindings->vec3[1].x[i] + bindings->vec3[2].x[i] * accel_step;
        const float my = bindings->vec3[1].y[i] + bindings->vec3[2].y[i] * accel_step;
        const float mz = bindings->vec3[1].z[i] + bindings->vec3[2].z[i] * accel_step;

        bindings->vec3[1].x[i] = mx;
        bindings->vec3[1].y[i] = my;
        bindings->vec3[1].z[i] = mz;

        bindings->vec3[0].x[i] += mx * bindings->f32_uniforms[0];
        bindings->vec3[0].y[i] += my * bindings->f32_uniforms[0];
        bindings->vec3[0].z[i] += mz * bindings->f32_uniforms[0];
    }
}
```

This should be generated into the project build tree and compiled by the normal
toolchain. The editor does not need to ship a compiler for this path.

## Optional Editor Native Hot Reload

For users with a compiler installed, the editor can compile generated kernels
into a versioned shared library and hot-load it:

| Platform | Toolchain detection | Output | Loader |
| --- | --- | --- | --- |
| Windows | `vswhere`, MSVC Build Tools, `cl.exe`, `clang-cl` | `.dll` | `LoadLibraryW` |
| macOS | Xcode Command Line Tools, `clang++` | `.dylib` / `.bundle` | `dlopen` |
| Linux | `clang++` / `g++` | `.so` | `dlopen` |

Rules:

- Export a stable C ABI only.
- Do not pass STL or C++ objects across the plugin boundary.
- Use graph hash/build number in the output filename to avoid Windows file
  locking during reload.
- Compile in the background; VM/lowered execution remains active until native
  output is ready.
- Treat native hot reload as an editor acceleration, not a shipping dependency.

## VM Branch Status

As of this document, no branch named `vm` exists on `origin`. The closest
branches are:

- `origin/lane/15-script`
- `origin/lane-b/15-script`
- `origin/lane-b/19-editor-ipc`
- `origin/lane-b/20-editor-web`

The visible script lane branches appear to be old divergent snapshots relative
to `integration/wave-hybrid-material-shadows`; their diffs include broad
cross-repo deletions. They should not be merged directly into the current
integration branch. If there is a newer VM branch under another remote/name, use
that exact ref. Otherwise, port VM concepts manually into the current structure.

## Bench Snapshot

From `psynder_bench_math_logic_kernel` on Apple Silicon:

| Balls | AoS ns/ball | Hand SoA ns/ball | Kernel ns/ball | Kernel vs AoS | Kernel vs SoA |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 1.982 | 3.926 | 12.156 | 0.16x | 0.32x |
| 10 | 0.857 | 1.500 | 2.219 | 0.39x | 0.68x |
| 100 | 0.835 | 0.237 | 0.461 | 1.81x | 0.51x |
| 10000 | 0.967 | 0.627 | 0.627 | 1.54x | 1.00x |

Interpretation:

- Tiny counts need a scalar/direct fast path.
- Large counts are already near handwritten SoA for the fused ball-motion case.
- AoS is beaten once the stream count is large enough.

## Next Work

1. Add a stable C ABI for native math kernel plugins.
2. Add a generated C++ emitter for the current `MathLogicKernel` IR.
3. Add toolchain detection for MSVC/clang/gcc as an editor service.
4. Add dynamic library loading with versioned output names.
5. Add function-pointer execution plans so lowered patterns avoid bytecode
   dispatch.
6. Add scalar/direct fast paths for very small counts.
7. Add `f32_stream()` so per-entity mass and scalar attributes stay SoA.
8. Add visual graph serialization that maps directly to Math IR.
9. Port useful VM-branch concepts manually after identifying the right branch or
   files.
