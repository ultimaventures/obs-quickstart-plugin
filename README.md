# obs-setup

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
/obs-setup
  /src
    /detection (empty for now)
    /network
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
  - Change project name to obs-setup
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
- Detect available encoders (NVENC, AMF, QSV, x264)
- Determine encoder priority and handle missing encoders
- Detect first available webcam (device ID)
- Collect CPU & GPU info
- Tests:
  - Mock different hardware configurations
  - Verify encoder priority: NVENC > AMF > QSV > x264
  - Handle missing encoders gracefully

**Module: SpeedTestWrapper**
- Optional speed test for upload bandwidth
- Handles network errors gracefully
- Returns estimated speed in Mbps
- Test: Returns plausible speed, fallback if network blocked

**Module: ProfileManager**
- Create new profile (e.g., "AutoSetup_Beginner_[Platform]") without modifying existing profiles
- Create new scene collection (e.g., "Beginner_Stream_Setup")
- Switch to new profile
- Apply calculated settings to profile
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
**Module: SettingsCalculator**
- Calculate optimal resolution, FPS, bitrate based on:
  - Upload speed
  - Encoder type
  - CPU cores
- Tests:
  - Given 7000 kbps → outputs 720p60 @ 4500 kbps
  - No hardware encoder → uses x264 ultrafast
  - CPU-limited systems → lower settings

**Module: PerformanceMonitor**
- Record local test for 30 seconds
- Monitor CPU usage, GPU load, dropped frames, rendered frames
- Determine system stability
- Retry logic: max 3 attempts
- Fallback: minimum config if unstable after retries
  - 720p30 @ 2500 kbps, x264 ultrafast
- Tests:
  - Returns correct metrics
  - Determines stability according to thresholds

---

#### Sprint 3: Scene Creation (Week 11-12)
**Module: SceneBuilder**
- Create scene structure: Starting Soon, Live, BRB, Ending
- Add placeholder sources:
  - Game Capture (mode: fullscreen app; may require user config)
  - Webcam (first detected device)
  - Placeholder text overlays (e.g., "Configure game capture", "Stream title")
- Tests:
  - Scene structure created
  - Placeholder sources added correctly

---

#### Sprint 4: Audio Filters (Week 13-14)
**Module: AudioFilterManager**
- Apply conditional audio filters based on CPU headroom:
  - CPU usage during recording test < 60% → RNNoise + Compressor + Limiter
  - CPU usage ≥ 60% → Speex + Noise Gate only
- Methods:
  - `addRNNoise()`, `addSpeex()`, `addCompressor()`, `addLimiter()`
- Tests:
  - Filters applied according to CPU thresholds

---

#### Sprint 5: UI / Setup Wizard (Week 15-16)
**Module: SetupWizard (Qt)**
- Collect user inputs:
  - Content type (Gaming/IRL/Just Chatting)
  - Platform(s) (Twitch/YouTube/Both/Other)
  - Stream key presence
  - Upload speed (optional, can use speed test)
- Show summary of calculated settings
- Display reminders / next steps:
  - Configure game capture
  - Add stream key if not present
- Tests:
  - Correct user input captured
  - Summary displayed correctly

---

#### Sprint 6: Integration & Final Validation
- Integrate all modules:
  - System detection → Settings calculation → Profile creation → Scene & sources → Audio filters → UI wizard
- Run end-to-end test with:
  - Multiple hardware configurations
  - Different CPU/GPU loads
  - Optional network speed input
- Validate fallback behavior
- Validate max retries and minimum config fallback


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


<!-- tree generated by markdown-notes-tree starts here -->

- [**build-aux**](build-aux)
- [**cmake**](cmake)
    - [**common**](cmake/common)
    - [**linux**](cmake/linux)
    - [**macos**](cmake/macos)
        - [**resources**](cmake/macos/resources)
    - [**windows**](cmake/windows)
        - [**resources**](cmake/windows/resources)
- [**data**](data)
    - [**locale**](data/locale)
- [**plans**](plans)
    - [Plan: Initialize and Adapt OBS Plugin Template](plans/01-setup-obs-plugintemplate.md)
- [**src**](src)
    - [**detection**](src/detection)
    - [**filters**](src/filters)
    - [**monitoring**](src/monitoring)
    - [**network**](src/network)
    - [**profile**](src/profile)
    - [**settings**](src/settings)
    - [**sources**](src/sources)
    - [**ui**](src/ui)
- [OBS Setup Plugin Architecture](architecture.md)
- [OBS Plugin Code Conventions](conventions.md)
- [OBS Plugin Flowchart: Open-Source Community MVP](flowchart.md)
- [OBS-Focused Plugin Automatable Setup Guide](functionality.md)
- [stack](stack.md)
- [OBS Setup Plugin Testing Strategy](testing.md)

<!-- tree generated by markdown-notes-tree ends here -->
