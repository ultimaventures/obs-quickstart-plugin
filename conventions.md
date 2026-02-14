# OBS Plugin Code Conventions

This document defines coding, file, and documentation conventions for the OBS Setup Plugin to ensure consistent style and maintainability across the C++ codebase.

---

## 1. Naming Conventions

**Classes:** PascalCase (e.g., `SystemDetector`, `ProfileManager`)
**Methods / Functions:** camelCase (e.g., `detectEncoders()`, `runLocalRecordingTest()`)
**Variables / Members:** camelCase with mandatory `m_` prefix for member variables (e.g., `m_profileName`)
**Constants:** ALL_CAPS with underscores (e.g., `MAX_CPU_THRESHOLD`)
**Namespaces:** lowercase with underscores (e.g., `obs_setup`)
**Const Correctness:** Use `const` for immutable variables, `const&` for read-only parameters, mark methods `const` if they don't modify state
**Auto Usage:** Use `auto` for iterators and obvious RHS types; avoid if type info is important

---

## 2. File Naming

* Source files: snake_case.cpp (e.g., `system_detector.cpp`)
* Header files: snake_case.h (e.g., `profile_manager.h`)
* Private implementation: `*_impl.h`

---

## 3. Header Guard Style

* `PROJECT_MODULE_FILENAME_H` pattern, all uppercase

```cpp
#ifndef OBS_SETUP_PROFILE_MANAGER_H
#define OBS_SETUP_PROFILE_MANAGER_H
#endif // OBS_SETUP_PROFILE_MANAGER_H
```

---

## 4. Comment Style

* Doxygen for classes, functions, and modules
* Inline comments: `//` only when necessary

```cpp
/** Detects available OBS encoders. */
std::vector<EncoderInfo> detectEncoders();
```

---

## 5. Git Commit Message Format

* Conventional Commits style:

```
<type>(<scope>): <short summary>
```

* Types: `feat`, `fix`, `docs`, `refactor`, `test`, `chore`
* Example: `feat(profile): add new profile creation API`

---

## 6. Code Formatting (clang-format)

* Use clang-format version 15 exactly
* Pin in CI and ensure developers use same version
* Example `.clang-format` snippet:

```yaml
BasedOnStyle: Google
IndentWidth: 4
ColumnLimit: 100
AllowShortFunctionsOnASingleLine: InlineOnly
PointerAlignment: Left  # int* ptr style
```
* Run `clang-format -i <file>` before commit

---

## 7. Error Handling Patterns

* Return `bool` for success/failure
* Use `std::optional<T>` or `std::expected<T, Error>` for detailed errors
* NEVER throw exceptions across OBS plugin boundary
* Rollback partial changes on failure
* Pattern:

```cpp
if (!createNewProfile()) {
    blog(LOG_ERROR, "Failed to create profile");
    rollbackProfile();
    return false;
}
```

---

## 8. Logging Format

* Use OBS `blog()` function
* Format: `[MODULE] Level: Message`
* Levels: `LOG_INFO`, `LOG_WARNING`, `LOG_ERROR`
* Example:

```cpp
blog(LOG_INFO, "[Network] Upload speed measured: %.2f Mbps", speedMbps);
blog(LOG_ERROR, "[Profile] Failed to backup existing profile");
```

---

## 9. OBS API Memory Management

**All OBS objects MUST use RAII wrappers:**

```cpp
// WRONG - leak
obs_source_t* source = obs_source_create(...);

// RIGHT - auto-release wrapper
OBSSource source = obs_source_create(...);
// OR
obs_source_t* raw = obs_source_create(...);
OBSSourceAutoRelease holder(raw);
```

**Reference counting:**

* `obs_*_get_*()` → increment ref count → must release
* `obs_*_create()` → ref count 1 → must release
* Use `obs_source_get_ref()` / `obs_source_release()`
* NEVER store raw pointers without ownership clarity

**RAII wrappers location:** `/src/common/obs_wrappers.h`

```cpp
class OBSSourceAutoRelease {
    obs_source_t* source;
public:
    explicit OBSSourceAutoRelease(obs_source_t* s) : source(s) {}
    ~OBSSourceAutoRelease() { if (source) obs_source_release(source); }
};
```

---

## 10. Thread Safety & Concurrency

* All OBS API calls on main thread
* Worker threads allowed for network tests, I/O, heavy computation
* Cross-thread communication via Qt signals or `QMetaObject::invokeMethod`
* Mutex only for shared state; NEVER while calling OBS API
* Debug asserts for main thread:

```cpp
assert(QThread::currentThread() == qApp->thread());
```

---

## 11. Include Order

1. Corresponding header
2. Blank line
3. C headers `<stdio.h>`
4. C++ standard library `<vector>`
5. Third-party `<obs.h>`, `<QDialog>`
6. Blank line
7. Project headers `"profile_manager.h"`

**Use quotes for project headers, angle brackets for system/third-party.**

```cpp
#include "system_detector.h"

#include <string>
#include <vector>

#include <obs.h>
#include <obs-frontend-api.h>

#include "common/logger.h"
#include "common/obs_wrappers.h"
```

---
