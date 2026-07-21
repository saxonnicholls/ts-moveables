# TSMoveables - Thread Safe Moveables
#
# Header-only. This produces the interface target snicholls::ts_moveables,
# builds the unit tests and demo when configured as the top-level project,
# and installs a find_package(ts_moveables) config.

cmake_minimum_required(VERSION 3.16)

project(ts_moveables
    VERSION 1.0.0
    DESCRIPTION "Thread Safe Moveable Objects - moveable wrappers for the immovable C++ synchronisation primitives"
    HOMEPAGE_URL "https://github.com/saxonnicholls/ts-moveables"
    LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# The library
add_library(ts_moveables INTERFACE)
add_library(snicholls::ts_moveables ALIAS ts_moveables)

target_compile_features(ts_moveables INTERFACE cxx_std_17)

# Headers are included as "ts_moveables.hpp" / "moveable_mutex.hpp" both from
# the source tree and from the installed include/ts_moveables directory
target_include_directories(ts_moveables INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/TSMoveables>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/ts_moveables>)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
target_link_libraries(ts_moveables INTERFACE Threads::Threads)

# Some targets (notably Clang on AArch64 Linux) lower std::atomic operations
# such as is_lock_free() to libatomic calls instead of inlining them
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(ts_moveables INTERFACE atomic)
endif()

# Tests and demo - on by default only when this is the top-level project
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(TS_MOVEABLES_TOP_LEVEL ON)
else()
    set(TS_MOVEABLES_TOP_LEVEL OFF)
endif()

option(TS_MOVEABLES_BUILD_TESTS "Build the ts-moveables unit tests and demo" ${TS_MOVEABLES_TOP_LEVEL})

if(TS_MOVEABLES_BUILD_TESTS)
    enable_testing()

    add_executable(ts_moveables_tests tests/tests.cpp)
    target_link_libraries(ts_moveables_tests PRIVATE snicholls::ts_moveables)
    add_test(NAME unit_tests COMMAND ts_moveables_tests)

    add_executable(ts_moveables_demo TSMoveables/main.cpp)
    target_link_libraries(ts_moveables_demo PRIVATE snicholls::ts_moveables)
endif()

# Install and export
file(GLOB TS_MOVEABLES_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/TSMoveables/*.hpp")

install(FILES ${TS_MOVEABLES_HEADERS}
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ts_moveables)

install(TARGETS ts_moveables EXPORT ts_moveablesTargets)

install(EXPORT ts_moveablesTargets
    NAMESPACE snicholls::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ts_moveables)

configure_package_config_file(
    cmake/ts_moveablesConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/ts_moveablesConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ts_moveables)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/ts_moveablesConfigVersion.cmake
    COMPATIBILITY SameMajorVersion
    ARCH_INDEPENDENT)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/ts_moveablesConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/ts_moveablesConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ts_moveables)
