# Jitter Observatory test suite. No test declares a timeout: every test synchronises on
# completed work rather than on elapsed time.

add_library(jitter_test_support STATIC support/harness.cpp)
target_link_libraries(jitter_test_support PUBLIC JitterObservatory::jitter_observatory)
target_include_directories(jitter_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
jitter_configure_target(jitter_test_support)

function(jitter_add_test target)
    add_executable(${target} ${ARGN})
    target_link_libraries(${target} PRIVATE jitter_test_support)
    jitter_configure_target(${target})
    add_test(NAME ${target} COMMAND ${target})
endfunction()

jitter_add_test(test_core unit/test_core.cpp)
jitter_add_test(test_time_evidence unit/test_time_evidence.cpp)
jitter_add_test(test_metrics_window unit/test_metrics_window.cpp)
jitter_add_test(test_classification unit/test_classification.cpp)
jitter_add_test(test_baseline_attribution unit/test_baseline_attribution.cpp)
jitter_add_test(test_json_wire unit/test_json_wire.cpp)
jitter_add_test(test_persistence unit/test_persistence.cpp)
jitter_add_test(test_engine_ingest integration/test_engine_ingest.cpp)
jitter_add_test(test_generation_history integration/test_generation_history.cpp)
jitter_add_test(test_persistence_restart integration/test_persistence_restart.cpp)
jitter_add_test(test_properties property/test_determinism_invariants.cpp)
jitter_add_test(test_adversarial adversarial/test_adversarial.cpp)
jitter_add_test(test_concurrency concurrency/test_runtime_concurrency.cpp)
jitter_add_test(test_transport transport/test_transport_process.cpp)
jitter_add_test(test_cli_end_to_end e2e/test_cli_end_to_end.cpp)

# Independent downstream consumer proof: install the package into a staging prefix and
# configure, build and run a separate CMake project that only uses find_package.
add_test(NAME downstream_find_package
    COMMAND ${CMAKE_COMMAND}
        -DJITTER_SOURCE_DIR=${CMAKE_SOURCE_DIR}
        -DJITTER_BINARY_DIR=${CMAKE_BINARY_DIR}
        -DJITTER_CONFIG=$<CONFIG>
        -DJITTER_GENERATOR=${CMAKE_GENERATOR}
        -DJITTER_STAGE=${CMAKE_BINARY_DIR}/downstream-stage
        -P ${CMAKE_CURRENT_SOURCE_DIR}/downstream/run_downstream_check.cmake)

# The transport test needs to launch the probe process.
set_tests_properties(test_transport PROPERTIES
    ENVIRONMENT "JITTER_PROBE_EXECUTABLE=$<TARGET_FILE:jitter-ingest-probe>;JITTER_CTL_EXECUTABLE=$<TARGET_FILE:jitterctl>")
set_tests_properties(test_cli_end_to_end PROPERTIES
    ENVIRONMENT "JITTER_CTL_EXECUTABLE=$<TARGET_FILE:jitterctl>;JITTER_PROBE_EXECUTABLE=$<TARGET_FILE:jitter-ingest-probe>")
