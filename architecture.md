# OBS Setup Plugin Architecture

This document outlines the architecture of the OBS Setup Plugin, designed to automate 70–80% of the setup process for 80–90% of users. The plugin is modular, testable, and extensible to accommodate different system configurations.

## Directory Structure

```
/src
  /detection      # System & encoder detection
  /network        # Upload speed assessment
  /profile        # New profile & scene collection creation
  /settings       # Encoder, bitrate, resolution logic
  /sources        # Scene & source creation
  /filters        # Conditional audio filter application
  /monitoring     # Local recording test & metrics
  /ui             # Setup dialog and user interaction
  plugin-main.c
  plugin-support.c
  plugin-support.h
  /data
    /locale
  /cmake
  CMakeLists.txt
```

---

## Module Breakdown

### 1. /detection – System & Encoder Detection

**Purpose:** Detect available hardware and recommend encoder

**What We Can Detect:**
- ✅ CPU core count (via `std::thread::hardware_concurrency()`)
- ✅ Available encoders (via OBS encoder enumeration)
- ✅ Which platform (Windows/macOS/Linux)

**Detection Strategy:**

**Simple Binary Classification:**
```cpp
struct HardwareInfo {
    int cpuCores;              // Actual count from system
    std::string bestEncoder;   // Best available from OBS
    bool hasHardwareEncoder;   // NVENC/AMF/QSV/VideoToolbox exists?
};
```

**Encoder Priority (automatically determined by OBS availability):**
1. Check if NVENC exists → use it
2. Check if AMF exists → use it
3. Check if QSV exists → use it
4. Check if VideoToolbox exists → use it
5. Fall back to x264

**Error Handling:** Log errors, fallback to software encoder, graceful degradation

**Unit Tests:** Simulate multiple GPU configurations, verify encoder mapping

**Encoder Priority by Platform:**

**Windows:**
1. NVENC (NVIDIA RTX 2000+)
2. AMF (AMD RX 5000+)
3. QSV (Intel 7th gen+)
4. x264 (software fallback)

**macOS:**
1. VideoToolbox (Apple Silicon M1/M2/M3 - native hardware)
2. VideoToolbox (Intel Mac with T2 chip)
3. x264 (software fallback for older Macs)

**Linux:**
1. VAAPI (Intel/AMD)
2. NVENC (NVIDIA with proprietary drivers)
3. x264 (software fallback)

**Detection Logic:**
```cpp
std::vector SystemDetector::detectEncoders() {
    std::vector encoders;
    
#ifdef __APPLE__
    // macOS - check VideoToolbox first
    if (isVideoToolboxAvailable()) {
        encoders.push_back({
            "com.apple.videotoolbox.videoencoder.h264",
            "VideoToolbox H.264",
            true,
            100  // Highest priority
        });
    }
#endif

#ifdef _WIN32
    // Windows - check NVENC
    if (isNvencAvailable()) {
        encoders.push_back({
            "ffmpeg_nvenc",
            "NVIDIA NVENC H.264",
            true,
            90
        });
    }
    // ... AMD, QSV
#endif

    // Software fallback (all platforms)
    encoders.push_back({
        "obs_x264",
        "Software x264",
        true,
        10  // Lowest priority
    });
    
    return encoders;
}
```

**Apple Silicon Considerations:**
- VideoToolbox on M1/M2/M3 is FASTER and higher quality than x264
- Must be default choice on Apple Silicon
- Different quality presets than NVENC/AMF
- Test on actual Mac hardware (not just CI)

**API Surface:**

```cpp
class SystemDetector {
public:
    HardwareInfo detect();
    std::string getFirstWebcam();
private:
    std::string detectBestEncoder();
    bool encoderExists(const std::string& encoderId);
};
```

---

### 2. /network – Upload Speed Assessment

**Purpose:** Estimate sustained upload bandwidth using public speed test APIs

**Inputs:** Public speed test endpoint (speedtest.net or similar)

**Outputs:** Estimated sustained upload bandwidth (70-80% multiplier applied)

**CRITICAL: Timeout & Security Requirements**

**Timeout Limits:**
- Connection timeout: 5 seconds
- Total test timeout: 30 seconds maximum
- No retries on failure - fail fast

**SSL Verification:**
- MUST verify SSL certificates for speed test endpoints
- Reject self-signed certificates
- Use system certificate store

**Error Handling:**
- Network unreachable → Default to 3500 kbps (safe minimum)
- Firewall/proxy blocks request → Default to 3500 kbps
- API rate limit (429) → Default to 3500 kbps
- Timeout → Default to 3500 kbps
- Any error → Log, show user warning, proceed with defaults

**Fallback Behavior:**
```cpp
double NetworkTester::runSpeedTest() {
    try {
        httplib::Client client("https://speedtest.net");
        client.set_connection_timeout(5);  // 5 second connect
        client.set_read_timeout(30);       // 30 second total
        
        auto result = client.Post("/test", payload);
        if (result && result->status == 200) {
            return parseSpeed(result->body);
        }
    } catch (const std::exception& e) {
        blog(LOG_WARNING, "[Network] Speed test failed: %s", e.what());
    }
    
    // Always return safe default on any error
    blog(LOG_INFO, "[Network] Using conservative default: 3500 kbps");
    return 3500.0; // Conservative minimum
}
```

**UI Messaging:**
- Show spinner: "Testing upload speed... (30 sec max)"
- On failure: "Speed test unavailable, using conservative defaults"
- Allow skip button: User can skip speed test entirely

**API Surface:**

```cpp
class NetworkTester {
public:
    double runSpeedTest(); // returns Mbps
};
```

---

### 3. /profile – New Profile & Scene Collection Creation

**Purpose:** Create a NEW OBS profile and scene collection (never modify existing).

**Inputs:** Hardware detection, user platform preferences

**Outputs:**

* NEW profile: `AutoSetup_Beginner_[Platform]_[Date]`
* NEW scene collection

**Critical:** Backup existing configuration before creating new profile

**Error Handling:** Rollback profile creation on failure

**Unit Tests:** Validate JSON creation, backup & rollback logic

**API Surface:**

```cpp
class ProfileManager {
public:
    bool createNewProfile(const std::string& name);
    bool backupExistingProfile();
};
```

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

### 4. /settings – Encoder, Bitrate, Resolution Logic

**Purpose:** Determine optimal OBS settings per system/network.

**Input:** HardwareInfo from detection module

**Decision Logic (Simple Rules):**

**If hardware encoder available:**
- Resolution: 1920x1080
- FPS: 60
- Bitrate: 6000 kbps
- Encoder: Detected hardware encoder
- Preset: "quality" or equivalent

**If software encoding + CPU >= 8 cores:**
- Resolution: 1280x720
- FPS: 60
- Bitrate: 4500 kbps
- Encoder: obs_x264
- Preset: "veryfast"

**If software encoding + CPU < 8 cores:**
- Resolution: 1280x720
- FPS: 30
- Bitrate: 2500 kbps
- Encoder: obs_x264
- Preset: "ultrafast"

**Philosophy:**
Binary classification only: hardware encoder OR software encoder.
No complex GPU tier detection - not worth the maintenance burden.
Settings are conservative starting points, not final configuration.

**Outputs:** Encoder settings, resolution/FPS, keyframe interval, preset

**Error Handling:** Validate OBS constraints, fallback to safe presets

**Unit Tests:** Test CPU/GPU/network scenarios, validate limits

**API Surface:**

```cpp
class SettingsManager {
public:
    void applyEncoderSettings();
    void calculateBitrate();
};
```

---

### 5. /sources – Scene & Source Creation

**Purpose:** Generate scenes and add sources.

**Inputs:** Scene collection, available sources, user preferences

**Outputs:** Configured scenes with sources attached

**Error Handling:** Log unsupported sources, skip invalid sources

**Unit Tests:** Validate scene/source hierarchy and placement

**API Surface:**

```cpp
class SourceManager {
public:
    bool addSourceToScene(const std::string& scene, const std::string& source);
};
```

---

### 6. /filters – Conditional Audio Filter Application

**Purpose:** Apply audio filters based on CPU headroom

**Inputs:** Audio sources, CPU usage from monitoring test

**Outputs:** Applied filters

**Logic:**

* CPU < 60% → Apply RNNoise + Compressor + Limiter
* CPU >= 60% → Apply Speex + Noise Gate only

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

1. /detection (no dependencies)
2. /network (parallel possible)
3. /settings (depends on detection & network)
4. /profile (depends on settings)
5. /sources (depends on profile & detection)
6. /filters (depends on sources & monitoring)
7. /monitoring (depends on profile, settings, sources)
8. /ui (orchestrates all modules)

**Critical:** /profile before applying settings; /monitoring after initial setup

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

**Storage:** $OBS_CONFIG/plugin_config/obs-setup/config.json
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