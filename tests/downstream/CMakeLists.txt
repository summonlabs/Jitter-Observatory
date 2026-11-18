cmake_minimum_required(VERSION 3.24)

project(JitterDownstreamConsumer
    VERSION 1.0.0
    DESCRIPTION "Independent consumer that proves the installed Jitter Observatory package"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(JitterObservatory 1.0 CONFIG REQUIRED)

add_executable(downstream_consumer main.cpp)
target_link_libraries(downstream_consumer PRIVATE JitterObservatory::jitter_observatory)

# The exported target carries no sanitizer options, so a consumer of an
# instrumented package instruments itself: MSVC refuses to link plain objects
# with instrumented ones (LNK2038).
option(JITTER_CONSUMER_ASAN "Build the consumer with AddressSanitizer" OFF)
if(JITTER_CONSUMER_ASAN)
    if(MSVC)
        target_compile_options(downstream_consumer PRIVATE /fsanitize=address /Zi)
        target_link_options(downstream_consumer PRIVATE /INCREMENTAL:NO /DEBUG)
    else()
        target_compile_options(downstream_consumer PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer -g)
        target_link_options(downstream_consumer PRIVATE -fsanitize=address,undefined)
    endif()
endif()

# The consumer must not silently inherit this project's warning policy.
get_target_property(inherited_options JitterObservatory::jitter_observatory INTERFACE_COMPILE_OPTIONS)
if(inherited_options)
    message(FATAL_ERROR "the exported target leaks compile options: ${inherited_options}")
endif()

message(STATUS "downstream consumer configured against JitterObservatory ${JitterObservatory_VERSION}")
