# scripts/

Developer helpers. Most CI / build wiring lives in `cmake/` and
`.github/workflows/`; this directory is for ad-hoc dev scripts.

- `smoke_sample.sh` — acquires the `/tmp/psynder_smoke.lockdir` mutex and
  runs a sample binary headless for N frames (Mac parallel-agent safety).
- `build_release.sh` — rebuilds the web editor bundle, configures the
  `mac-release` preset, and builds the `psynder_arcade` release executable.
- `start_arcade.sh` — launches the `mac-release` Arcade executable from the repo
  root after clearing any stale shared Mac runtime lock.
- `smoke_arcade_editor.sh` — build + focused editor/scene CTest filter + Arcade
  smoke launch, clearing the shared Mac runtime lock for human terminal runs.
- Per-lane fixture scripts may land here as lanes file PRs.
