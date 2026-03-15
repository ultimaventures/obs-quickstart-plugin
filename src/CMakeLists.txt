# Add each module directory
add_subdirectory(detection)
add_subdirectory(network)
add_subdirectory(profile)
add_subdirectory(settings)
add_subdirectory(sources)
add_subdirectory(filters)
add_subdirectory(monitoring)
add_subdirectory(ui)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    plugin-main.cpp
)

# Link dependencies (assuming nlohmann/json and cpp-httplib will be added as needed)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE
    plugin-support
)

# Placeholder for future dependency configuration:
# find_package(nlohmann_json REQUIRED)
# target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE nlohmann_json::nlohmann_json)
# find_package(cpp-httplib REQUIRED)
# target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE httplib::httplib)
