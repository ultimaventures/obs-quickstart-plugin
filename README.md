# obs-quickstart-plugin

## Name
OBS Quickstart Setup for Streamers

## Description
IN DEVELOPMENT - not currently ready for use. We are building an OBS plugin that will programmatically handle 70%-80% of the necessary/best-practice setup of OBS for 80%-90% of users. There are certain setup actions that simply can't be accomplished by a plugin and there are certain edge cases (multiple GPUs, exotic Linux setups, users with 3+ webcams) that will not be covered, but this will remove a huge amount of the friction with being a new streamer/OBS user for the vast majority of users.

Here's what you'll most likely still want to do even after running this, as these cannot be fully automated via plugin:
* Connect Twitch account via OAuth
* Connect YouTube account via OAuth
* Retrieve or manually paste platform stream keys (Twitch/YouTube/TikTok)
* Configure third-party chat docks (Restream Chat)
* Certain third-party plugin configurations - we recommend:
    * Enable Replay Buffer
    * Enable Source Record Plugin (record individual sources separately)
    * Enable Move Transition plugin for smooth animations
* Full service-specific multi-output configuration (we recommend Restream over OBS Multiple RTMP plugin for beginners)
* OAuth-based alert account linking for Streamlabs / StreamElements

## Badges
On some READMEs, you may see small images that convey metadata, such as whether or not all the tests are passing for the project. You can use Shields to add some to your README. Many services also have instructions for adding a badge.

## Visuals
Depending on what you are making, it can be a good idea to include screenshots or even a video (you'll frequently see GIFs rather than actual videos). Tools like ttygif can help, but check out Asciinema for a more sophisticated method.

## Installation
Within a particular ecosystem, there may be a common way of installing things, such as using Yarn, NuGet, or Homebrew. However, consider the possibility that whoever is reading your README is a novice and would like more guidance. Listing specific steps helps remove ambiguity and gets people to using your project as quickly as possible. If it only runs in a specific context like a particular programming language version or operating system or has dependencies that have to be installed manually, also add a Requirements subsection.

## Usage
Use examples liberally, and show the expected output if you can. It's helpful to have inline the smallest example of usage that you can demonstrate, while providing links to more sophisticated examples if they are too long to reasonably include in the README.

## Support
Tell people where they can go to for help. It can be any combination of an issue tracker, a chat room, an email address, etc.

## Roadmap
### 1) **Set Up Project Skeleton**
In addition to directory structures from [OBS Plugin Template](https://github.com/obsproject/obs-plugintemplate)
```
/obs-quickstart-plugin
  /src
    /detection (empty for now)
    /audio
    /hotkeys
    /profile
    /settings
    /sources
    /filters
    /monitoring
    /ui
  /tests
    /unit
    /integration
  CMakeLists.txt
  README.md
  architecture.md ✅
  CONVENTIONS.md (TODO)
  TESTING.md (TODO)
```
### 2) Proof of Concept Task
Before Sprint 1 starts, someone must:

* Clone obs-plugintemplate
* Build a basic C++ plugin that loads in OBS
* Add menu item "Tools > OBS Setup Test"
* Show a Qt dialog saying "Hello World"
* Log "Plugin loaded" to OBS log
* Detect one encoder and log it

Success criteria: Plugin compiles, loads without crashing OBS, shows dialog.

### 3. Consolidated Subtasks/Sprints

#### Pre-sprint setup
- Update CMakeLists.txt:
  - Change project name to obs-quickstart-plugin
  - Add subdirectories
  - Set version, author, etc.
- GitHub Actions → GitLab CI/CD Translation
  - obs-plugintemplate includes:
    ```
    .github/workflows/build.yml
    .github/workflows/release.yml
    ```
  - We'll create:
    ```
    .gitlab-ci.yml
    ```
- Test local build:
  ```
  bash   cmake -B build
  cmake --build build
  ```
  Should compile and load in OBS (even if it does nothing yet)

#### Sprint 1: Foundation / System Detection (Week 3-4)
**Module: SystemDetector**
- Detect first available webcam (device ID)
- Collect CPU & GPU info
- Tests:
  - Mock configurations with/without webcam
  - Verify CPU count reporting


**Module: ProfileManager**
- Create new profile ("Quickstart", deduplicated if taken) without modifying existing profiles
- Create new scene collection ("Quickstart")
- Switch to new profile
- Trigger OBS's Auto-Configuration Wizard on the new profile (programmatic trigger via `QMetaObject::invokeMethod`, manual fallback to Tools > Auto-Configuration Wizard, "copy my current video settings" skip option)
- Test: Profile and collection created successfully, original untouched

---

#### When to add CI/CD:
- After Sprint 1 completes (basic modules working locally)
- Before merging to main branch
- When we need automated testing

**Where to document it:**
- Create CI-CD.md or DEPLOYMENT.md separate from architecture
- Don't clutter architecture docs with build pipeline details

#### Sprint 2: Core Logic / Settings Engine (Week 5-6)
**Module: SettingsCalculator** (plugin-owned settings only)
- Apply NVENC preset → p5 default (p1–p7 scale; stepped down toward p1 by stability test if GPU-bound)
- Apply audio → 48 kHz, Stereo
- Tests:
  - Preset and audio settings applied correctly

**Module: PerformanceMonitor**
- Record local test for 30 seconds — **auto-runs as the final performance-validation step** (progress dialog, not skippable); also available via Tools > Quickstart: Run stability check
- Monitor CPU usage, GPU load, dropped frames, rendered frames
- Determine system stability
- Retry logic: max 3 attempts, stepping the encoder's quality ladder only (NVENC p-scale toward p1; QSV quality → speed; AMF Quality → Speed; x264 veryfast → ultrafast; VideoToolbox has no ladder — run once, then warn/offer)
- Fallback: if still unstable, warn user and offer to re-run the Auto-Configuration Wizard or apply conservative fallback (720p30 @ 2500 kbps, x264 ultrafast) with explicit confirmation — never silently rewrite the wizard's settings
- Tests:
  - Returns correct metrics
  - Determines stability according to thresholds
  - Retry adjusts preset only; resolution/FPS/bitrate untouched

---

#### Sprint 3: Scene Creation (Week 11-12)
**Module: SceneBuilder**
- Create scene structure: Starting Soon, BRB, Just Chatting (full-screen webcam), Gameplay + Webcam, Gameplay Only, Stream Ended
- Add placeholder sources:
  - Game Capture (mode: fullscreen app; may require user config)
  - Webcam (first detected device)
  - Placeholder text overlays (e.g., "Configure game capture", "Stream title")
- Place bundled default overlays from `data/overlays/` (Starting Soon / BRB / Stream Ending frames, webcam frame, lower-third; original or CC0-licensed; positioned relative to the real canvas size, never assumed 1920x1080)
- Tests:
  - Scene structure created
  - Placeholder sources added correctly
  - Overlay sources reference existing bundled files

**Module: HotkeyManager** (part of Sprint 3)
- Bind Ctrl+Shift+1–6 to created scenes (1 = Starting Soon, 2 = BRB, 3 = Just Chatting, 4 = Gameplay + Webcam, 5 = Gameplay Only, 6 = Stream Ended) — opt-in during setup, conflict-checked against existing bindings. Bare number keys are avoided: OBS hotkeys are system-global and would switch the live scene while typing in chat or gaming. Rebindable in Settings > Hotkeys
- Tests: hotkeys bound to correct scenes

---

#### Sprint 4: Microphone Setup & Troubleshooting (Week 13-14)
*Note (2026-09-29): moved ahead of the UI wizard — the setup wizard's mic-check step depends on this module.*
**Module: AudioManager**
- Setup-time mic check: enumerate input devices; one-click "Use this microphone" if unselected; "plug in + Check again" re-scan if none present; live level meter ("talk to test")
- Persistent reminder: silent mic check on every OBS launch; dialog with "Set up now" / "I don't use a mic" / "Remind me later" (persisted in plugin config, never nag after a deliberate choice)
- Mic troubleshooter (Tools > Quickstart: Mic troubleshooter): automated checks (device present? muted? wrong device? track mismatch in Advanced output?) with one-click fixes, then guided OS-level checklist (macOS permission, physical mute button, exclusive mode, push-to-talk)
- Tests:
  - Device-present-but-unselected → offers one-click assign
  - No devices → prompts re-scan
  - Reminder choice persists across launches


## Contributing
State if you are open to contributions and what your requirements are for accepting them.

For people who want to make changes to your project, it's helpful to have some documentation on how to get started. Perhaps there is a script that they should run or some environment variables that they need to set. Make these steps explicit. These instructions could also be useful to your future self.

You can also document commands to lint the code or run tests. These steps help to ensure high code quality and reduce the likelihood that the changes inadvertently break something. Having instructions for running tests is especially helpful if it requires external setup, such as starting a Selenium server for testing in a browser.

## Authors and acknowledgment
Show your appreciation to those who have contributed to the project.

## License
For open source projects, say how it is licensed.

## Project status
If you have run out of energy or time for your project, put a note at the top of the README saying that development has slowed down or stopped completely. Someone may choose to fork your project or volunteer to step in as a maintainer or owner, allowing your project to keep going. You can also make an explicit request for maintainers.


---

#### Sprint 5: Audio Filters (Week 15-16)
**Module: AudioFilterManager**
- Apply RNNoise + Compressor + Limiter by default; benchmark RNNoise CPU cost during implementation and keep the Speex + Noise Gate fallback only if it is expensive on min-spec hardware
- Methods:
  - `addRNNoise()`, `addSpeex()`, `addCompressor()`, `addLimiter()`
- Tests:
  - Default filter chain (RNNoise + Compressor + Limiter) applied

---

#### Sprint 6: UI / Setup Wizard (Week 17-18)
**Module: SetupWizard (Qt)**
- Wizard steps:
  - Create "Quickstart" profile & scene collection, run OBS Auto-Configuration Wizard on it (the wizard collects the streaming service and key — no separate platform question)
  - Apply plugin-owned settings (NVENC preset, 48 kHz stereo)
  - Microphone check (one-click assign, live level meter)
  - Scene/source creation with bundled overlays
  - Audio filters (RNNoise + Compressor + Limiter — applied before the stability test so it measures their cost)
  - Stability test (auto-run with progress dialog)
- Show summary of applied settings
- Display reminders / next steps:
  - Configure game capture
  - Add stream key if not present
  - Mic setup status, scene hotkeys (Ctrl+Shift+1–6)
- Create shortcut reference dock (View > Docks > Quickstart Shortcuts)
- Register Tools menu items: re-run setup wizard, Run stability check, Mic troubleshooter
- Tests:
  - Correct user input captured
  - Summary displayed correctly
  - Wizard step order (profile → OBS wizard → plugin settings → mic → scenes → filters → stability)

---

#### Sprint 7: Integration & Final Validation
- Integrate all modules:
  - System detection → Profile creation → OBS wizard trigger → Plugin settings → Mic check → Scene & sources (+ overlays, hotkeys) → Audio filters → Stability test → UI wizard
- Run end-to-end test with:
  - Multiple hardware configurations
  - Different CPU/GPU loads
  - Mic present / unselected / absent
- Validate fallback behavior
- Validate max retries and minimum config fallback
