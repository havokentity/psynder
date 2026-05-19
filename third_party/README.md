# third_party

Vendored third-party sources land under this directory; the build system
prefers vcpkg manifest mode and FetchContent before falling back to vendoring.

Plan (lane responsibility):

- `fmt` — Lane 01 (core). Auto-fetched via `cmake/Dependencies.cmake`.
- `zstd` — Lane 05 (asset). vcpkg or FetchContent.
- `lua` — Lane 15 (script). vcpkg or FetchContent.
- `freetype` — Lane 17 (ui-rml). vcpkg or FetchContent.
- `rmlui` — Lane 17 (ui-rml). Submodule.
- `tracy` — optional, gated by `PSYNDER_ENABLE_TRACY`.
- `catch2` — Lane 25 (tests). Auto-fetched.
- `quake-refs` — read-only reference source under GPL; **never linked into the engine binary**.
