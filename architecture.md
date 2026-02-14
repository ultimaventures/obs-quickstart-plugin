# OBS Setup Plugin Architecture

This document outlines the architecture of the OBS Setup Plugin, designed to automate 70–80% of the setup process for 80–90% of users. The plugin is modular, testable, and extensible to accommodate different system configurations.

## Directory Structure

```
/src
  /detection      # System & encoder detection
  /network        # Speed test wrapper
  /profile        # Profile/scene collection creation
  /settings       # Encoder, bitrate, resolution logic
  /sources        # Scene & source creation
  /filters        # Audio filter application
  /monitoring     # Recording/test & metrics
  /ui             # Setup dialog and user interaction
```

---

## Module Breakdown

### 1. /detection – System & Encoder Detection

**Purpose:** Identify available hardware, OS, GPU(s), and OBS-compatible encoders.

**Flowchart Phases:** System Scan, Encoder Selection

**Inputs:** OBS runtime environment, system hardware queries (CPU, GPU, RAM, monitors)

**Outputs:** Available encoders, detected GPUs, monitor configurations, recommended default encoder

**Error Handling:** Warn if no compatible encoder found, fallback to software encoder, log hardware query errors

**Unit Tests:** Simulate multiple GPU configurations, verify correct encoder mapping

### 2. /network – Speed Test Wrapper

**Purpose:** Determine optimal bitrate/resolution based on network speed.

**Flowchart Phases:** Network Assessment → Bitrate/Resolution Suggestion

**Inputs:** Internet speed, latency/ping to streaming servers

**Outputs:** Suggested max bitrate, connection reliability metrics

**Error Handling:** Timeout fallback to conservative bitrate, default safe profile on invalid speed test

**Unit Tests:** Mock network latency and throughput variations, validate bitrate calculation logic

### 3. /profile – Profile & Scene Collection Creation

**Purpose:** Create OBS profiles and scene collections programmatically.

**Flowchart Phases:** Profile Setup → Scene Setup

**Inputs:** Detected hardware, streaming platform preferences, optional user defaults

**Outputs:** OBS profile JSON, scene collection JSON

**Error Handling:** Fail gracefully if profile creation fails, rollback partial profile creation

**Unit Tests:** Verify profile and scene JSON generation, test rollback scenarios

### 4. /settings – Encoder, Bitrate, Resolution Logic

**Purpose:** Determine optimal OBS settings per system/network.

**Flowchart Phases:** Bitrate & Resolution Selection → Encoder Configuration

**Inputs:** Hardware detection, network metrics, user streaming goals

**Outputs:** Encoder settings, output resolution & FPS, keyframe interval, preset, rate control

**Error Handling:** Fall back to safe presets, validate each setting against OBS constraints

**Unit Tests:** Test combinations of CPU/GPU/network scenarios, validate limits and fallback behavior

### 5. /sources – Scene & Source Creation

**Purpose:** Automatically generate scenes and add sources (webcams, game capture, images).

**Flowchart Phases:** Scene Setup → Source Addition

**Inputs:** Scene collection, available sources, optional user preferences

**Outputs:** Configured scenes with sources attached, source visibility, positioning, scaling

**Error Handling:** Log missing or unsupported sources, skip invalid sources without aborting setup

**Unit Tests:** Mock source discovery, validate scene hierarchy and source placement

### 6. /filters – Audio Filter Application

**Purpose:** Apply recommended filters (noise suppression, gain, compressor) to audio sources.

**Flowchart Phases:** Audio Filter Setup

**Inputs:** Audio sources, recommended filter presets

**Outputs:** Configured filters attached to sources, validation of filter parameters

**Error Handling:** Skip unsupported filters, warn if filter application fails

**Unit Tests:** Validate filter attachment, test parameter bounds

### 7. /monitoring – Recording Test & Metrics

**Purpose:** Run a test recording/stream to validate settings.

**Flowchart Phases:** Validation → Metrics Feedback

**Inputs:** Configured profile and scene collection, test duration

**Outputs:** Recording performance metrics (FPS, dropped frames, CPU/GPU load), success/failure flag

**Error Handling:** Abort if recording fails, suggest adjusted settings

**Unit Tests:** Simulate test recording, validate adjustment recommendations

### 8. /ui – Setup Dialog & User Interaction

**Purpose:** Provide guided setup flow and feedback to user.

**Flowchart Phases:** User Interaction & Confirmation → Optional Manual Adjustments

**Inputs:** Outputs from all previous modules, user input

**Outputs:** Final configuration confirmation, optional logging/reporting

**Error Handling:** Validate user input, graceful fallback if UI cannot load

**Unit Tests:** Test UI flows with mock outputs, validate error dialogs

---

## General Notes

* All modules should log operations for transparency and troubleshooting.
* Each module must expose functions suitable for unit testing with dependency injection.
* Modules should be loosely coupled to allow easy extension.
* Edge cases (multiple GPUs, exotic Linux setups, multi-webcam setups) should be handled gracefully but may fall outside automation scope.

[Flowchart](flowchart.md) Phase     → Module
-------------------------------------------------------
System Scan / Hardware Detection → /detection
Encoder Selection                 → /detection
Network Assessment                → /network
Bitrate/Resolution Suggestion     → /settings
Profile Setup                     → /profile
Scene Setup                        → /profile + /sources
Source Addition                    → /sources
Audio Filter Setup                 → /filters
Validation / Test Recording        → /monitoring
Metrics Feedback                   → /monitoring
User Interaction / Confirmation    → /ui
Optional Manual Adjustments        → /ui

Inputs/Outputs flow
/detection -> hardware info, encoders -> /profile, /settings, /sources
/network   -> network metrics       -> /settings
/profile   -> profile/scene JSON    -> /sources
/settings  -> encoder/bitrate/res   -> /sources, /monitoring
/sources   -> configured scenes     -> /filters, /monitoring
/filters   -> applied audio filters -> /monitoring
/monitoring -> performance metrics  -> /ui
/ui        -> final confirmation    -> user
