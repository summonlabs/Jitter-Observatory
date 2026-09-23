# Proof surfaces: REAL, SYNTHETIC, UNSUPPORTED

Every claim in this repository is labelled. Nothing here is a claim about switching
hardware, RDMA, InfiniBand, NVLink, multi host fabrics or a vendor telemetry source.

## REAL

| Surface | Evidence |
| --- | --- |
| Ingest transport over TCP loopback between two independent operating system processes | `tests/transport/test_transport_process.cpp`: the test process listens with `TcpIngestServer` and a separate `jitter-ingest-probe` process connects, handshakes, sends batches and reads acknowledgements. The test asserts byte counts, frame counts and per batch acknowledgements |
| Command line tooling as real child processes | `tests/e2e/test_cli_end_to_end.cpp`: `jitterctl` is executed as a child process for every command, including `serve`, and its canonical output is parsed |
| Persistence format, integrity and recovery | `tests/unit/test_persistence.cpp` and `tests/integration/test_persistence_restart.cpp`: real files on disk are created, bit flipped, truncated, reopened and reloaded |
| Installable CMake package consumed by an independent project | `tests/downstream/`: the package is installed into a staging prefix and a separate CMake project uses `find_package(JitterObservatory 1.0 CONFIG REQUIRED)`, builds and runs |
| Determinism, ordering and invariant properties | `tests/property/test_determinism_invariants.cpp`: seeded randomised workloads, repeated runs, byte level comparison of canonical output |
| Bounded concurrency, backpressure, cancellation and lock auditing | `tests/concurrency/test_runtime_concurrency.cpp`: real threads, real worker queues, and the lock auditor's counters |
| Metric arithmetic | `tests/unit/test_metrics_window.cpp`: exact expected values, exact relations (`stddev == sqrt(variance)`), exact translation invariance of delta metrics |
| Memory and lifetime behaviour under AddressSanitizer | the whole suite is run under MSVC `/fsanitize=address` (see `docs/validation.md`) |

## SYNTHETIC

Everything about the *contents* of an observation in this repository is synthetic.

| Surface | Why |
| --- | --- |
| `jitter::ScenarioPlan` / `register_scenario` / `make_batches` | a deterministic generator, labelled `EvidenceOrigin::Synthetic` with `SourceAuthority::Simulated` |
| `jitterctl demo` | writes a store whose observations are generated, and prints `"evidence_origin": "synthetic"` |
| Every test that ingests latency values | the values come from the generator, not from a fabric |
| The TCP transport tests | the *transport* is real; the latency values it carries are synthetic, and the tests assert `synthetic_origin` is the full sample count and `real_origin` is zero |

The generator declares its own clock equivalence with the runtime timeline because that
statement is true *for the generator*: observation and receive times are derived from one
run clock. It is declared explicitly, with a justification, and it is part of the store,
rather than being assumed by the code.

## UNSUPPORTED

These are things this runtime cannot observe, and therefore never claims:

* Per hop timing attribution for a hop that emits no timestamps, or whose clock domain
  cannot be compared with the series clock. The report says `unsupported` with the exact
  reason and produces **no value**; `complete` is false.
* Instability classification without a declared policy. An empty policy yields
  `unsupported` with the reason `no_threshold_rules_declared`, never `stable`.
* Any statement about the current state from stale, expired, unknown, unsupported or
  contradicted evidence.
* Any measurement of a real fabric. There is no switch, ASIC, RDMA, InfiniBand, NVLink or
  vendor telemetry integration in this repository, and no test pretends otherwise.
* Leak detection under MSVC AddressSanitizer: the platform reports
  `detect_leaks is not supported`, so no leak claim is made. ASan coverage here is
  memory error detection, not leak detection.
* ThreadSanitizer: not available on this toolchain. Race freedom is argued from the lock
  ownership audit and exercised by real multi threaded tests, and it is *not* claimed as
  an equivalent substitute for a race detector.
