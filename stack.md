Language: C++17
Build System: CMake 3.16+
Plugin Type: Native OBS plugin (not script)

## Core Dependencies:
- libobs (OBS core API)
- obs-frontend-api (UI/profile/scene collection management)
- Qt5/Qt6 (for setup wizard dialog)
- nlohmann/json (for config storage)
- cpp-httplib or libcurl (for speed test)

## Optional Dependencies:
- obs-websocket API (for external monitoring tools)

## Target Platforms:
- Windows 10/11 (primary)
- macOS 12+ (secondary)
- Linux (Ubuntu 22.04+, future)

## Platform-Specific Dependencies

**macOS (12+):**
- VideoToolbox framework (system)
- Metal framework (for GPU detection)
- Required for Apple Silicon (M1/M2/M3) support

**Note:** VideoToolbox is not optional on macOS - it is the primary hardware encoder for modern Macs.

Minimum OBS Version: 28.0+ (for stable frontend API)