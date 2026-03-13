**GREAT CATCH. You DO need C++, and yes, you need to convert the template.**

**Conversion is mostly just renaming:**
```bash
mv src/plugin-main.c src/plugin-main.cpp
```

**Plus a few small code changes:**

### **Changes Required:**

#### **1. Extern "C" Linkage for OBS Entry Points**

OBS expects C-style function names. Wrap entry points:

```cpp
// plugin-main.cpp
#ifdef __cplusplus
extern "C" {
#endif

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-setup", "en-US")

bool obs_module_load(void) {
    blog(LOG_INFO, "Plugin loaded");
    return true;
}

void obs_module_unload(void) {
    blog(LOG_INFO, "Plugin unloaded");
}

#ifdef __cplusplus
}
#endif
```

**Why:** C++ mangles function names (`obs_module_load` becomes `_Z15obs_module_loadv`). `extern "C"` prevents this.

#### **2. Explicit Casts for void\***

C allows implicit `void*` conversions. C++ requires explicit casts.

```c
// C code - works fine
void* data = malloc(100);
char* str = data;  // Implicit conversion
```

```cpp
// C++ - requires explicit cast
void* data = malloc(100);
char* str = static_cast<char*>(data);  // Explicit cast required
```

**In OBS context:**
```cpp
// C version
obs_data_t* settings = obs_source_get_settings(source);

// C++ version (same - obs_data_t* is not void*)
obs_data_t* settings = obs_source_get_settings(source);
// No change needed here
```

Most OBS functions return typed pointers, so this rarely matters.

#### **3. Update CMakeLists.txt**

Tell CMake you're using C++:

```cmake
# CMakeLists.txt
project(obs-setup VERSION 1.0.0 LANGUAGES CXX)  # Changed from C to CXX

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(obs-setup MODULE)

target_sources(obs-setup PRIVATE
    src/plugin-main.cpp  # Changed from .c to .cpp
)
```

---

## **Step-by-Step Conversion Process**

### **1. Clone Template and Prepare**
```bash
git clone https://github.com/obsproject/obs-plugintemplate.git obs-setup
cd obs-setup
rm -rf .git
git init
```

### **2. Rename Source Files**
```bash
mv src/plugin-main.c src/plugin-main.cpp
# If there are other .c files in the template, rename those too
```

### **3. Update CMakeLists.txt**

**Find this line:**
```cmake
project(obs-plugintemplate VERSION 1.0.0)
```

**Change to:**
```cmake
project(obs-setup VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
```

**Find this line:**
```cmake
target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/plugin-main.c)
```

**Change to:**
```cmake
target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/plugin-main.cpp)
```

### **4. Add extern "C" to plugin-main.cpp**

**Wrap the OBS entry points:**
```cpp
#include <obs-module.h>

#ifdef __cplusplus
extern "C" {
#endif

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-setup", "en-US")

bool obs_module_load(void) {
    blog(LOG_INFO, "[OBS Setup] Plugin loaded");
    return true;
}

void obs_module_unload(void) {
    blog(LOG_INFO, "[OBS Setup] Plugin unloaded");
}

const char* obs_module_name(void) {
    return "OBS Setup Plugin";
}

const char* obs_module_description(void) {
    return "Automated stream setup for beginners";
}

#ifdef __cplusplus
}
#endif
```

### **5. Test Build**
```bash
cmake -B build
cmake --build build
```

**Should compile without errors.**

### **6. Test in OBS**
```bash
# Copy plugin to OBS plugins folder
# Windows: %AppData%/obs-studio/plugins/
# macOS: ~/Library/Application Support/obs-studio/plugins/
# Linux: ~/.config/obs-studio/plugins/

# Launch OBS, check log for "Plugin loaded"
```

---

## **Common Issues During Conversion**

### **Issue 1: Missing C++ Headers**

If you get errors about `std::string` or `std::vector`:

```cpp
// Add to top of plugin-main.cpp
#include <string>
#include <vector>
```

### **Issue 2: Qt Headers Not Found**

When you add Qt code later:

```cmake
# CMakeLists.txt
find_package(Qt6 COMPONENTS Widgets REQUIRED)

target_link_libraries(obs-setup PRIVATE
    OBS::libobs
    Qt6::Widgets
)
```

### **Issue 3: Name Mangling Errors**

If you see linker errors like:
```
undefined reference to `obs_module_load'
```

**You forgot `extern "C"` around the entry points.**

---

## **DOCUMENTATION UPDATES**

### **Add to STACK.md:**

```markdown
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
```

### **Add to CONVENTIONS.md - Section 11 (Include Order):**

```markdown
## 11. Include Order

1. Corresponding header
2. Blank line
3. **C++ standard library** (`<string>`, `<vector>`, `<memory>`)
4. **Qt headers** (`<QDialog>`, `<QWidget>`)
5. **OBS headers** (`<obs.h>`, `<obs-frontend-api.h>`)
6. Blank line
7. Project headers (`"profile_manager.h"`)

**Example:**
```cpp
#include "system_detector.h"

#include <string>
#include <vector>

#include <QDialog>
#include <QMessageBox>

#include <obs.h>
#include <obs-frontend-api.h>

#include "common/logger.h"
#include "common/obs_wrappers.h"
```

**Note:** OBS headers are C-compatible and work in C++ without modification.
```

---

## **YOUR CHECKLIST BEFORE STARTING DEVELOPMENT**

- [ ] Clone obs-plugintemplate
- [ ] Rename `src/plugin-main.c` → `src/plugin-main.cpp`
- [ ] Update `CMakeLists.txt`: Set language to CXX, set C++17 standard
- [ ] Add `extern "C"` wrapper around OBS entry points
- [ ] Test build: `cmake -B build && cmake --build build`
- [ ] Load in OBS, verify "Plugin loaded" in log
- [ ] Create module directories: `/src/detection`, `/src/network`, etc.
- [ ] Update STACK.md with C++ justification
- [ ] Update CONVENTIONS.md include order with C++ headers

---