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

1. [Ask user content type] → Gaming / IRL / Just Chatting
2. [Ask user platform] → Twitch / YouTube / Other
3. [Check stream key configured]

   * (Decision: Stream key configured?)

     * Yes → proceed
     * No → Show instructions to add manually in Settings > Stream
4. [Optional Network Speed Test] → Skippable

   * Use conservative multiplier (e.g., 70-80% of detected upload)
   * Result feeds into resolution/bitrate decision

---

## Phase 2: System Detection

1. [Detect available encoders] → NVENC / AMF / QSV / x264 / VideoToolbox
2. [Detect first webcam device]
3. [Check CPU cores & baseline CPU usage]
4. [Detect GPU VRAM & generation]

---

## Phase 3: Create New Profile / Scene Collection

1. [Create profile] → "AutoSetup_Beginner_[Platform]"
2. [Create scene collection] → "Beginner_Stream_Setup"
3. [Switch to new profile]

---

## Phase 4: Apply Settings

1. [Set Encoder] → Best available hardware encoder, fallback to x264
2. (Decision: Network speed available?)

   * Yes → Set resolution / FPS / bitrate based on speed test
   * No → Use conservative defaults
3. Resolution/FPS/Bitrate mapping:

   * <5 Mbps → 720p30 @ 3500 kbps
   * 5–8 Mbps → 720p60 @ 4500 kbps
   * > 8 Mbps → 1080p60 @ 6000 kbps
4. [Set Audio] → 48 kHz, Stereo, 160 kbps
5. [Set Keyframe Interval] → 2 sec

---

## Phase 5: Performance Test (Local Recording)

1. [Start 30-second local recording]
2. [Monitor metrics] → CPU %, GPU render lag, skipped frames
3. (Decision: Metrics unstable?)

   * Yes → [Reduce preset / FPS / resolution] → Retry recording test
   * Max retries: 3 attempts

     * If still unstable after 3 attempts:

       * Fall back to minimum config (720p30 @ 2500 kbps, x264 ultrafast)
       * Warn user their system may struggle with streaming
   * No → Proceed to next phase

---

## Phase 6: Create Scene Structure

1. [Create Scenes] → Starting Soon, Live, BRB, Ending Soon
2. Live Scene Placeholder Sources:

   * Game Capture → Fullscreen app, unconfigured
   * Webcam → First detected device
   * Text overlays → "Stream title" and "Configure game capture"

---

## Phase 7: Apply Audio Filters Conditionally

1. (Decision: CPU usage during recording test < 60%?)

   * Yes → [Add RNNoise + Compressor + Limiter]
   * No → [Add Speex + Noise Gate only]

---

## Phase 8: Summary & Next Steps

1. [Show settings summary to user]
2. [Show reminders] → Configure game capture, add stream key if missing
3. [Provide buttons] → "Start with these settings" / "I want to customize"

---

## Dynamic Monitoring Loop (Optional during setup)

* Continuously monitor CPU %, GPU render lag, dropped frames during recording
* Adjust FPS / Resolution / Encoder preset as needed
* Retry local recording until metrics stable (max 3 attempts; see Phase 5)

---

## Safety & Best Practices

* Never modify existing user profile or scene collection
* Always create new profile / scene collection
* Log changes locally, no telemetry by default
* User input required for critical selections (game capture, webcam, stream key)
* Safe universal defaults: 1080p60 @ 6000 kbps, keyframe 2 sec, 48kHz stereo audio
* Placeholder sources prevent broken streams while guiding users to configure

---

## Known Limitations

* Game capture requires manual game/window selection in OBS
* Multi-monitor setups may need additional manual configuration
* Users without stream keys must add them themselves
* Network speed test may be blocked by firewalls or restricted networks
