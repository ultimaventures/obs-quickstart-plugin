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
# and later by ctest, but dependency DLLs (Qt, libobs, obs-frontend-api) are
# not on the loader PATH in CI, so the executable dies with 0xc0000135
# (STATUS_DLL_NOT_FOUND). Copy the runtime DLLs next to the executable.
# This POST_BUILD step must be registered before gtest_discover_tests adds
# its own, so the DLLs are in place when discovery runs the executable.
if(WIN32)
    add_custom_command(
        TARGET profile_tests
        POST_BUILD
        COMMAND
            ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:profile_tests>
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
    add_custom_command(
        TARGET settings_tests
        POST_BUILD
        COMMAND
            ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:settings_tests>
            $<TARGET_FILE_DIR:settings_tests>
        COMMAND_EXPAND_LISTS
    )
endif()
gtest_discover_tests(settings_tests)
