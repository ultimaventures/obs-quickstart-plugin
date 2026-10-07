# Unit tests for the plugin's pure-logic modules.
#
# The test binary compiles the real module TU and links the real Qt, libobs
# and obs-frontend-api, but there is no OBS runtime in the build step, so
# the OBS C functions touched by the tested paths are stubbed in the test TU
# (see profile_tests.cpp). No source refactor of the module is needed.

include(FetchContent)

FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
)
# For Windows: prevent gtest from overriding the parent project's CRT choice.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
# Don't install googletest: CI builds only the plugin target on macOS, so the
# gtest libs are never built there and `cmake --install` would fail looking
# for them. Tests still build and run via ctest on every platform.
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(INSTALL_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(
    profile_tests
    ${CMAKE_CURRENT_LIST_DIR}/profile_tests.cpp
    ${PROJECT_SOURCE_DIR}/src/profile/profile-module.cpp
)
target_include_directories(
    profile_tests
    PRIVATE ${PROJECT_SOURCE_DIR}/src/profile
)
target_link_libraries(
    profile_tests
    PRIVATE
        OBS::libobs
        OBS::obs-frontend-api
        Qt6::Core
        Qt6::Widgets
        GTest::gtest_main
)

add_executable(
    settings_tests
    ${CMAKE_CURRENT_LIST_DIR}/settings_tests.cpp
    ${PROJECT_SOURCE_DIR}/src/settings/settings-module.cpp
)
target_include_directories(
    settings_tests
    PRIVATE ${PROJECT_SOURCE_DIR}/src/settings
)
target_link_libraries(
    settings_tests
    PRIVATE
        OBS::libobs
        OBS::obs-frontend-api
        Qt6::Core
        Qt6::Widgets
        GTest::gtest_main
)

include(GoogleTest)

if(WIN32)
    # Windows: the test executables are run by ctest, but dependency DLLs
    # are not on the loader PATH in CI, so the executable dies at startup
    # with 0xc0000135 (STATUS_DLL_NOT_FOUND). $<TARGET_RUNTIME_DLLS>
    # covers the direct deps (Qt, libobs, obs-frontend-api); the remaining
    # DLLs (libcurl, w32-pthreads, ...) ship in the prebuilt windows-deps
    # package and the obs-studio Development install. No CMake target models
    # those DLLs, so glob their directories; the set stays correct across
    # dependency version bumps.
    #
    # Staging happens exactly once in a dedicated target that both test
    # targets depend on: two parallel POST_BUILD copies racing on the same
    # destination directory intermittently left a truncated DLL behind,
    # which the loader reported as 0xc0000135 for the second test binary.
    file(
        GLOB _obs_test_dll_dirs
        "${PROJECT_SOURCE_DIR}/.deps/bin/64bit"
        "${PROJECT_SOURCE_DIR}/.deps/obs-deps-*/bin"
        "${PROJECT_SOURCE_DIR}/.deps/obs-deps-*/bin/64bit"
    )
    set(_obs_test_dlls "")
    foreach(_obs_test_dll_dir IN LISTS _obs_test_dll_dirs)
        file(GLOB _obs_test_dir_dlls "${_obs_test_dll_dir}/*.dll")
        list(APPEND _obs_test_dlls ${_obs_test_dir_dlls})
    endforeach()
    list(REMOVE_DUPLICATES _obs_test_dlls)
    add_custom_target(
        stage_test_runtime_dlls
        ALL
        COMMAND
            ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:profile_tests>"
        COMMAND
            ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:profile_tests>
            $<TARGET_RUNTIME_DLLS:settings_tests> ${_obs_test_dlls}
            "$<TARGET_FILE_DIR:profile_tests>"
        COMMAND_EXPAND_LISTS
        COMMENT "Staging runtime DLLs for unit tests"
        VERBATIM
    )
    add_dependencies(profile_tests stage_test_runtime_dlls)
    add_dependencies(settings_tests stage_test_runtime_dlls)
endif()

# Discover tests at ctest time, not at build time: running the test
# executables during the build races with parallel build activity
# (DLL staging, antivirus scans), which produced sporadic loader failures.
# By ctest time the build -- and the staging above -- is fully complete.
gtest_discover_tests(profile_tests DISCOVERY_MODE PRE_TEST)
gtest_discover_tests(settings_tests DISCOVERY_MODE PRE_TEST)

if(APPLE)
    # macOS: the test executables link the real libobs and Qt frameworks,
    # which live in .deps and are not on the loader path when the test
    # binaries run (dyld: Library not loaded: @rpath/libavcodec.dylib --
    # libobs itself links the FFmpeg dylibs shipped in the prebuilt
    # macos-deps package under .deps/obs-deps-*/lib, which the earlier
    # rpath list missed).
    # cmake/macos/xcode.cmake sets CMAKE_SKIP_RPATH for the Xcode generator,
    # so the BUILD_RPATH property is a no-op on CI; pass -rpath straight to
    # the linker instead, which bypasses CMake's rpath machinery entirely.
    file(
        GLOB _obs_test_qt_lib_dirs
        "${PROJECT_SOURCE_DIR}/.deps/obs-deps-qt6-*/lib"
    )
    file(
        GLOB _obs_test_prebuilt_lib_dirs
        "${PROJECT_SOURCE_DIR}/.deps/obs-deps-*/lib"
    )
    set(_obs_test_rpaths
        "${PROJECT_SOURCE_DIR}/.deps/Frameworks"
        "${PROJECT_SOURCE_DIR}/.deps/lib"
        ${_obs_test_qt_lib_dirs}
        ${_obs_test_prebuilt_lib_dirs}
    )
    list(REMOVE_DUPLICATES _obs_test_rpaths)
    set(_obs_test_rpath_link_options "")
    foreach(_obs_test_rpath IN LISTS _obs_test_rpaths)
        list(
            APPEND
            _obs_test_rpath_link_options
            "LINKER:-rpath,${_obs_test_rpath}"
        )
    endforeach()
    target_link_options(profile_tests PRIVATE ${_obs_test_rpath_link_options})
    target_link_options(settings_tests PRIVATE ${_obs_test_rpath_link_options})
endif()
