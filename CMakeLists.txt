cmake_minimum_required(VERSION 3.24)

project(JitterObservatory
    VERSION 1.0.0
    DESCRIPTION "Vendor-neutral latency-variance and path-timing-instability observation runtime"
    HOMEPAGE_URL "https://github.com/summonlabs/Jitter-Observatory"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Global language configuration
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(JITTER_BUILD_APPS "Build the jitterctl and ingest probe applications" ON)
option(JITTER_BUILD_TESTS "Build the Jitter Observatory test suite" ON)
option(JITTER_BUILD_EXAMPLES "Build the example programs" ON)
option(JITTER_BUILD_BENCHMARKS "Build the benchmark programs" ON)
option(JITTER_WARNINGS_AS_ERRORS "Treat first-party compiler warnings as errors" ON)
option(JITTER_ENABLE_ASAN "Enable AddressSanitizer (and UBSan where the toolchain provides it)" OFF)
option(JITTER_ENABLE_LOCK_AUDIT "Enable the runtime lock-order and reentrancy auditor" ON)

if(JITTER_ENABLE_LOCK_AUDIT AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    message(STATUS "Lock audit requested for build type '${CMAKE_BUILD_TYPE}': the auditor stays enabled")
endif()

# ---------------------------------------------------------------------------
# Shared compile configuration applied directly to first-party targets.
# The flags are PRIVATE so that installing the package never imposes this
# project's warning policy or sanitizer choice on a downstream consumer.
# ---------------------------------------------------------------------------
function(jitter_configure_target target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4
            /permissive-
            /Zc:__cplusplus
            /Zc:preprocessor
            /utf-8
            /EHsc
            /MP)
        if(JITTER_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wshadow
            -Wconversion
            -Wsign-conversion
            -Wold-style-cast
            -Wcast-qual
            -Wnon-virtual-dtor
            -Woverloaded-virtual
            -Wdouble-promotion
            -Wformat=2)
        if(JITTER_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()

    if(JITTER_ENABLE_ASAN)
        if(MSVC)
            target_compile_options(${target} PRIVATE /fsanitize=address)
            target_link_options(${target} PRIVATE /INCREMENTAL:NO)
        else()
            target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -g)
            target_link_options(${target} PRIVATE -fsanitize=address,undefined)
        endif()
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------
file(GLOB_RECURSE JITTER_PUBLIC_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/include/jitter/*.hpp")
file(GLOB_RECURSE JITTER_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

if(NOT JITTER_SOURCES)
    message(FATAL_ERROR "no library sources found under ${CMAKE_CURRENT_SOURCE_DIR}/src")
endif()

add_library(jitter_observatory STATIC ${JITTER_SOURCES} ${JITTER_PUBLIC_HEADERS})
add_library(JitterObservatory::jitter_observatory ALIAS jitter_observatory)

target_include_directories(jitter_observatory
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(jitter_observatory PUBLIC cxx_std_20)
target_compile_definitions(jitter_observatory PRIVATE JITTER_BUILDING_LIBRARY=1)
target_compile_definitions(jitter_observatory PUBLIC JITTER_LOCK_AUDIT=$<BOOL:${JITTER_ENABLE_LOCK_AUDIT}>)

find_package(Threads REQUIRED)
target_link_libraries(jitter_observatory PUBLIC Threads::Threads)

if(WIN32)
    target_link_libraries(jitter_observatory PUBLIC ws2_32)
endif()

jitter_configure_target(jitter_observatory)

set_target_properties(jitter_observatory PROPERTIES
    OUTPUT_NAME jitter_observatory
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})

set(JITTER_TOOL_TARGETS "" CACHE INTERNAL "Command line targets installed alongside the library")

if(JITTER_BUILD_APPS)
    add_subdirectory(apps)
endif()

if(JITTER_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(JITTER_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

if(JITTER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Installation and packaging
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS jitter_observatory ${JITTER_TOOL_TARGETS}
    EXPORT JitterObservatoryTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT JitterObservatoryTargets
    FILE JitterObservatoryTargets.cmake
    NAMESPACE JitterObservatory::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/JitterObservatory)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/JitterObservatoryConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/JitterObservatoryConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/JitterObservatory
    PATH_VARS CMAKE_INSTALL_INCLUDEDIR)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/JitterObservatoryConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
    ARCH_INDEPENDENT)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/JitterObservatoryConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/JitterObservatoryConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/JitterObservatory)

message(STATUS "Jitter Observatory ${PROJECT_VERSION} | build type: ${CMAKE_BUILD_TYPE} | tests: ${JITTER_BUILD_TESTS} | apps: ${JITTER_BUILD_APPS} | asan: ${JITTER_ENABLE_ASAN} | lock audit: ${JITTER_ENABLE_LOCK_AUDIT}")
