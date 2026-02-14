# OBS Plugin Code Conventions

This document defines coding, file, and documentation conventions for the OBS Setup Plugin to ensure consistent style and maintainability across the C++ codebase.

---

## 1. Naming Conventions

**Classes:**

* PascalCase
* Example: `SystemDetector`, `ProfileManager`

**Methods / Functions:**

* camelCase
* Example: `detectEncoders()`, `runLocalRecordingTest()`

**Variables / Members:**

* camelCase
* Example: `cpuUsage`, `sceneCollection`
* Member variables may use `m_` prefix: `m_profileName`

**Constants:**

* ALL_CAPS with underscores
* Example: `MAX_CPU_THRESHOLD`

**Namespaces:**

* lowercase with underscores
* Example: `obs_setup`

---

## 2. File Naming

* Source files: snake_case.cpp

  * Example: `system_detector.cpp`
* Header files: snake_case.h

  * Example: `profile_manager.h`
* Private implementation files (if using PIMPL): `*_impl.h`

---

## 3. Header Guard Style

* Use `PROJECT_MODULE_FILENAME_H` pattern, all uppercase
* Example:

```cpp
#ifndef OBS_SETUP_PROFILE_MANAGER_H
#define OBS_SETUP_PROFILE_MANAGER_H

// header content

#endif // OBS_SETUP_PROFILE_MANAGER_H
```

---

## 4. Comment Style

* Use Doxygen for all classes, functions, and modules
* Example:

```cpp
/**
 * Detects available OBS encoders.
 * @return Vector of EncoderInfo structs for each detected encoder.
 */
std::vector<EncoderInfo> detectEncoders();
```

* Inline comments: `// Brief explanation` only when needed

---

## 5. Git Commit Message Format

* Use **Conventional Commits** style:

```
<type>(<scope>): <short summary>

<body - optional detailed description>
```

* Types: `feat`, `fix`, `docs`, `refactor`, `test`, `chore`
* Example:

```
feat(profile): add new profile creation API
```

---

## 6. Code Formatting (clang-format)

* Base style: Google C++
* Indent width: 4
* Use `clang-format` version 15 or later
* Example `.clang-format` snippet:

```yaml
BasedOnStyle: Google
IndentWidth: 4
ColumnLimit: 100
AllowShortFunctionsOnASingleLine: InlineOnly
PointerAlignment: Left
```

* Run `clang-format -i <file>` before commit

---

## 7. Error Handling Patterns

* Use return codes or `bool` for non-critical functions
* Throw exceptions only for critical failures
* Always log errors before returning
* Rollback any partial changes on failure
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

* Use OBS `blog()` function for all logs
* Format: `[MODULE] Level: Message`
* Levels: `LOG_INFO`, `LOG_WARNING`, `LOG_ERROR`
* Example:

```cpp
blog(LOG_INFO, "[Network] Upload speed measured: %.2f Mbps", speedMbps);
blog(LOG_ERROR, "[Profile] Failed to backup existing profile");
```

* Include module name in square brackets for clarity

---

**All code must follow these conventions to ensure readability, maintainability, and smooth integration with the OBS C++ plugin ecosystem.**
