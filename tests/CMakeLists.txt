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

include(GoogleTest)

# Windows: the test executables are run at build time by gtest_discover_tests
# and later by ctest, but dependency DLLs are not on the loader PATH in CI,
# so the executable dies at startup with 0xc0000135 (STATUS_DLL_NOT_FOUND).
# $<TARGET_RUNTIME_DLLS> covers the direct deps (Qt, libobs,
# obs-frontend-api), but obs.dll itself is built from obs-studio sources and
# dynamically links the FFmpeg/support DLLs (avcodec-61.dll, ...) that ship
# in the prebuilt windows-deps package under .deps/bin -- not
# .deps/bin/64bit, where the built obs.dll lands. No CMake target models
# those DLLs, so glob both dirs and copy everything next to the executable;
# the set stays correct across dependency version bumps.
# Each POST_BUILD step must be registered before gtest_discover_tests adds
# its own, so the DLLs are in place when discovery runs the executable.
if(WIN32)
    file(
        GLOB _obs_test_dlls
        "${PROJECT_SOURCE_DIR}/.deps/bin/64bit/*.dll"
        "${PROJECT_SOURCE_DIR}/.deps/bin/*.dll"
    )
    add_custom_command(
        TARGET profile_tests
        POST_BUILD
        COMMAND
            ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:profile_tests> ${_obs_test_dlls}
            $<TARGET_FILE_DIR:profile_tests>
        COMMAND_EXPAND_LISTS
    )
endif()
gtest_discover_tests(profile_tests)

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

if(WIN32)
    # _obs_test_dlls is globbed once above (see the profile_tests block).
    add_custom_command(
        TARGET settings_tests
        POST_BUILD
        COMMAND
            ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:settings_tests> ${_obs_test_dlls}
            $<TARGET_FILE_DIR:settings_tests>
        COMMAND_EXPAND_LISTS
    )
endif()
gtest_discover_tests(settings_tests)

if(APPLE)
    # macOS: the test executables link the real libobs and Qt frameworks,
    # which live in .deps and are not on the loader path when
    # gtest_discover_tests runs the binaries at build time
    # (dyld: Library not loaded: @rpath/libobs.framework/...).
    # cmake/macos/xcode.cmake sets CMAKE_SKIP_RPATH for the Xcode generator,
    # so the BUILD_RPATH property is a no-op on CI; pass -rpath straight to
    # the linker instead, which bypasses CMake's rpath machinery entirely.
    file(
        GLOB _obs_test_qt_lib_dirs
        "${PROJECT_SOURCE_DIR}/.deps/obs-deps-qt6-*/lib"
    )
    set(_obs_test_rpaths
        "${PROJECT_SOURCE_DIR}/.deps/Frameworks"
        "${PROJECT_SOURCE_DIR}/.deps/lib"
        ${_obs_test_qt_lib_dirs}
    )
    set(_obs_test_rpath_link_options "")
    foreach(_obs_test_rpath IN LISTS _obs_test_rpaths)
        list(
            APPEND
            _obs_test_rpath_link_options
            "LINKER:-rpath,${_obs_test_rpath}"
        )
    endforeach()
    target_link_options(
        profile_tests
        settings_tests
        PRIVATE ${_obs_test_rpath_link_options}
    )
endif()
