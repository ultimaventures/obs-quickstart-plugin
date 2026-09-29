# OBS Plugin Flowchart: Open-Source Community MVP

**Goal:** Automate OBS setup safely for a beginner/professional baseline stream, fully implementable using OBS plugin capabilities.

---

## Legend

* **[Action]** → plugin performs task
* **(Decision)** → plugin evaluates condition
* **→** → flow path
* **Thresholds** are annotated in brackets

---

## Phase 1: Quick Setup Dialog

<!-- 1. [Ask user content type] → Gaming / IRL / Just Chatting --> Not sure how this will change any of the settings, going to skip this question.
2. [Ask user platform] → Twitch / YouTube / Other
3. [Stream key] → owned by the Auto-Configuration Wizard (Phase 3), which collects it during setup. No pre-check: a fresh Quickstart profile has no key yet. (If the user chose "copy my current video settings", the key arrives with the copied profile.)


---

## Phase 2: System Detection


2. [Detect first webcam device]
3. [Check CPU cores & baseline CPU usage]

---

## Phase 3: Create New Profile & Run OBS's Auto-Configuration Wizard

1. [Create profile] → "Quickstart" (deduplicated if taken: "Quickstart 2", …)
2. [Create scene collection] → "Quickstart"
3. [Switch to new profile]
4. [Run OBS Auto-Configuration Wizard on the new profile]
   * Primary: trigger programmatically via `QMetaObject::invokeMethod` on `on_autoConfigure_triggered` (no public frontend API exists; this mirrors OBS's own first-run launch) — first confirm via `indexOfSlot`, honor the boolean return for fallback
   * Fallback: prompt the user to run **Tools > Auto-Configuration Wizard** manually
   * Skip option: "Copy my current video settings instead" — copies Video/Output keys from the previous profile for users who already ran the wizard
   * The wizard sets: service/server/stream key, Simple output mode, bitrate, encoder, recording encoder/quality, base/output resolution, FPS
   * The wizard dialog is modal → snapshot Video/Output keys before, compare after; if unchanged (cancelled), prompt before applying plugin settings; then the user clicks Continue in our wizard

---

## Phase 4: Apply Plugin-Owned Settings

*Note (2026-09-28): Encoder, resolution, FPS, and bitrate are set by the Auto-Configuration Wizard in Phase 3 — the plugin no longer computes or applies them.*

1. [Set Audio] → 48 kHz, Stereo, 160 kbps (the wizard never touches audio)
2. [Set NVENC preset] → p5 default (stepped down toward p1 by the stability test if GPU-bound)

---

## Phase 5: Microphone Setup & Check

1. [Enumerate audio input devices]
2. (Decision: Mic state?)
   * Device exists but not selected in OBS → show device name + one-click **"Use this microphone"**
   * No input devices at all → prompt to plug in a mic/headset + **"Check again"** re-scan
   * Already configured → proceed
3. [Show live level meter] → "Talk to test" visual confirmation
4. [Register startup reminder] → on every OBS launch, silent mic check; if unconfigured, dialog with **"Set up now"** / **"I don't use a mic"** / **"Remind me later"** (persisted in plugin config; never nag after a deliberate choice)

---

## Phase 6: Create Scene Structure

1. [Create Scenes] → Starting Soon, BRB, Just Chatting (full-screen webcam), Gameplay + Webcam, Gameplay Only, Stream Ended
   *Note: scenes and overlays are distinct — multiple scenes may share one overlay (overlay mapping in functionality.md).*
2. Gameplay w/camera Scene Placeholder Sources:
   * Game capture
      * Game Capture source (unconfigured - will show black screen)
      * Mode: Capture specific window
      * Window: [Not Set]
      * Text overlay on scene: "Right-click Game Capture → Properties to select your game"
   * Webcam
      * If webcam detected:
         * Add Video Capture Device source
         * Set to first detected device (auto-configured, works immediately)
      * If no webcam detected:
         * Skip adding webcam source entirely
         * Add text source: "No webcam detected - Right-click Camera Feed → Properties to select your camera"
3. Gameplay w/o camera Scene Placeholder Sources:
   * Game capture - larger than "Gameplay w/camera" scene, uses different overlay
      * Game Capture source (unconfigured - will show black screen)
      * Mode: Capture specific window
      * Window: [Not Set]
      * Text overlay on scene: "Right-click Game Capture → Properties to select your game"
4. Just Chatting Scene Placeholder Sources
   * Uses same overlay as "Gameplay w/o camera"
   * replaces Game capture placeholder with camera feed or placeholder
5. [Place bundled overlays] → from `data/overlays/` (shipped with the plugin): Starting Soon / BRB / Stream Ending full-screen frames (1920x1080), gameplay + webcam frame, subtle lower-third. The Stream Ending frame is for the actual wind-down (raid target, socials, schedule) — never a mid-stream "ending soon" warning. All assets original or CC0-licensed. If an overlay file is missing, log and continue — never fail setup over a cosmetic asset.
6. [Set source order/Z-order in scenes]:
      1. Game Capture (bottom layer)
      2. Webcam (top layer, positioned in corner)
      3. Overlays (frames, borders - top layer)
      4. Text overlays (instructions - topmost)

   [Set reasonable defaults]:
      - Webcam: 320x240, positioned bottom-right corner
      - Overlays: Full-screen 1920x1080
7. [Bind scene hotkeys] → Ctrl+Shift+1–6 → created scenes (opt-in; conflict-checked; bare number keys avoided — OBS hotkeys are system-global)
8. [Create shortcut reference dock] → **View > Docks > Quickstart Shortcuts**: scene→hotkey map + common OBS shortcuts (start/stop streaming/recording, mute mic/desktop, studio mode). Lives in OBS window chrome via `obs_frontend_add_dock_by_id` — can never appear in program output or on stream. Enabled by default; hideable under View > Docks.

---

## Phase 7: Performance Test (Local Recording) — auto-run

1. [Start 30-second local recording] — runs **automatically** as the final performance-validation step with a progress dialog ("Making sure your PC can handle streaming…"); not presented as a skippable choice
2. [Monitor metrics] → CPU %, GPU render lag, skipped frames
3. (Decision: Metrics unstable?)

   * Yes → [Switch NVENC preset Quality → Performance] → Retry recording test
   * Max retries: 3 attempts

     * If still unstable after 3 attempts:

       * Warn the user; offer to re-run the Auto-Configuration Wizard or apply a conservative fallback (720p30 @ 2500 kbps, x264 ultrafast) with explicit confirmation
       * Never silently rewrite the wizard's resolution/FPS/bitrate
   * No → Proceed to next phase
4. [CPU measurement feeds Phase 8 filter decision]
5. Afterwards available via **Tools > Quickstart: Run stability check** (re-run after hardware changes or whenever something feels off)

---

## Phase 8: Apply Audio Filters Conditionally

1. [Add RNNoise + Compressor + Limiter] (default) — benchmark RNNoise CPU cost during implementation; keep the Speex + Noise Gate fallback only if RNNoise proves expensive on min-spec hardware

---

## Phase 9: Summary & Next Steps

1. [Show settings summary to user]
2. [Show reminders] → Configure game capture, add stream key if missing, mic setup status, scene hotkeys (Ctrl+Shift+1–6)
3. [Provide buttons] → "Start with these settings" / "I want to customize"
4. [Point to Tools menu] → Quickstart: Run stability check, Mic troubleshooter, re-run setup wizard

---

## Dynamic Monitoring Loop (Optional during setup)

* Continuously monitor CPU %, GPU render lag, dropped frames during recording
* Adjust NVENC preset as needed (Quality → Performance)
* Retry local recording until metrics stable (max 3 attempts; see Phase 7)
* FPS / resolution / bitrate are owned by the Auto-Configuration Wizard and are not adjusted here

---

## Safety & Best Practices

* Never modify existing user profile or scene collection
* Always create new profile / scene collection
* Log changes locally, no telemetry by default
* User input required for critical selections (game capture, webcam, stream key)
* Safe universal defaults: 48kHz stereo audio, MKV + auto-remux (keyframe interval is injected by the streaming service in Simple mode; video/encoder/bitrate settings owned by the Auto-Configuration Wizard)
* Placeholder sources prevent broken streams while guiding users to configure

---

## Known Limitations

* Game capture requires manual game/window selection in OBS
* Multi-monitor setups may need additional manual configuration
* Users without stream keys must add them themselves
