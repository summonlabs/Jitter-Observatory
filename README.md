# Jitter Observatory

A standalone, vendor-neutral **Fabric OS runtime that owns observation of latency
variance and path timing instability**, built by Summon Software Labs.

Jitter Observatory ingests typed latency observations, keeps them in bounded windows,
computes explicitly named jitter metrics, decides whether the evidence is admissible,
classifies instability against a declared policy, attributes timing to individual hops
only where the clocks actually permit it, segments history by route generation, and
persists all of it in a versioned, integrity checked store.

It does **not** route traffic, enforce objectives, claim latency causality, or classify
arbitrary variance as a fault. See [docs/boundary.md](docs/boundary.md).

```
                declarations                      observations            questions
  clocks, sources, series, paths,        latency batches, fenced by      summarize, compare,
  generations, equivalences, policies    sequence, epoch, incarnation    history, explain,
                    |                              |                     attribute, export
                    v                              v                            |
          +-------------------------------------------------------+               |
          |                      Observatory                      |<--------------+
          |  broad clocks, registries, windows, guards, episodes   |
          +-------------------------------------------------------+
                    |                     |                    |
             canonical JSON         versioned store       bounded runtime
             + explanations         + recovery            + cancellation
```

## Headline guarantees

Each of these is enforced by the implementation and covered by tests that would fail if
the guarantee were weakened.

| Guarantee | Where it is enforced | Test |
| --- | --- | --- |
| Metric definitions never blur together: every metric has its own content addressed identity and its own unit, and two different metrics are never comparable | `metrics.hpp`, `check_comparable` | `test_core`, `test_metrics_window` |
| Incomparable clocks cannot yield per hop jitter attribution: the hop is reported `unsupported` with the exact reason and **no value is fabricated** | `attribution.hpp` | `test_baseline_attribution`, `test_generation_history` |
| Route generation changes segment history: a new topology opens a generation, closes open episodes, and makes comparisons across generations refuse rather than approximate | `path.hpp`, `episode.hpp`, `baseline.hpp` | `test_generation_history` |
| Stale evidence cannot establish current instability: with no fresh observation, the level is `stale`, `expired`, `missing`, `conflicting` or `insufficient` — never `stable` and never `unstable` | `classification.hpp` | `test_classification`, `test_engine_ingest` |
| Identical samples and window policy yield deterministic metrics: arrival order inside a batch and batch size never change the measurement content | `window.hpp`, `summary.hpp` | `test_properties`, `example_deterministic_replay` |
| Persisted dynamic evidence never silently becomes fresh after a restart | `engine.hpp` restore path | `test_persistence_restart` |
| Absence of evidence is never positive evidence | `evidence.hpp` | `test_time_evidence`, `test_adversarial` |
| No threshold exists without a declared, justified policy; an empty policy asserts nothing | `classification.hpp` | `test_classification` |

## Building

Requirements: a C++20 compiler, CMake 3.24 or newer, and (for the tests) a compiler on
`PATH` so that the downstream package proof can build its consumer.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix /some/prefix
```

Options: `JITTER_BUILD_APPS`, `JITTER_BUILD_TESTS`, `JITTER_BUILD_EXAMPLES`,
`JITTER_BUILD_BENCHMARKS`, `JITTER_WARNINGS_AS_ERRORS` (default on),
`JITTER_ENABLE_ASAN`, `JITTER_ENABLE_LOCK_AUDIT` (default on).

The build uses `/W4 /WX` on MSVC and `-Wall -Wextra -Wpedantic -Wconversion` plus
friends elsewhere, applied **privately**: installing the package never imposes this
project's warning policy on a consumer.

### Consuming the installed package

```cmake
find_package(JitterObservatory 1.0 CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE JitterObservatory::jitter_observatory)
```

`tests/downstream/` is exactly that project, and it is built and executed by the test
suite from a staged installation.

## Command line

```sh
jitterctl version
jitterctl metrics
jitterctl demo      --store demo.jostore --samples 4096 --seed 7 --spike-every 64
jitterctl catalog   --store demo.jostore
jitterctl summarize --store demo.jostore --series <hex> --pretty
jitterctl explain   --store demo.jostore --series <hex>
jitterctl baseline  --store demo.jostore --series <hex> --name nightly
jitterctl compare   --store demo.jostore --series <hex> --baseline <hex>
jitterctl history   --store demo.jostore --series <hex> --path <hex>
jitterctl attribute --store demo.jostore --series <hex>
jitterctl verify    --store demo.jostore
jitterctl serve     --store demo.jostore --port 9000 --connections 1
```

`jitter-ingest-probe --plan FILE --host H --port P` is an independent process that
speaks the wire protocol and reports what the receiver acknowledged. See
[docs/tooling.md](docs/tooling.md).

A store written by `demo` carries **synthetic** observations and says so on every
document it produces. A summary from a freshly loaded store reports its evidence as
`stale` by default, with the reason `restored_evidence_is_not_admitted_as_current`;
`--admit-persisted` is the explicit opt in.

## Repository layout

```
include/jitter/   public headers, one per layer
src/              implementation, plus the small platform socket layer
apps/             jitterctl, jitter-ingest-probe
tests/            unit, integration, property, adversarial, concurrency, transport,
                  end to end, and the independent downstream consumer
examples/         summarize, deterministic replay, lock audit walkthrough
benchmarks/       metric/ingest/summary and persistence benchmarks
docs/             boundary, architecture, metrics, evidence model, persistence,
                  tooling, proof surfaces, validation
```

## Documentation

* [Boundary](docs/boundary.md) — what is owned and what is explicitly not.
* [Architecture](docs/architecture.md) — layers, identity, determinism, lock ownership.
* [Metrics](docs/metrics.md) — every definition, formula, unit and identity rule.
* [Evidence model](docs/evidence-model.md) — freshness, provenance, authority, conflicts.
* [Persistence](docs/persistence.md) — format, integrity, recovery, restart semantics.
* [Tooling](docs/tooling.md) — library use, command line, wire protocol, benchmarks.
* [Proof surfaces](docs/proof-surfaces.md) — every REAL, SYNTHETIC and UNSUPPORTED claim.
* [Validation](docs/validation.md) — commands, results, and the defects hardening found.

## Limits and honest gaps

* The repository contains **no** switch, ASIC, RDMA, InfiniBand, NVLink or multi host
  integration. Every latency value in tests and tools is generated by a deterministic
  synthetic source. The real, proven surfaces are the transport, the store, the tooling,
  the package and the arithmetic.
* The ingest listener has no idle timeout by design (a timer would make tests
  non-deterministic); a session's lifetime is bounded by `--connections` and by the
  per-connection frame and batch budgets.
* Exported JSON escapes control characters but does not validate UTF-8: metadata values
  are treated as opaque text.
* AddressSanitizer on MSVC provides memory error detection but not leak detection, and
  there is no ThreadSanitizer here. Race freedom rests on the lock ownership audit and on
  real multi threaded tests, not on a race detector.
* Baselines are compared against the current window of the *same* generation and window
  policy. Cross generation and cross policy comparisons are refused by design.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
