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
gtest_discover_tests(profile_tests)
