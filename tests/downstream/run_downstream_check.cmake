# Installs the built package into a staging prefix and then configures, builds and runs
# an independent consumer project against it.

foreach(required JITTER_SOURCE_DIR JITTER_BINARY_DIR JITTER_STAGE)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} must be defined")
    endif()
endforeach()

set(stage "${JITTER_STAGE}")
file(REMOVE_RECURSE "${stage}")
file(MAKE_DIRECTORY "${stage}")

set(install_prefix "${stage}/prefix")
set(consumer_build "${stage}/consumer-build")

message(STATUS "installing into ${install_prefix}")
execute_process(
    COMMAND ${CMAKE_COMMAND} --install "${JITTER_BINARY_DIR}" --prefix "${install_prefix}"
            --config "${JITTER_CONFIG}"
    RESULT_VARIABLE install_result
    OUTPUT_VARIABLE install_output
    ERROR_VARIABLE install_error)
if(NOT install_result EQUAL 0)
    message(FATAL_ERROR "install failed (${install_result})\n${install_output}\n${install_error}")
endif()

set(config_file "${install_prefix}/lib/cmake/JitterObservatory/JitterObservatoryConfig.cmake")
if(NOT EXISTS "${config_file}")
    # Some platforms use lib64; probe both before failing.
    set(config_file "${install_prefix}/lib64/cmake/JitterObservatory/JitterObservatoryConfig.cmake")
endif()
if(NOT EXISTS "${config_file}")
    message(FATAL_ERROR "installed package config was not found under ${install_prefix}")
endif()
message(STATUS "package config: ${config_file}")

execute_process(
    COMMAND ${CMAKE_COMMAND}
            -S "${JITTER_SOURCE_DIR}/tests/downstream"
            -B "${consumer_build}"
            -G "${JITTER_GENERATOR}"
            -DCMAKE_BUILD_TYPE=${JITTER_CONFIG}
            -DCMAKE_PREFIX_PATH=${install_prefix}
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_output
    ERROR_VARIABLE configure_error)
if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR "downstream configure failed\n${configure_output}\n${configure_error}")
endif()
message(STATUS "${configure_output}")

execute_process(
    COMMAND ${CMAKE_COMMAND} --build "${consumer_build}" --config "${JITTER_CONFIG}"
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_error)
if(NOT build_result EQUAL 0)
    message(FATAL_ERROR "downstream build failed\n${build_output}\n${build_error}")
endif()

find_program(CONSUMER_EXECUTABLE
    NAMES downstream_consumer downstream_consumer.exe
    PATHS "${consumer_build}"
    NO_DEFAULT_PATH)
if(NOT CONSUMER_EXECUTABLE)
    file(GLOB_RECURSE candidates "${consumer_build}/*downstream_consumer*")
    foreach(candidate ${candidates})
        if(NOT IS_DIRECTORY "${candidate}")
            set(CONSUMER_EXECUTABLE "${candidate}")
            break()
        endif()
    endforeach()
endif()
if(NOT CONSUMER_EXECUTABLE)
    message(FATAL_ERROR "downstream consumer executable was not produced")
endif()

execute_process(
    COMMAND "${CONSUMER_EXECUTABLE}"
    RESULT_VARIABLE run_result
    OUTPUT_VARIABLE run_output
    ERROR_VARIABLE run_error)
message(STATUS "consumer output: ${run_output}")
if(NOT run_result EQUAL 0)
    message(FATAL_ERROR "downstream consumer failed (${run_result})\n${run_output}\n${run_error}")
endif()

message(STATUS "downstream find_package validation succeeded")
