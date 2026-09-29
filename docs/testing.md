# OBS Setup Plugin Testing Strategy

This document defines the testing strategy for the OBS Setup Plugin. It covers unit, integration, memory, system, and UI tests, aligned with the project stack, architecture, and coding conventions.

---

## 1. Unit Tests

**Purpose:** Validate individual modules, calculation logic, and error handling in isolation.

### Modules & Coverage:

1. **/detection**

   * Mock configurations with/without webcam
   * Test CPU count reporting
   * <s>Test `detectEncoders()` returns expected encoders</s> *(removed 2026-09-28: encoder detection is owned by OBS's Auto-Configuration Wizard)*
2. **/settings**

   * Validate keyframe intervals, presets, audio settings
   * <s>Bitrate calculation with mocked network speeds; resolution and encoder selection logic</s> *(removed 2026-09-28: owned by the Auto-Configuration Wizard)*
3. **/filters**

   * CPU headroom estimation logic
   * Conditional filter application paths
4. **/audio**

   * Mic check states: configured / present-but-unselected / none-present
   * One-click assign, re-scan, reminder-choice persistence
   * Troubleshooter automated checks (muted? wrong device? track mismatch?)
5. **/hotkeys**

   * Scene hotkeys bound to the correct scenes
   * Shortcut dock lists exactly the bound mappings
6. **Error Handling**

   * Mock OBS API failures
   * Ensure graceful fallback and rollback occurs

**Framework:** Google Test (gtest) for C++

**Mock Implementation Strategy:**

* **Option A (Recommended):** Wrap OBS API calls in thin interfaces and mock the wrapper.
* Example:

```cpp
class IOBSInterface { virtual obs_source_t* createSource(...) = 0; };
class OBSReal : public IOBSInterface { /* calls real OBS */ };
class OBSMock : public IOBSInterface { /* controlled test data */ };
```

* Avoid mocking OBS internals or callbacks

**Test Naming Convention:** `ModuleName_FunctionalityUnderTest_ExpectedBehavior`
* Example: `SettingsManager_ApplyEncoderPreset_SetsP5`

**Coverage Requirement:** >80% for core modules (detection, settings, sources, filters, monitoring, audio, hotkeys)

**Unit Test Examples:**

```cpp
TEST(SystemDetector, GetFirstWebcam_WebcamPresent_ReturnsDeviceId) {
    OBSMock mockOBS;
    mockOBS.setAvailableWebcams({"webcam-0"});
    SystemDetector detector(mockOBS);
    EXPECT_EQ(detector.getFirstWebcam(), "webcam-0");
}
```

---

## 2. Integration Tests

**Purpose:** Validate module interactions and real OBS API integration.

### Scenarios:

1. Profile & Scene Collection Creation
2. Source Creation & Configuration
3. Filter Application
<s>4. Encoder Initialization</s> *(removed 2026-09-28: encoder setup is owned by the Auto-Configuration Wizard)*
4. Edge Cases (multi-GPU, no webcam, no mic)

**Execution:**

* Install OBS Studio, set `OBS_TEST_MODE=1`
* Use headless mode (Xvfb on Linux)
* Run via scripts (`./run_integration_tests.sh` Linux, `obs-test-runner.exe` Windows)
* Use temporary OBS profiles to avoid altering user data
* Automated tests via OBS API when possible
* Manual verification for complex GUI workflows
* Clean up test profiles after execution

**Test Examples:**

**Test:** Profile creation
```cpp
TEST_F(ProfileIntegrationTest, CreateProfile_ExistingProfilePresent_LeavesOriginalUntouched) {
    obs_frontend_set_current_profile("UserProfile");
    ProfileManager pm;
    ASSERT_TRUE(pm.createNewProfile("AutoSetup_Test"));
    EXPECT_TRUE(profileExists("UserProfile"));
    EXPECT_EQ(obs_frontend_get_current_profile(), "AutoSetup_Test");
    obs_frontend_remove_profile("AutoSetup_Test");
}
```
**Test:** Source creation with auto-detected webcam
```cpp
TEST_F(SourceIntegrationTest, CreateWebcam_DeviceDetected_AddsToScene) {
    // Setup: Ensure webcam available
    ASSERT_TRUE(hasWebcamDevice());
    
    // Execute: Create webcam source
    SourceManager sm;
    ASSERT_TRUE(sm.addWebcamSource("Live", "Webcam"));
    
    // Verify: Source exists in scene
    obs_source_t* scene = obs_get_source_by_name("Live");
    EXPECT_TRUE(sceneContainsSource(scene, "Webcam"));
    obs_source_release(scene);
}
```

**Test:** Filter application conditional on CPU
```cpp
TEST_F(FilterIntegrationTest, ApplyFilters_HighCPU_UsesSpeexNotRNNoise) {
    // Mock high CPU usage
    mockMonitor.setCPUUsage(75.0);
    
    // Execute: Apply filters
    FilterManager fm;
    fm.applyFilters(audioSource);
    
    // Verify: Speex applied, RNNoise not applied
    EXPECT_TRUE(sourceHasFilter(audioSource, "Speex"));
    EXPECT_FALSE(sourceHasFilter(audioSource, "RNNoise"));
}
```

* Include source creation, filter application, encoder init, edge cases

---

## 3. Memory Tests

**What to Check:**
- All `obs_source_create()` matched with `obs_source_release()`
- All `obs_scene_create()` matched with `obs_scene_release()`
- No leaks after plugin unload
- No leaks after wizard cancel/error paths
- RAII wrappers release correctly

**Tools:** Valgrind (Linux), Dr. Memory (Windows)

**Focus:**

* Match every `obs_source_create()` / `obs_source_release()`
* Plugin unload, wizard cancel
* Long-running sessions

**Example Valgrind Command:**

```bash
valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes --log-file=valgrind.log obs --portable
```

**CI:** Nightly runs, any new leaks block release

---

## 4. System Tests

**Purpose:** Validate plugin behavior under real hardware, OS, and profile conditions.

### Hardware Scenarios:

1. Low-End: i3 CPU, integrated graphics
2. Mid-Range: i5 CPU, GTX 1060
3. High-End: i9 CPU, RTX 4090

### OS/Platform Scenarios:

* Windows 10 (with NVENC, without GPU)
* Linux (different encoder availability)
* OBS with pre-existing user profiles

### Checks:

* Plugin does not crash
* Existing profiles remain untouched
* All settings applied correctly
* Performance metrics within expected thresholds

### macOS Specific

**Required Hardware:**
- Intel Mac with T2 chip (2018+)
- Apple Silicon Mac (M1/M2/M3)


---

## 5. UI Testing

**Manual:** Walkthrough wizard, input validation, error dialogs, cancel/back behavior

**Automated (Optional):** Qt Test framework

```cpp
QTest::mouseClick(wizard.platformComboBox, Qt::LeftButton);
QTest::keyClicks(wizard.platformComboBox, "Twitch");
QCOMPARE(wizard.getSelectedPlatform(), "Twitch");
```

**Checklist:** Buttons clickable, inputs validated, progress updated, cancel works, errors clear

---

## 6. Thread Safety Testing

**Techniques:**

**Debug Assertions:** Verify main thread in OBS API calls
```cpp
void ProfileManager::createProfile() {
    assert(QThread::currentThread() == qApp->thread());
    // ... OBS API calls
}
```

**Thread Sanitizer (TSan):** Detect race conditions
```bash
cmake -DCMAKE_CXX_FLAGS="-fsanitize=thread" ..
./run_tests  # TSan will report data races
```
* Manual simulation of concurrent network/UI operations
* Stress testing worker threads

**CI:** Run TSan on Linux CI; flag all races
**Recording Test Thread Safety:**
- Recording test MUST use Qt timers, not blocking sleep
- Test for UI responsiveness during 30-second test
- Verify progress updates every second
- Ensure cancel button works during test

---


## 8. Test Organization & Commands

**Directory Structure:**

```
/tests
  /unit
  /integration
  /mocks
  /fixtures
  CMakeLists.txt
```

**Commands:**

* Unit: `./tests/unit_tests --gtest_filter=*`
* Integration: `./tests/integration_tests`
* Coverage: `make coverage`
* CI example:

```yaml
test:
  script:
    - cmake -DCMAKE_BUILD_TYPE=Debug ..
    - make
    - ctest --output-on-failure
    - make coverage
```

---

## 9. CI/CD Integration Plan

* **Unit Tests:** Run on every pull request; must pass before merge
* **Integration Tests:** Run on nightly builds or pre-release testing
* **Memory Tests:** Linux CI uses Valgrind/ASAN; Windows optionally with Dr. Memory
* **Coverage Enforcement:** Core modules must have >80% coverage, reported in CI
* **System Tests:** Marked for manual execution on physical or VM environments; automated where feasible (Windows VMs, Linux containers)
* **Format/Conventions Check:** `clang-format` 19 applied in CI
* **Fail Conditions:** Unit test failures, coverage <80%, or critical memory leaks block merge
