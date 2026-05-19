# scripts/

Developer helpers. Most CI / build wiring lives in `cmake/` and
`.github/workflows/`; this directory is for ad-hoc dev scripts.

- `smoke_sample.sh` — acquires the `/tmp/psynder_smoke.lockdir` mutex and
  runs a sample binary headless for N frames (Mac parallel-agent safety).
- Per-lane fixture scripts may land here as lanes file PRs.
