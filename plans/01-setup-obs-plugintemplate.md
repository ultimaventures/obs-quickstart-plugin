# Plan: Initialize and Adapt OBS Plugin Template

## Objective
Initialize the project codebase by adopting the `obs-plugintemplate`, converting it to C++17, and refactoring it to match the modular architecture defined in `obs-quickstart-plugin/architecture.md`.

## Prerequisites
- `obs-quickstart-plugin/` documentation (Reviewed)
- `obs-plugintemplate` repository URL: `https://github.com/obsproject/obs-plugintemplate`

## Implementation Steps

### 1. Initialize Repository from Template
- **Action:** Clone `obs-plugintemplate` into a temporary directory.
- **Action:** Move the template files into the project root, excluding `.git`.
- **Action:** Ensure `obs-quickstart-plugin/` and `plans/` are preserved.

### 2. Convert to C++ and Reorganize Structure
- **Action:** Rename `src/plugin-main.c` to `src/plugin-main.cpp`.
- **Action:** Wrap OBS entry points in `extern "C"` in `src/plugin-main.cpp`.
- **Action:** Create module subdirectories in `src/`:
  - `src/detection/`, `src/profile/`, `src/settings/`, `src/sources/`, `src/filters/`, `src/monitoring/`, `src/ui/` *(the `/network` speed-test module was removed from scope 2026-09-28 and its directory deleted 2026-09-29 — see ADR-002)*
- **Action:** Create placeholder headers/sources for each module.

### 3. Update CMake Configuration
- **Action:** Modify root `CMakeLists.txt`:
  - Set project name to `obs-quickstart-plugin`.
  - Set `LANGUAGES CXX`, `CMAKE_CXX_STANDARD 17`, and `CMAKE_CXX_STANDARD_REQUIRED ON`.
- **Action:** Update `src/CMakeLists.txt` to include new subdirectories and `src/plugin-main.cpp`.
- **Action:** Configure dependencies (`libobs`, `Qt6`, `nlohmann/json`).

### 4. CI/CD Migration
- **Action:** Remove `.github/` directory.
- **Action:** Create `.gitlab-ci.yml` with stages for `build`, `test`, and `package`.
- **Note (2026-03-15):** Mid-build change - We decided to host the repository in GitLab but use GitHub Actions for building releases and CI. The `.github/` directory will be restored/maintained, and `.gitlab-ci.yml` will be kept as a minimal placeholder or for basic checks.

### 5. Documentation Updates
- **Action:** Update `STACK.md` with C++17 justification.
- **Action:** Update `CONVENTIONS.md` with C++ include order rules.

### 6. Cleanup & Standardization
- **Action:** Update `.gitignore`.
- **Action:** Apply `clang-format 19` to all files.

## Verification
1. **Build Check:** Run `cmake -S . -B build` and `cmake --build build`.
2. **Structure Check:** Verify all module folders exist in `src/`.
3. **Log Check:** Load in OBS and verify "Plugin loaded" message.

## Scope Decisions (2026-09-28)

The following scope decisions were made after this plan was completed, based on research into OBS's built-in Auto-Configuration Wizard (verified against OBS Studio 31.0.0 source). They are recorded here for planning continuity; the normative detail lives in `docs/architecture.md`, `docs/flowchart.md`, `docs/functionality.md`, and `README.md`.

**Deferred to OBS's Auto-Configuration Wizard (recorded in the `docs/decisions.md` ADR log):**
- Base/output resolution and FPS selection
- Encoder selection (NVENC/AMF/QSV/VideoToolbox/x264)
- Upload-speed-driven bitrate calculation and the `/network` speed-test module
- Rationale: the wizard runs real per-server bandwidth tests with scoring and top-down encoding probes — battle-tested, and it already runs on first launch. Rebuilding it adds maintenance for no gain.

**New plugin flow:** setup creates a `Quickstart` profile (deduplicated), switches to it, and triggers the wizard on it — programmatically via `QMetaObject::invokeMethod` on `on_autoConfigure_triggered` (no public frontend API; mirrors OBS's own first-run launch), with a manual Tools-menu fallback and a "copy my current video settings" skip option.

**Confirmed in scope (wizard doesn't set these):** 48 kHz/stereo audio, NVENC preset adjustment, scenes/sources, transitions, MKV + auto-remux, alerts/multistream assistance, webcam detection. Keyframe interval is intentionally NOT set by the plugin — in Simple output mode the streaming service applies its recommended interval automatically (see `docs/architecture.md`).

**New scope added:**
- `/audio` — setup-time mic check (one-click assign, live meter), persistent mic reminder on every OBS launch until configured, and a Tools-menu Mic Troubleshooter (automated checks + guided OS-level checklist).
- `/hotkeys` — scene hotkeys Ctrl+Shift+1–6 (opt-in, not bound by default) plus a "Quickstart Shortcuts" reference dock (View > Docks; window chrome only, never in program output).
- Stability test reframed as validation: auto-runs as the final performance-validation step (not skippable), retries adjust the encoder's quality control per the per-encoder ladder (NVENC/QSV/AMF/x264; VideoToolbox has no ladder), re-runnable via Tools > Quickstart: Run stability check. Never silently rewrites the wizard's resolution/FPS/bitrate.
- Bundled default overlays under `data/overlays/` (1920x1080, original/CC0 only), placed automatically by SceneBuilder.
