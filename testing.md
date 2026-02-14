# OBS Setup Plugin Testing Strategy

This document defines the testing strategy for the OBS Setup Plugin. It covers unit, integration, memory, and system tests, aligned with the project stack, architecture, and coding conventions.

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

**Framework:**

* Use **Google Test (gtest)** for C++ unit testing
* Catch2 can be considered for lightweight modules, but gtest is preferred for CI integration

**Mock Strategy:**

* Create mock headers for OBS API (`obs_mock.h`) implementing the same interface
* Mock return values, simulate failures, and track calls for verification

**Test Naming Convention:**

* `ModuleName_FunctionalityUnderTest_ExpectedBehavior`
* Example: `SettingsManager_CalculateBitrate_ReturnsSafeValue`

**Coverage Requirements:**

* Aim for **>80% code coverage** for all core modules (detection, settings, sources, filters, monitoring)
* CI must report coverage; coverage <80% fails merge for critical modules

**Notes:**

* Testing calculation and decision logic is low overhead and high value
* Mocking OBS calls prevents unsafe operations during automated testing

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

* Use temporary OBS profiles to avoid altering user data
* Automated tests via OBS API when possible
* Manual verification for complex GUI workflows

**Notes:**

* Integration tests ensure OBS object safety and avoid runtime crashes

---

## 3. Memory Tests

**Purpose:** Detect leaks and resource mismanagement.

**Tools:**

* Linux: Valgrind, ASAN
* Windows: Dr. Memory, Visual Studio Memory Diagnostics

**Targets:**

* Plugin load/unload cycles
* RAII wrapper correctness
* Long-running sessions (>1 hour simulated)

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

## 5. CI/CD Integration Plan

* **Unit Tests:** Run on every pull request; must pass before merge
* **Integration Tests:** Run on nightly builds or pre-release testing
* **Memory Tests:** Linux CI uses Valgrind/ASAN; Windows optionally with Dr. Memory
* **Coverage Enforcement:** Core modules must have >80% coverage, reported in CI
* **System Tests:** Marked for manual execution on physical or VM environments; automated where feasible (Windows VMs, Linux containers)
* **Format/Conventions Check:** `clang-format` v15 applied in CI
* **Fail Conditions:** Unit test failures, coverage <80%, or critical memory leaks block merge

---

**Conclusion:**
This strategy ensures robust, memory-safe, and cross-platform compatible plugin development. OBS API mocking, strict CI/CD enforcement, and coverage metrics maintain quality while balancing manual verification for system-level tests.
