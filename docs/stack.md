
## Language & Standards

**Language:** C++17

**Why C++ over C:**
- Qt framework requires C++ (for UI wizard)
- RAII for safer OBS object management
- STL containers (std::vector, std::string) for convenience
- Class-based architecture for modularity

**OBS Compatibility:**
- OBS Studio core is C
- Plugins can be C or C++
- Entry points must use `extern "C"` linkage
- All OBS API calls are C-compatible

**Template Conversion:**
obs-plugintemplate uses .c files by default (minimal example).
We convert to .cpp and add:
- `extern "C"` wrapper for entry points
- C++17 standard in CMake
- Qt integration

Build System: CMake 3.16+
Plugin Type: Native OBS plugin (not script)

## Core Dependencies:
- libobs (OBS core API)
- obs-frontend-api (UI/profile/scene collection management)
- Qt5/Qt6 (for setup wizard dialog)
- nlohmann/json (for config storage)

## Optional Dependencies:
- obs-websocket API (for external monitoring tools)

## Target Platforms:
- Windows 10/11 (primary)
- macOS 12+ (secondary)
- Linux (Ubuntu 22.04+, future)

## Platform-Specific Dependencies

**macOS (12+):**
- VideoToolbox framework (system)
- Metal framework (system)
- Required for Apple Silicon (M1/M2/M3) support

**Note:** VideoToolbox is not optional on macOS - it is the primary hardware encoder for modern Macs.

Build target: OBS 31.1.1+ (matches `buildspec.json` — the plugin is built against the OBS 31.1.1 SDK from the plugin template; older runtimes are untested and backwards compatibility is to be verified)