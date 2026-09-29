# Architecture Decision Records

Short log of scope decisions that removed previously-planned work. The struck-through
sections these replaced have been deleted; this file is the record.

## ADR-001 (2026-09-28): Encoder detection and selection owned by OBS's Auto-Configuration Wizard

Removed: encoder enumeration, priority logic (NVENC > AMF > QSV > x264 and per-platform
variants), encoder bits in /detection, related unit tests.

Rationale: the wizard probes encoders against real hardware (top-down encoding probes,
CPU-tier caps) — battle-tested over years. Duplicating it adds maintenance burden for no gain.

## ADR-002 (2026-09-28): No built-in network speed test

Removed: the /network module, SpeedTestWrapper, `src/network` build entry
(build entry removed 2026-09-29; the directory is retained but unbuilt).

Rationale: the wizard runs real per-server bandwidth tests against the user's actual
ingest servers with per-server scoring — strictly more accurate than a generic
speed-test API for choosing streaming bitrate.

## ADR-003 (2026-09-28): Resolution / FPS / bitrate calculation owned by the wizard

Removed: bitrate tables, resolution/FPS mappings, "conservative defaults" logic.

/settings now applies only what the wizard leaves untouched: encoder preset,
audio settings, recording configuration.

Note (2026-09-29, verified against OBS master source): keyframe interval was
also removed from /settings. In Simple output mode — which the wizard forces —
OBS has no keyframe-interval config path (`SimpleOutput` never reads one); the
streaming service injects its recommended `keyint` (Twitch: 2 s) automatically.
A plugin-set keyframe would require Advanced output mode — deferred, not MVP.
The NVENC preset ladder now uses the verified p1–p7 scale (default p5), not the
legacy Quality/Performance names.

## ADR-004 (2026-09-28): Custom overlay picker deferred past MVP

The setup flow ships bundled overlays only — no per-scene custom-overlay file picker.
A tutorial for swapping in custom overlays is future work.

Rationale: keep first-run setup simple for non-technical users.

## ADR-005 (2026-09-29): Detection scope is CPU, platform, and webcam only

Removed: GPU VRAM/generation detection.

Rationale: nothing downstream needs GPU specs — encoder choice is the wizard's, and
the stability-test quality ladders are per-encoder, not per-GPU-tier.

## ADR-006 (2026-09-29): Stream Ending overlay is for the actual wind-down

The bundle includes a Stream Ending frame, but it is shown only when the stream is
genuinely ending (raid target, socials, schedule) — never as a mid-stream "ending soon"
warning, which would push new viewers to leave.
