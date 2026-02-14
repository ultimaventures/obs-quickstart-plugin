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

**Tools:**

* GoogleTest or Catch2 for C++
* Mock OBS API interfaces (`obs_mock.h`)
* CI runs all unit tests before merge

**Notes:**

* Testing raw calculation and decision logic is low overhead and highly valuable.
* Mocking OBS calls prevents unsafe operations during automated testing.

---

## 2. Integration Tests

**Purpose:** Validate module interactions and real OBS API integration.

### Scenarios:

1. Profile & Scene Collection Creation

   * Create new profiles on a clean OBS install
   * Verify JSON files generated correctly
2. Source Creation & Configuration

   * Add video/audio sources
   * Apply transformations and settings
3. Filter Application

   * CPU thresholds applied correctly in integrated setup
4. Encoder Initialization

   * Initialize selected encoders and verify availability
5. Edge Cases

   * Multi-GPU systems
   * No webcam available

**Execution:**

* Use temporary OBS profiles to avoid altering user data
* Automated tests via OBS API when possible
* Manual verification for complex GUI workflows

**Notes:**

* Integration tests are critical because the plugin interacts with OBS objects that cannot be fully mocked.
* Running on real OBS ensures crash avoidance before commits.

---

## 3. Memory Tests

**Purpose:** Detect leaks and resource mismanagement.

### Tools:

* Linux: Valgrind, ASAN
* Windows: Dr. Memory, Visual Studio Memory Diagnostics

### Targets:

* Plugin load/unload cycles
* RAII wrappers correctness (OBS object release)
* Long-running sessions (>1 hour simulated)

**Notes:**

* Memory tests are critical for C++ OBS plugins due to the OBS C API and RAII wrapper usage.
* Automated CI memory tests are recommended for Linux builds.

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

**Notes:**

* System tests are essential for verifying assumptions on hardware capabilities and cross-platform encoder availability.
* Automated VM testing is possible for Windows; physical machines preferred for GPU tests.

---

## 5. CI/CD Recommendations

* Run unit tests for every PR
* Run integration tests on nightly builds
* Include memory checks on Linux CI using Valgrind/ASAN
* Tag system tests for manual execution on real hardware

---

## 6. Optional Considerations / Effort vs Value

* **Low priority:** exhaustive combinations of every possible webcam/monitor configuration; most users fit standard setups
* **High priority:** encoding, profile creation, OBS API interaction, memory safety, cross-platform support

---

**Conclusion:**
This testing strategy ensures the OBS Setup Plugin is robust, memory-safe, and compatible across hardware and OS configurations. It balances automated tests with necessary manual verification for integration and system-level behaviors.
