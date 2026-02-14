Language: C++17
Build System: CMake 3.16+
Plugin Type: Native OBS plugin (not script)

Core Dependencies:
- libobs (OBS core API)
- obs-frontend-api (UI/profile/scene collection management)
- Qt5/Qt6 (for setup wizard dialog)
- nlohmann/json (for config storage)
- cpp-httplib or libcurl (for speed test)

Optional Dependencies:
- obs-websocket API (for external monitoring tools)

Target Platforms:
- Windows 10/11 (primary)
- macOS 12+ (secondary)
- Linux (Ubuntu 22.04+, future)

Minimum OBS Version: 28.0+ (for stable frontend API)