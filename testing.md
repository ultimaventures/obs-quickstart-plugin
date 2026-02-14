# OBS Setup Plugin Testing Strategy

This document defines the testing strategy for the OBS Setup Plugin. It covers unit, integration, memory, system, and UI tests, aligned with the project stack, architecture, and coding conventions.

---

## 1. Unit Tests

**Purpose:** Validate individual modules, calculation logic, and error handling in isolation.

### Modules & Coverage:

1. **/detection**

   * Mock multiple hardware configurations (CPU, GPU, webcams)
   * Test `detectEncoders()` returns expected encoders
2. **/settings**

   * Bitrate calculation with mocked network speeds
   * Resolution and encoder selection logic
   * Validate keyframe intervals, presets
3. **/filters**

   * CPU headroom estimation logic
   * Conditional filter application paths
4. **Error Handling**

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
* Example: `SettingsManager_CalculateBitrate_ReturnsSafeValue`

**Coverage Requirement:** >80% for core modules (detection, settings, sources, filters, monitoring)

**Unit Test Examples:**

```cpp
TEST(SystemDetector, DetectEncoders_NvencAvailable_ReturnsNvencFirst) {
    OBSMock mockOBS;
    mockOBS.setAvailableEncoders({"ffmpeg_nvenc", "obs_x264"});
    SystemDetector detector(mockOBS);
    auto encoders = detector.detectEncoders();
    ASSERT_GE(encoders.size(), 1);
    EXPECT_EQ(encoders[0].id, "ffmpeg_nvenc");
}
```

---

## 2. Integration Tests

**Purpose:** Validate module interactions and real OBS API integration.

### Scenarios:

1. Profile & Scene Collection Creation
2. Source Creation & Configuration
3. Filter Application
4. Encoder Initialization
5. Edge Cases (multi-GPU, no webcam)

**Execution:**

* Install OBS Studio, set `OBS_TEST_MODE=1`
* Use headless mode (Xvfb on Linux)
* Run via scripts (`./run_integration_tests.sh` Linux, `obs-test-runner.exe` Windows)
* Use temporary OBS profiles to avoid altering user data
* Automated tests via OBS API when possible
* Manual verification for complex GUI workflows
* Clean up test profiles after execution

**Test Examples:**

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

* Include source creation, filter application, encoder init, edge cases

---

## 3. Memory Tests

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

* Thread Sanitizer (TSan) for race detection
* Manual simulation of concurrent network/UI operations
* Stress testing worker threads

**CI:** Run TSan on Linux CI; flag all races

---

## 7. Test Organization & Commands

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

## 5. CI/CD Integration Plan

* **Unit Tests:** Run on every pull request; must pass before merge
* **Integration Tests:** Run on nightly builds or pre-release testing
* **Memory Tests:** Linux CI uses Valgrind/ASAN; Windows optionally with Dr. Memory
* **Coverage Enforcement:** Core modules must have >80% coverage, reported in CI
* **System Tests:** Marked for manual execution on physical or VM environments; automated where feasible (Windows VMs, Linux containers)
* **Format/Conventions Check:** `clang-format` v15 applied in CI
* **Fail Conditions:** Unit test failures, coverage <80%, or critical memory leaks block merge

**Conclusion:**
Provides actionable detail for inexperienced developers to implement, run, and maintain unit, integration, memory, system, and UI tests. Ensures OBS API safety, memory correctness, thread safety, and cross-platform reliability.
