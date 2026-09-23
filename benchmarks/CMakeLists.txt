# Jitter Observatory benchmarks. Every benchmark counts completed work items and
# verifies a checksum over its results so that the work cannot be optimised away.

add_executable(bench_observatory bench_observatory.cpp)
target_link_libraries(bench_observatory PRIVATE JitterObservatory::jitter_observatory)
jitter_configure_target(bench_observatory)

add_executable(bench_store bench_store.cpp)
target_link_libraries(bench_store PRIVATE JitterObservatory::jitter_observatory)
jitter_configure_target(bench_store)
