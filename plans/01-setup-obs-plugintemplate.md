# Plan: Initialize and Adapt OBS Plugin Template

## Objective
Initialize the project codebase by adopting the `obs-plugintemplate`, converting it to C++17, and refactoring it to match the modular architecture defined in `obs-quickstart-plugin/architecture.md`.

## Prerequisites
- `obs-quickstart-plugin/` documentation (Reviewed)
- `obs-plugintemplate` repository URL: `https://github.com/obsproject/obs-plugintemplate`

## Implementation Steps

### 1. Initialize Repository from Template
- **Action:** Clone `obs-plugintemplate` into a temporary directory.
- **Action:** Move the template files into the project root, excluding `.git`.
- **Action:** Ensure `obs-quickstart-plugin/` and `plans/` are preserved.

### 2. Convert to C++ and Reorganize Structure
- **Action:** Rename `src/plugin-main.c` to `src/plugin-main.cpp`.
- **Action:** Wrap OBS entry points in `extern "C"` in `src/plugin-main.cpp`.
- **Action:** Create module subdirectories in `src/`:
  - `src/detection/`, `src/network/`, `src/profile/`, `src/settings/`, `src/sources/`, `src/filters/`, `src/monitoring/`, `src/ui/`
- **Action:** Create placeholder headers/sources for each module.

### 3. Update CMake Configuration
- **Action:** Modify root `CMakeLists.txt`:
  - Set project name to `obs-quickstart-plugin`.
  - Set `LANGUAGES CXX`, `CMAKE_CXX_STANDARD 17`, and `CMAKE_CXX_STANDARD_REQUIRED ON`.
- **Action:** Update `src/CMakeLists.txt` to include new subdirectories and `src/plugin-main.cpp`.
- **Action:** Configure dependencies (`libobs`, `Qt6`, `nlohmann/json`, `cpp-httplib`).

### 4. CI/CD Migration
- **Action:** Remove `.github/` directory.
- **Action:** Create `.gitlab-ci.yml` with stages for `build`, `test`, and `package`.

### 5. Documentation Updates
- **Action:** Update `STACK.md` with C++17 justification.
- **Action:** Update `CONVENTIONS.md` with C++ include order rules.

### 6. Cleanup & Standardization
- **Action:** Update `.gitignore`.
- **Action:** Apply `clang-format 16` to all files.

## Verification
1. **Build Check:** Run `cmake -S . -B build` and `cmake --build build`.
2. **Structure Check:** Verify all module folders exist in `src/`.
3. **Log Check:** Load in OBS and verify "Plugin loaded" message.
