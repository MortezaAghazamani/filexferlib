cmake_minimum_required(VERSION 3.20)
project(filexferlib VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# =====================================================================
# Options
# =====================================================================

option(FILEXFERLIB_BUILD_TESTS     "Build tests"                           ON)
option(FILEXFERLIB_BUILD_EXAMPLES  "Build examples"                        ON)
option(FILEXFERLIB_BUILD_BENCH     "Build benchmark"                       ON)
option(FILEXFERLIB_DISABLE_LOGGING "Disable all logging (zero cost)"       OFF)

# =====================================================================
# Compiler flags
# =====================================================================

if(MSVC)
    add_compile_options(/W4 /permissive- /Zc:__cplusplus /utf-8)
    add_compile_definitions(_WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# =====================================================================
# Dependencies
# =====================================================================

find_package(asio CONFIG REQUIRED)
find_package(xxHash CONFIG REQUIRED)
find_package(zstd CONFIG REQUIRED)

# =====================================================================
# Core library
# =====================================================================

add_library(filexferlib
    src/error.cpp
    src/client.cpp
    src/server.cpp
    src/transfer.cpp
    src/sync.cpp
    src/detail/frame.cpp
    src/detail/session.cpp
    src/detail/hash.cpp
    src/detail/compression.cpp
)

target_include_directories(filexferlib
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(filexferlib
    PUBLIC
        asio::asio
        xxHash::xxhash
        zstd::libzstd
)

# ---- Zero-cost logging toggle ----
if(FILEXFERLIB_DISABLE_LOGGING)
    target_compile_definitions(filexferlib PUBLIC FILEXFERLIB_DISABLE_LOGGING)
    message(STATUS "filexferlib: logging DISABLED (zero-cost)")
else()
    message(STATUS "filexferlib: logging enabled")
endif()

# =====================================================================
# Examples
# =====================================================================

if(FILEXFERLIB_BUILD_EXAMPLES)
    add_executable(example_server examples/example_server.cpp)
    target_link_libraries(example_server PRIVATE filexferlib)

    add_executable(example_client examples/example_client.cpp)
    target_link_libraries(example_client PRIVATE filexferlib)

    add_executable(example_backup examples/example_backup.cpp)
    target_link_libraries(example_backup PRIVATE filexferlib)

    add_executable(example_mirror examples/example_mirror.cpp)
    target_link_libraries(example_mirror PRIVATE filexferlib)
endif()

# =====================================================================
# Tests
# =====================================================================

if(FILEXFERLIB_BUILD_TESTS)
    enable_testing()

    add_executable(test_protocol tests/test_protocol.cpp)
    target_link_libraries(test_protocol PRIVATE filexferlib)
    add_test(NAME test_protocol COMMAND test_protocol)

    add_executable(test_units tests/test_units.cpp)
    target_link_libraries(test_units PRIVATE filexferlib)
    add_test(NAME test_units COMMAND test_units)

    add_executable(test_edge tests/test_edge.cpp)
    target_link_libraries(test_edge PRIVATE filexferlib)
    add_test(NAME test_edge COMMAND test_edge)

    add_executable(test_integration tests/test_integration.cpp)
    target_link_libraries(test_integration PRIVATE filexferlib)
    add_test(NAME test_integration COMMAND test_integration)
    set_tests_properties(test_integration PROPERTIES TIMEOUT 300)

    add_executable(test_stress tests/test_stress.cpp)
    target_link_libraries(test_stress PRIVATE filexferlib)
    add_test(NAME test_stress COMMAND test_stress)
    set_tests_properties(test_stress PROPERTIES TIMEOUT 600)

    add_executable(test_try tests/test_try.cpp)
    target_link_libraries(test_try PRIVATE filexferlib)
    add_test(NAME test_try COMMAND test_try)
endif()

# =====================================================================
# Benchmark
# =====================================================================

if(FILEXFERLIB_BUILD_BENCH)
    add_executable(bench_transfer bench/bench_transfer.cpp)
    target_link_libraries(bench_transfer PRIVATE filexferlib)
endif()

# =====================================================================
# Install
# =====================================================================

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ---- Install targets ----
install(TARGETS filexferlib
    EXPORT filexferlibTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# ---- Install headers ----
install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# ---- Export CMake targets ----
install(EXPORT filexferlibTargets
    FILE filexferlibTargets.cmake
    NAMESPACE filexferlib::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/filexferlib
)

# ---- Generate and install package config ----
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/filexferlibConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/filexferlibConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/filexferlib"
)

# ---- Generate and install package version ----
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/filexferlibConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/filexferlibConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/filexferlibConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/filexferlib"
)
