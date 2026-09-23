# Jitter Observatory command line applications.

add_executable(jitterctl jitterctl/main.cpp)
target_link_libraries(jitterctl PRIVATE JitterObservatory::jitter_observatory)
jitter_configure_target(jitterctl)
set_target_properties(jitterctl PROPERTIES OUTPUT_NAME jitterctl)

add_executable(jitter-ingest-probe jitter-ingest-probe/main.cpp)
target_link_libraries(jitter-ingest-probe PRIVATE JitterObservatory::jitter_observatory)
jitter_configure_target(jitter-ingest-probe)
set_target_properties(jitter-ingest-probe PROPERTIES OUTPUT_NAME jitter-ingest-probe)

set(JITTER_TOOL_TARGETS jitterctl jitter-ingest-probe PARENT_SCOPE)
