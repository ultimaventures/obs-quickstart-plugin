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
```

---

## Module Breakdown

### 1. /detection – System & Encoder Detection

**Purpose:** Identify available hardware, OS, GPU(s), and OBS-compatible encoders.

**Inputs:** OBS runtime environment, system hardware queries (CPU, GPU, RAM, monitors)

**Outputs:** Detected encoders, GPUs, monitor configurations, recommended default encoder

**Error Handling:** Log errors, fallback to software encoder, graceful degradation

**Unit Tests:** Simulate multiple GPU configurations, verify encoder mapping

**API Surface:**

```cpp
class SystemDetector {
public:
    struct EncoderInfo { /* ... */ };
    std::vector<EncoderInfo> detectEncoders();
    std::string getFirstWebcam();
};
```

---

### 2. /network – Upload Speed Assessment

**Purpose:** Estimate sustainable upload bandwidth using public speed test APIs (no streaming).

**Inputs:** Public speed test endpoint

**Outputs:** Estimated sustained upload bandwidth (70–80% multiplier)

**Error Handling:** Timeout fallback to conservative estimate, log errors

**Unit Tests:** Mock API responses, validate bandwidth calculations

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

---

### 4. /settings – Encoder, Bitrate, Resolution Logic

**Purpose:** Determine optimal OBS settings per system/network.

**Inputs:** Hardware info, network metrics, user streaming goals

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
