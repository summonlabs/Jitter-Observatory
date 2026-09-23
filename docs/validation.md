# Validation

Everything below was executed on the development machine for the v1.0.0 release:
Windows, MSVC 19.44 (Visual Studio 2022 Build Tools), CMake 4.3, Ninja, 16 hardware
threads. The commands are reproducible as written from a Visual Studio developer
environment.

## Builds

| Configuration | Command | Result |
| --- | --- | --- |
| Debug | `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build` | 0 errors, 0 warnings (`/W4 /WX`) |
| Release | `cmake -S . -B build-rel -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build-rel` | 0 errors, 0 warnings (`/W4 /WX`) |
| Debug + AddressSanitizer | `-DJITTER_ENABLE_ASAN=ON` | 0 errors, 0 warnings |

First-party warning count is zero in every configuration; the compiler treats any warning
as an error.

## Test suite

`ctest --output-on-failure` from the build directory, with a compiler on `PATH` (the
downstream package proof configures and builds a separate project).

| Test | What it proves |
| --- | --- |
| `test_core` | SHA-256 and CRC-32C against published vectors, identity derivation and distinctness, canonical encoding round-trips and truncation, checked arithmetic, metric table integrity |
| `test_time_evidence` | comparability rules, declared equivalences, expiry, unit convertibility, the two age measurements, every evidence state, dominance rules, conflict resolution and retention bounds |
| `test_metrics_window` | exact metric values, exact `stddev == sqrt(variance)` and `cv == stddev/mean` relations, translation invariance, EWMA recurrence, insufficiency, and count/time/tumbling window semantics with a hard capacity bound |
| `test_classification` | no policy means no assertion; missing, stale, expired, conflicting and insufficient evidence never produce a positive level; thresholds, `require_all_rules`, determinism, policy validation |
| `test_baseline_attribution` | comparison deltas and ratios, refusal across generations and window policy changes, no delta from non fresh evidence; per hop attribution with absent timing, incomparable clocks, insufficient arrivals and mixed domains |
| `test_json_wire` | canonical JSON ordering, escaping, number rendering, document envelopes; frame round-trips, incomplete frames asking for more data, malformed magic/version/type/flags/checksum rejection; batch codec round-trip and tamper detection |
| `test_persistence` | header and record integrity, chain links, version refusal, payload corruption, damaged tail handling with and without authority, mid file corruption refusal, trailer replacement, record bounds |
| `test_engine_ingest` | registration consistency, ingest outcomes, idempotent duplicates, rewinds, epoch and incarnation fencing, historical ingest not advancing the guard, authority and origin spoofing refusal, closed generations, evidence classes, byte stable export |
| `test_generation_history` | unchanged topology does not open a generation, a revision change does not segment history, a route change does close episodes and segment history, comparisons refuse across generations, attribution follows the current generation, import ordering, retention bounds |
| `test_persistence_restart` | persisted evidence never silently becomes fresh, explicit admission, guard rebuilding so replays stay fenced, baselines/episodes/conflicts surviving a restart, damaged store refusal and authorised recovery, re-save after restore |
| `test_properties` | seeded randomised workloads: arrival order inside a batch and batch size never change the measurement content; window bounds hold; classifications never contradict their evidence; store round-trips random payloads; metric identities are stable |
| `test_adversarial` | thousands of random byte strings through the frame decoder and the store loader, every mutation of a valid store, identity drift, observation bounds, mixed identity batches, spoofed authority/origin/time, replays and contradictions, extreme timestamps, engine bounds |
| `test_concurrency` | four submitting threads, backpressure reporting and counting, drop-oldest policy, real cancellation, shutdown discarding queued work, concurrent readers and writers, and a violation free lock audit |
| `test_transport` | a real TCP connection from an independent process, handshake rejection for an unregistered source, malformed first messages, checksummed framing, and identical summaries between the network path and the in-process path |
| `test_cli_end_to_end` | the tool as a child process: version, metric catalog, demo store, integrity verification, conservative versus admitted summaries, explanations, history, attribution, baseline capture and comparison, damaged store detection, and a real serve session |
| `downstream_find_package` | the installed package is consumed by an independent CMake project that only uses `find_package`, and that project runs |

Results: **16 of 16 tests pass** in Debug and in Release, and 15 of 15 (the downstream
package proof is run in the other two configurations) pass under AddressSanitizer.

No test declares a timeout, no test sleeps, and no assertion depends on elapsed time. Every
synchronisation is on completed work: the runtime's `drain()` waits for an empty queue and
zero in-flight jobs, the transport test waits for the child process to exit, and the server
tests block on the client's own disconnect.

## Repeated runs

The full suite was run five times and the concurrency suite ten times in a row with no
failures, to check that nothing depends on scheduling luck.

## Defects found and fixed during hardening

The first passing state was reached, and then deliberately attacked. The material defects
that the attacks found, and that are fixed in this release:

1. `register_*` normalized a *copy* of a descriptor, so a caller kept a nil identity and
   every later reference failed. Registration now writes the derived identity back.
2. The scenario builder never registered a series, so batches were silently skipped. This
   was the reason a whole run could "succeed" while ingesting nothing; the generator now
   validates a plan before producing anything and refuses an incomplete one.
3. Frame and store headers wrote their magic as raw bytes but read it as a length prefixed
   string, so decoding always failed.
4. The store header declared 64 bytes while its layout needed 80, so a freshly written
   store never verified.
5. `SocketSubsystem` was a local in `listen_tcp`, so `WSACleanup` ran while the listener
   was open; a separate process could not connect to it.
6. Consumers that kept their own `EngineConfig` copy ended up with an empty metric list.
   The engine now falls back to its configured metrics and publishes the effective
   configuration.
7. Windows `system()` strips the first and last quote of a quoted command line containing
   a redirection, which made every child process invocation fail.
8. The engine's catalog accessors returned copies, so any pointer obtained from a lookup
   pointed into a temporary.
9. `StoreWriter::reopen` accounted for the file size before removing a superseded
   trailer, leaving the byte accounting wrong.
10. Window selection and the age model subtracted externally supplied timestamps directly,
    which can overflow for extreme readings.
11. Two equivalence records were written per save, which made restore fail on a duplicate.
12. A closed generation could be presented as the current one because the caller supplied
    `current_generation`; the engine now derives it from the path catalog.
13. Evicting the oldest observation moved every remaining observation, which made admission
    quadratic in the window capacity. The window now evicts in constant time; the
    admission benchmark went from roughly 22 microseconds to roughly 2 microseconds per
    sample as a result.

Two benchmarks were also corrected during hardening: the admission measurement was
including the synthetic generator's own work, and the workload used an observation
interval that put every reading outside the freshness budget, so the summary it measured
contained no numbers at all. The measurements are now separated and use admissible
evidence.

## Package proof

`tests/downstream/run_downstream_check.cmake` installs the built package into a staging
prefix, then configures, builds and runs a separate project whose only link to this
repository is `find_package(JitterObservatory 1.0 CONFIG REQUIRED)`. The consumer also
asserts that the exported target carries no compile options, so this project's warning
policy is never imposed on a downstream build.
