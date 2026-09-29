# OBS Setup Plugin Architecture

This document outlines the architecture of the OBS Setup Plugin, designed to automate 70–80% of the setup process for 80–90% of users. The plugin is modular, testable, and extensible to accommodate different system configurations.

## Directory Structure

```
/src
  /detection      # System detection (CPU, platform, webcam)
  /audio          # Microphone setup check, reminder, troubleshooter
  /hotkeys        # Scene hotkey bindings + shortcut reference dock
  /profile        # New profile & scene collection creation
  /settings       # Plugin-owned settings (preset, audio)
  /sources        # Scene & source creation (+ bundled overlays)
  /filters        # Conditional audio filter application
  /monitoring     # Local recording test & metrics
  /ui             # Setup dialog and user interaction
  plugin-main.cpp
  plugin-support.cpp
  plugin-support.h
  /data
    /locale
    /overlays     # Bundled default overlays (1920x1080, original/CC0)
  /cmake
  CMakeLists.txt
```


---

## Module Breakdown

### 1. /detection – System Detection

**Purpose:** Detect system capabilities and available devices (CPU, platform, webcam)

**What We Can Detect:**
- ✅ CPU core count (via `std::thread::hardware_concurrency()`)
- ✅ Which platform (Windows/macOS/Linux)
- ✅ First available webcam (via OBS device enumeration)

*Note (2026-09-28): Encoder detection removed from scope. Encoder selection is handled by OBS's Auto-Configuration Wizard, which probes encoders against real hardware. This module no longer recommends or selects encoders.*

**Detection Strategy:**

**Simple struct:**
```cpp
struct HardwareInfo {
    int cpuCores;              // Actual count from system
    std::string firstWebcam;   // First available webcam device id (empty if none)
    std::string platform;      // "windows" | "macos" | "linux"
};
```


**Error Handling:** Log errors, graceful degradation (missing webcam is not fatal — scene gets a placeholder text source)

**Unit Tests:** Simulate configurations with/without webcam, verify CPU count reporting


**API Surface:**

```cpp
class SystemDetector {
public:
    HardwareInfo detect();
    std::string getFirstWebcam();
};
```

---


---

### 3. /profile – New Profile & Scene Collection Creation

**Purpose:** Create a NEW OBS profile and scene collection (never modify existing), then hand video/encoder tuning to OBS's Auto-Configuration Wizard.

**Inputs:** User platform preference

**Outputs:**

* NEW profile: `Quickstart` (deduplicated to `Quickstart 2`, `Quickstart 3`, … if the name is taken — `obs_frontend_create_profile` requires unique names)
* NEW scene collection: `Quickstart`

**Critical:** Backup existing configuration before creating new profile

**Error Handling:** Rollback profile creation on failure

**Unit Tests:** Validate JSON creation, backup & rollback logic

**API Surface:**

```cpp
class ProfileManager {
public:
    bool createNewProfile(const std::string& name);
    bool backupExistingProfile();
    std::string deduplicatedProfileName(const std::string& base);
};
```

**Auto-Configuration Wizard trigger (NEW 2026-09-28):**

After switching to the new profile, the setup wizard offers a **"Run OBS's automatic tuner"** button instead of computing video/encoder settings itself:

1. **Primary path:** confirm the slot exists via `mainWindow->metaObject()->indexOfSlot("on_autoConfigure_triggered()")`, then invoke the wizard programmatically —
   `QMetaObject::invokeMethod(obs_frontend_get_main_window(), "on_autoConfigure_triggered", Qt::QueuedConnection)`.
   There is no public frontend API for this; it mirrors exactly how OBS launches the wizard on first run. The slot is verified against the OBS 31.1.1 source tree (`frontend/widgets/OBSBasic.hpp`) — the SDK does not ship OBSBasic headers, so re-verify the slot name against source on each OBS major-version bump. Honor the `invokeMethod` boolean return: on false, use the fallback.
2. **Fallback:** if the slot isn't found (future OBS versions), show a prompt guiding the user to **Tools > Auto-Configuration Wizard** manually.
3. **Skip option:** "Copy my current OBS video settings instead" — copies the Video/Output keys from the previous profile's `basic.ini` via the config API, for users who already ran the wizard and don't want to sit through the bandwidth test again.
4. Completion detection: snapshot the profile's Video/Output config keys before launching; when the modal dialog closes, compare — if unchanged (the user cancelled), ask whether to continue applying plugin-owned settings to the unconfigured profile or re-run the wizard. Then the user clicks **Continue** and setup proceeds.

*Why not rebuild it:* the wizard runs real per-server bandwidth tests with scoring, top-down encoding probes, and CPU-tier caps — battle-tested over years. The plugin's value is everything around it (scenes, sources, audio, overlays, validation), not re-deriving bitrate tables.

**Possible solution for Video Capture Device with Placeholder**

```// Add Video Capture Device source (will show error/black)
obs_source_t* cameraSource = obs_source_create(
    "dshow_input",  // Windows
    "Camera Feed",
    nullptr,
    nullptr
);
obs_scene_add(scene, cameraSource);

// Add text overlay explaining how to configure
obs_source_t* textSource = obs_source_create(
    "text_gdiplus",
    "Camera Setup Instructions",
    textSettings,
    nullptr
);
// Text: "No camera detected\nRight-click 'Camera Feed' → Properties to select device"
```

---

### 4. /settings – Plugin-Owned Settings

**Purpose:** Apply the settings OBS's Auto-Configuration Wizard does *not* set.

**This module applies only what the wizard leaves untouched:**

**Applies:**
- NVENC preset → p5 default (stepped down the p1–p7 scale toward p1 by /monitoring if GPU-bound; verified against OBS master 2026-09-29)
- Audio → 48 kHz, Stereo
- Recording → MKV format, auto-remux to MP4 where supported, Recording Quality → Indistinguishable

**Keyframe interval is intentionally NOT set by the plugin.** Verified against the OBS master source (2026-09-29): in Simple output mode — which the wizard forces — there is no keyframe-interval config path at all (`SimpleOutput` never reads one). The streaming service injects its recommended `keyint` (`rtmp-common.c`: `apply_video_encoder_settings`) before the encoder is updated, so Twitch's recommended 2 s applies automatically. A plugin-set keyframe would require Advanced output mode — deferred, not MVP.

**Error Handling:** Validate OBS constraints, fallback to safe presets

**Unit Tests:** Validate preset values, audio settings applied correctly

**API Surface:**

```cpp
class SettingsManager {
public:
    void applyEncoderPreset(const std::string& preset);
    void applyAudioSettings(); // 48 kHz, stereo
    void applyRecordingSettings(); // MKV + auto-remux + recording quality
};
```

---

### 5. /sources – Scene & Source Creation

**Purpose:** Generate scenes and add sources.

**Inputs:** Scene collection, available sources, user preferences

**Outputs:** Configured scenes with sources attached

**Bundled overlays (NEW 2026-09-28):** SceneBuilder automatically places the default overlays shipped under `data/overlays/` — Starting Soon / BRB / Stream Ending full-screen frames (1920x1080), a gameplay + webcam frame, and a subtle lower-third. The Stream Ending frame is for the actual wind-down (raid target, socials, schedule) — shown when the stream is genuinely ending, never as a mid-stream "ending soon" warning. All overlay assets must be original or CC0-licensed. At setup, SceneBuilder reads the actual canvas size via `obs_get_video_info()` — the wizard sets base resolution from the user's monitor (1440p, ultrawide, 16:10…), so 1920x1080 must never be assumed — and positions/scales overlays and the webcam PiP relative to it via bounds/scale. SceneBuilder copies the overlays into the plugin's config dir (`obs_module_config_path()`) and references the copies — scene collections store absolute paths, so referencing the install directory would break on plugin update/uninstall or when a collection is exported to another machine.

**Error Handling:** Log unsupported sources, skip invalid sources; if an overlay file is missing, log and continue without it (never fail setup over a cosmetic asset)

**Unit Tests:** Validate scene/source hierarchy and placement; verify overlay sources reference existing files

**API Surface:**

```cpp
class SourceManager {
public:
    bool addSourceToScene(const std::string& scene, const std::string& source);
    bool addBundledOverlays(const std::string& scene);
};
```

---

### 6. /filters – Conditional Audio Filter Application

**Purpose:** Apply audio filters based on CPU headroom

**Inputs:** Audio sources, CPU usage from monitoring test

**Outputs:** Applied filters

**Logic:**

* Default: RNNoise + Compressor + Limiter. Benchmark RNNoise's CPU cost on target hardware during implementation — if it exceeds a few percent of one core on min-spec machines, keep the Speex + Noise Gate fallback for the high-CPU branch. (The old <60% gate was measured on a near-idle desktop and is not a reliable signal.)

**Error Handling:** Skip individual filters if they fail, log errors

**Unit Tests:** Validate conditional filter logic

**API Surface:**

```cpp
class FilterManager {
public:
    void applyFilters(double cpuUsage);
};
```

---

### 7. /monitoring – Local Recording Test & Metrics

**Behavior (updated 2026-09-28):** The 30-second recording test runs **automatically as the final performance-validation step** with a progress dialog (audio filters and the setup summary follow it) ("Making sure your PC can handle streaming…") — it is not offered as a skippable choice. It is also exposed as **Tools > Quickstart: Run stability check** for re-runs after hardware changes.

**Role:** validation, not discovery. The wizard's short probes pick the settings; this test catches what they miss — thermal throttling, background load, driver issues — under a sustained 30-second load. Its CPU measurement also feeds /filters (RNNoise vs. Speex decision).

**Retry policy:** on instability, retry up to 3 times adjusting only the encoder's quality control — FPS/resolution/bitrate belong to the wizard and are never rewritten silently. Quality ladder per encoder: **NVENC** preset stepped down the p1–p7 scale toward p1 (p1 = max performance); **QSV** targetusage quality → speed; **AMF** quality preset Quality → Speed; **x264** preset veryfast → ultrafast. **VideoToolbox** exposes no quality ladder — run the test once and go straight to the warn/offer step on instability instead of repeating an identical 30-second test. If still unstable after the attempts: warn the user and offer to re-run the Auto-Configuration Wizard or apply a conservative fallback (720p30 @ 2500 kbps, x264 ultrafast) with explicit confirmation.

**Safety:** the test dialog has Cancel and a 45-second timeout; the test recording is deleted afterwards; the test refuses to start while streaming or recording is active.

**CRITICAL: Asynchronous Execution Required**

The 30-second recording test MUST NOT block the main thread:

**Implementation Pattern:**
1. Start recording on main thread (OBS API requirement)
2. Start Qt timer on main thread to poll metrics every 1 second
3. Collect metrics in background (CPU/GPU stats don't need OBS API)
4. After 30 seconds, stop recording on main thread
5. Report results via Qt signals to UI

**Example:**
```cpp
class Monitor : public QObject {
    Q_OBJECT
public:
    void startRecordingTest() {
        // Main thread - start recording
        obs_frontend_recording_start();
        
        // Start timer for polling (non-blocking)
        m_pollTimer.start(1000); // Poll every 1 second
    }

signals:
    void metricsUpdated(const Metrics& m);
    void testComplete(bool success);

private slots:
    void pollMetrics() {
        // Check CPU/GPU stats (can be done off main thread)
        Metrics m = collectMetrics();
        emit metricsUpdated(m);
        
        if (++m_secondsElapsed >= 30) {
            m_pollTimer.stop();
            obs_frontend_recording_stop(); // Main thread
            emit testComplete(true);
        }
    }
};
```

**UI Integration:**
```cpp
// Wizard shows progress bar during test
connect(monitor, &Monitor::metricsUpdated, [](const Metrics& m) {
    progressBar->setValue(m.secondsElapsed);
    cpuLabel->setText(QString("CPU: %1%").arg(m.cpuUsage));
});
```

---

### 8. /ui – Setup Dialog & User Interaction

**Purpose:** Guided wizard for setup and confirmation

**Inputs:** Module outputs, user preferences

**Outputs:** Final configuration confirmation

**Wizard steps (updated 2026-09-28):**
1. Platform selection (Twitch / YouTube / Other) + stream key check (manual instructions if missing)
2. Create "Quickstart" profile & scene collection, switch to it
3. **Run OBS's Auto-Configuration Wizard** on the new profile (programmatic trigger with manual fallback; "copy my current video settings" skip option) — see Module 3
4. Apply plugin-owned settings (NVENC preset, 48 kHz stereo)
5. **Microphone check** — one-click device assignment, live level meter — see Module 9
6. Scene/source creation with bundled overlays
7. **Stability test** — auto-runs with progress dialog — see Module 7
8. Conditional audio filters based on measured CPU
9. Summary & next steps (configure game capture, add stream key if missing)

**Tools menu items (registered via `obs_frontend_add_tools_menu_item`):**
- `Quickstart: Run setup wizard` — re-runs the full wizard
- `Quickstart: Run stability check` — standalone 30-second test (see Module 7)
- `Quickstart: Mic troubleshooter` — automated mic diagnostics (see Module 9)

**Error Handling:** Validate input, fallback if UI fails

**Unit Tests:** Test wizard flow, error dialogs

**API Surface:**

```cpp
class SetupWizard : public QDialog {
    Q_OBJECT
public:
    void startWizard();
signals:
    void setupCompleted(bool success);
};
```

---

### 9. /audio – Microphone Setup Check, Reminder & Troubleshooter

**Purpose:** Attack the #1 new-streamer support issue ("stream can't hear my mic") from both ends — prevention during setup, cure afterwards. The Auto-Configuration Wizard never touches audio, so this is fully plugin-owned.

**Setup-time mic check:**
- Enumerate audio input devices.
- Mic exists but not selected in OBS → show the device name with one-click **"Use this microphone"**.
- No input devices at all → prompt to plug one in, with **"Check again"** re-scan (never send the user into an empty dropdown).
- Live level meter ("talk to test") for visual confirmation of signal.

**Persistent reminder:**
- On every OBS launch, run a silent mic check — deferred (never a modal inside the finished-loading callback; use a queued invocation), and only for users who completed Quickstart setup (persisted flag). The silent check must not open the device in a way that triggers the OS microphone-permission prompt (notably macOS).
- If no mic configured → dialog with **"Set up now"** / **"I don't use a mic"** / **"Remind me later"**, persisted in plugin config (`$OBS_CONFIG/plugin_config/obs-quickstart-plugin/config.json`). Never nag after a deliberate choice.

**Mic Troubleshooter (Tools > Quickstart: Mic troubleshooter):**
- Automated checks first, one-click fixes where possible:
  - Configured Aux/mic device still present? (device enumeration vs. config)
  - Muted in OBS mixer, or volume slider at minimum?
  - Wrong device selected?
  - Track mismatch in Advanced output mode (mic on track 2, stream on track 1)?
- If all checks pass but the meter is dead → guided OS-level checklist:
  - macOS microphone permission for OBS
  - Physical mute button on headset
  - Windows exclusive-mode / device hijacked by another app
  - Push-to-talk / push-to-mute accidentally enabled, stray mute hotkey

**Error Handling:** Never block setup over audio — a missing mic degrades to the persistent reminder, not a setup failure.

**Unit Tests:** Device-present-but-unselected → offers one-click assign; no-devices → prompts re-scan; reminder-choice persistence round-trips.

**API Surface:**

```cpp
class AudioManager {
public:
    MicStatus checkMicrophone();          // configured / unselected / none-present
    bool assignMicrophone(const std::string& deviceId);
    void runTroubleshooter();             // automated checks + guided checklist
    void checkOnStartup();                // silent check + conditional reminder dialog
};
```

---

### 10. /hotkeys – Scene Hotkeys & Shortcut Reference Dock

**Purpose:** Make scene switching and common OBS actions discoverable for beginners who can't be expected to memorize shortcuts.

**Scene hotkeys:** plugin binds **Ctrl+Shift+1–6** to the created scenes (1 = Starting Soon, 2 = BRB, 3 = Just Chatting, 4 = Gameplay + Webcam, 5 = Gameplay Only, 6 = Stream Ended). Bound at scene-creation time via the OBS hotkey API, opt-in during setup, conflict-checked against existing bindings. Bare number keys are deliberately avoided — OBS hotkeys are system-global and would switch the live scene while the user types in chat or games. Rebindable in Settings > Hotkeys; the plugin honors the configured bindings at runtime, and the shortcut dock always displays live bindings.

**Shortcut reference dock:** a custom `QDockWidget` added via `obs_frontend_add_dock_by_id`, listed under **View > Docks > Quickstart Shortcuts**, showing:
- The scene → hotkey mapping (read from the bindings we created — always accurate)
- Common built-in OBS shortcuts beginners need: start/stop streaming, start/stop recording, mute mic, mute desktop audio, studio mode toggle (live bindings read where the API allows, otherwise labeled as defaults)

The dock lives in OBS's window chrome — it is physically incapable of appearing in program output, so it can never leak onto the stream. Enabled by default at setup; the user can hide it under View > Docks.

**Unit Tests:** Hotkeys bound to the correct scenes; dock lists exactly the bound mappings.

**API Surface:**

```cpp
class HotkeyManager {
public:
    void bindSceneHotkeys();   // 1–6 → created scenes
    void createShortcutDock();  // QDockWidget via obs_frontend_add_dock_by_id
};
```

---

## C++ Implementation Requirements

### Memory Management

* Use RAII for OBS objects
* Release via obs_*_release() or auto-release wrappers
* No raw pointers without clear ownership

### Thread Safety

* All OBS API calls on main thread
* Qt::QueuedConnection for cross-thread calls
* Worker threads only for network/performance tests

### Qt Integration

* UI uses Qt5/Qt6 widgets
* QDialog with QWizard pattern
* Signals/slots for UI updates

### Error Handling

* Check all OBS API return values
* Log via blog()
* Graceful degradation allowed

---

## Execution Order & Dependencies

1. /detection — CPU, platform, webcam (no dependencies)
2. /profile — create "Quickstart" profile & scene collection, switch to it
3. OBS Auto-Configuration Wizard — triggered on the new profile (or copy-settings skip); owned by OBS, not a plugin module
4. /settings — encoder preset, audio (applies on top of wizard output)
5. /audio — microphone check & assignment
6. /sources — scenes, sources, bundled overlays (depends on profile & detection)
7. /hotkeys — bind scene hotkeys 1–6, create shortcut dock (after sources)
8. /monitoring — auto-run 30-second recording test (after profile, settings, sources)
9. /filters — conditional audio filters (depends on sources & monitoring CPU measurement)
10. /ui — orchestrates all modules

**Critical:** /profile before the wizard trigger; wizard before /settings; /monitoring after initial setup; /filters after /monitoring

---

## Error Recovery

1. Do not leave partial config
2. Delete created profile if not finalized
3. Restore previous active profile
4. Log failure reason
5. Show user-friendly error message

**Triggers:** Encoder init fails, recording test fails 3+ times, user cancels, critical exceptions

---

## Plugin Configuration

**Storage:** $OBS_CONFIG/plugin_config/obs-quickstart-plugin/config.json
**Format:** JSON

**Stores:** Last used settings, user preferences, plugin version
**Never Stores:** Stream keys, OAuth tokens, PII

---

## Error Handling Pattern

1. Log error with context
2. Attempt graceful degradation
3. If critical, abort and rollback
4. Report user-actionable message

## Project Foundation

This plugin is built using the [OBS Plugin Template](https://github.com/obsproject/obs-plugintemplate) as a foundation, with the following customizations:

- Extended directory structure for modular architecture
- GitLab CI/CD instead of GitHub Actions
- Custom module organization (detection, network, profile, etc.)
- Qt wizard UI integration

The template provides:
- CMake build system
- Cross-platform compilation support
- Plugin entry point boilerplate
- Release packaging configuration