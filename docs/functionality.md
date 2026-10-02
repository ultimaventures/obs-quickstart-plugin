# OBS-Focused Plugin Automatable Setup Guide

This document lists **only steps that a native OBS plugin can automate**, including thresholds and recommended settings.

---


---

## 1. Bitrate + Encoder Configuration


**Keyframe Interval**

* Not set by the plugin. In Simple output mode (forced by the wizard) OBS has no keyframe-interval config path — the streaming service applies its recommended keyframe interval automatically via `obs_service_info::apply_encoder_settings` (verified against OBS 31.1.1 source, 2026-09-29). Limitation: services that publish no recommendation — notably the custom **"Other"** service — leave the encoder at its default interval; the plugin does not override it in MVP.

**NVENC Preset**

* Default: p5 (p1–p7 scale, p1 = max performance; verified against OBS 31.1.1 source, 2026-09-29)
* If GPU usage > 85% → step down the p1–p7 scale toward p1
* Plugin writes the exact profile keys per encoder (Simple mode): NVENC → `SimpleOutput` / `NVENCPreset2`; x264 → `SimpleOutput` / `Preset`; QSV → `SimpleOutput` / `QSVPreset`; AMD H.264/HEVC → `SimpleOutput` / `AMDPreset`; AMD AV1 → `SimpleOutput` / `AMDAV1Preset`. (`preset`/`preset2` are NVENC *encoder property* names, not profile config keys — don't write those.)

**Migrate to Advanced Mode**

* A "Migrate to Advanced Mode" button in the plugin's settings UI, visible only while Output Mode is Simple.
* Copies the streaming settings the plugin manages from Simple to Advanced output mode: encoder selection, encoder preset, and video/audio bitrates (1:1 value copies — verified against OBS 31.1.1 source, 2026-09-29; see `docs/architecture.md` § /settings for the key mapping table).
* Untouched, because they're mode-independent: video settings (base/output resolution, FPS), audio settings (sample rate, channels), and the stream service + key.
* Recording settings are intentionally NOT migrated. When migration completes, a modal tells the user: "Recording settings cannot be migrated from Simple to Advanced mode — please set them yourself here," and points them to Settings → Output (recording section).

---

## 2. Audio Configuration

**Sample Rate**

* Set to 48 kHz (industry standard)

**Channels**

* Set to Stereo

**Microphone Filters**

Plugin can add and configure filters on microphone sources:

1. **Noise Suppression**

   * RNNoise (default) — benchmark its CPU cost during implementation; keep Speex only as a fallback if RNNoise proves too expensive on min-spec hardware
2. **Noise Gate**

   * Close threshold: -40 dB
   * Open threshold: -35 dB
3. **Compressor**

   * Ratio: 4:1
   * Threshold: -18 dB
   * Attack: 6 ms
   * Release: 60 ms
   * Output Gain: Adjust until peaks hit -6 dB
4. **Limiter**

   * Limit to -1 dB

**Optional VST Plugin Integration**

* Add ReaPlugs (ReaComp, ReaEQ) for finer control if installed

---

## 3. Scene Structure

**Minimum Scenes**

* Starting Soon
* BRB
* Just Chatting (full-screen webcam)
* Gameplay + Webcam
* Gameplay Only
* Stream Ended

*Note (2026-09-29): Scenes and overlays are distinct — several scenes can share one overlay (e.g. "Gameplay Only" and "Just Chatting" are both full-screen single-source scenes; only their sources differ). Overlay mapping: Starting Soon → Starting Soon frame; BRB → BRB frame; Stream Ended → Stream Ending frame; Gameplay + Webcam → webcam frame; the lower third is available on all live scenes.*

**Live Scene Structure**

* Game Capture (bottom layer)
* Webcam
* Alerts (Browser Source)
* Chat Overlay (optional)

**Nested Scenes**

* Create a "Camera + Alerts" scene and add into multiple parent scenes

**Source Management**

* Add sources, set order, scale, and position
* Plugin can automate all source creation, placement, and nesting

---

## 4. Transitions

* Set Fade → 300ms
* Set Cut → instant
* Add Stinger via Media Source if desired
* Plugin can set transitions and durations

---

## 5. Multistream + Alerts (Plugin-Controllable)

* Add OBS Multiple RTMP plugin outputs (streaming to multiple platforms)
* Configure Browser Sources for alerts:

  * Streamlabs
  * StreamElements
* Plugin can automate adding browser sources, sizing, and positioning

---

## 6. Stream Testing Automation

The 30-second local recording test runs **automatically as the final performance-validation step** (with a progress dialog — it is not presented as a skippable choice). It is also available afterwards via **Tools > Quickstart: Run stability check**, so users can re-run it after hardware changes or whenever something feels off.

Plugin test flow:

1. Start recording
2. Monitor stats:

   * Dropped frames
   * CPU usage
   * GPU rendering lag
3. Adjust dynamically on instability:

   * Step the encoder's quality control down its ladder (NVENC p-scale toward p1; QSV quality → speed; AMF Quality → Speed; x264 veryfast → ultrafast; VideoToolbox has no ladder — run once, then warn/offer)
   * Retry recording test (max 3 attempts)
4. If still unstable after 3 attempts: warn the user and offer options — re-run OBS's Auto-Configuration Wizard, or apply a conservative fallback (720p30 @ 2500 kbps, x264 ultrafast) with explicit user confirmation. The plugin never silently rewrites the wizard's resolution/FPS/bitrate.
5. Finalize optimized profile

*Note (2026-09-29): Retry logic no longer lowers FPS/resolution/bitrate directly — those are owned by the Auto-Configuration Wizard. The test's role is validation (catching thermal throttling, background load, and driver issues the wizard's short probes miss). Audio filters are applied **before** the test, so it measures the real configuration including RNNoise's CPU cost; no test result feeds back into filter selection.*

---

## 7. Recording Configuration

* Set Recording Quality → Indistinguishable
* Set Recording Format → MKV
* Set auto-remux if supported
* Plugin can write these settings to OBS profile

---

## 8. Advanced Plugin Features

* Enable Replay Buffer
* Enable Source Record Plugin (record individual sources separately)
* Enable Move Transition plugin for smooth animations
* Plugin can trigger these automatically and configure durations

---

## 9. Microphone Setup Check & Persistent Reminder

"Stream can't hear my mic" is the most common new-streamer support issue, and the Auto-Configuration Wizard does not touch audio at all — so this is fully plugin-owned.

**During setup:**
* Enumerate audio input devices.
* Mic exists but isn't selected in OBS → show device name with one-click **"Use this microphone"** (no Settings menus, no dropdowns).
* No input devices exist at all → prompt to plug in a mic/headset, with a **"Check again"** button that re-scans (don't send them into an empty dropdown).
* Show a live level meter ("talk to test") so the user visually confirms signal.

**On every OBS launch:**
* Silent mic check on the frontend finished-loading event.
* If no mic is configured → small dialog: **"Set up now"** / **"I don't use a mic"** / **"Remind me later"**. The choice is persisted in plugin config — never nag after a deliberate choice.

**Mic Troubleshooter (Tools > Quickstart: Mic troubleshooter):**
* Automated checks first, with one-click fixes where possible:
  * Configured Aux/mic device still present? (via device enumeration)
  * Muted in the OBS mixer, or volume slider down?
  * Wrong device selected (e.g., laptop mic vs. newly plugged-in headset)?
  * Track mismatch in Advanced output mode (mic on track 2, stream on track 1)?
* If everything passes but the level meter is still dead → guided OS-level checklist:
  * macOS microphone permission granted to OBS
  * Physical mute button on the headset
  * Windows exclusive-mode conflicts / device hijacked by another app
  * Push-to-talk / push-to-mute accidentally enabled, stray mute hotkey

---

## 10. Scene Hotkeys & Shortcut Reference Dock

* Plugin binds **Ctrl+Shift+1–6** to the created scenes (opt-in during setup, conflict-checked against existing bindings). Bare number keys are avoided because OBS hotkeys are system-global — they would switch scenes while typing in chat or gaming. Rebindable in Settings > Hotkeys; the plugin honors the configured bindings. Caveat (verified against OBS master, 2026-09-29): on Linux/Wayland, out-of-focus hotkeys are impossible by protocol — they fire only while OBS is focused.
* Adds a custom dock (**View > Docks > Quickstart Shortcuts**) listing the scene hotkeys plus the built-in OBS shortcuts beginners need most (start/stop streaming, start/stop recording, mute mic, mute desktop audio, studio mode toggle). Live bindings are read where the API allows; the rest are labeled as defaults so the dock never lies about a rebound key.
* The dock lives in OBS's window chrome via `obs_frontend_add_dock_by_id` — it is physically incapable of appearing in program output, so it can never leak onto the stream. Enabled by default at setup; user can hide it under View > Docks.

---

## 11. Bundled Default Overlays

* Ship a small set of default overlays with the plugin under `data/overlays/`: Starting Soon, BRB, and Stream Ending full-screen frames (1920x1080), a gameplay + webcam frame, and a subtle lower-third.
* The Stream Ending frame is for the actual wind-down (raid target, socials, schedule) — shown when the stream is genuinely ending, never as a mid-stream "ending soon" warning that would push new viewers to leave.
* All overlays must be original or CC0-licensed — no gray-area "free download" packs.
* SceneBuilder copies them into the plugin's config dir (`obs_module_config_path()`) at setup and references the copies — scene collections store absolute paths, so install-dir references would break on update/uninstall/export.
* Overlay/webcam positioning is relative to the real canvas size (`obs_get_video_info()` read after the wizard) — never hardcoded to 1920x1080, since the wizard sets base resolution from the user's monitor.
* Future work: an in-plugin tutorial guiding users through swapping in their own custom overlays.

---

## Consolidated List: Non-Programmatic Steps

These **cannot be fully automated via plugin**:

* Connect Twitch account via OAuth
* Connect YouTube account via OAuth
* Retrieve or manually paste platform stream keys (Twitch/YouTube/TikTok)
* Configure third-party chat docks (Restream Chat)
* Certain third-party plugin configurations without API exposure
* Platform-specific bitrate caps enforcement (Twitch 6 Mbps limit)
* Full service-specific multi-output configuration (Restream vs OBS Multiple RTMP plugin)
* OAuth-based alert account linking for Streamlabs / StreamElements

---
