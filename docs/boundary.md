# Boundary: what Jitter Observatory owns

Jitter Observatory observes latency variance and path timing instability. It is a
measurement runtime, not a control plane. The boundary below is enforced by the API, not
by convention.

## Owned

| Capability | Where |
| --- | --- |
| Typed measurement, series, path, hop, source, generation and clock domain identities | `include/jitter/id.hpp`, `source.hpp`, `series.hpp`, `path.hpp`, `sample.hpp`, `time.hpp` |
| Bounded observation windows with deterministic selection | `include/jitter/window.hpp` |
| Explicitly named jitter metrics, including absolute delta and variance based measures | `include/jitter/metrics.hpp` |
| Baseline capture and comparison | `include/jitter/baseline.hpp` |
| Policy driven instability classification | `include/jitter/classification.hpp` |
| Clock comparability assessment | `include/jitter/time.hpp` |
| Per hop timing attribution where timing permits | `include/jitter/attribution.hpp` |
| Route generation segmentation of history | `include/jitter/path.hpp`, `include/jitter/episode.hpp` |
| Historical instability episodes | `include/jitter/episode.hpp` |
| Freshness, provenance and conflict semantics | `include/jitter/evidence.hpp`, `include/jitter/provenance.hpp` |
| Deterministic summaries, explanations and canonical export | `include/jitter/summary.hpp`, `report.hpp`, `render.hpp`, `json.hpp` |
| Versioned, integrity checked persistence with conservative recovery | `include/jitter/persistence.hpp` |
| Bounded worker runtime, backpressure and cancellation | `include/jitter/runtime.hpp` |
| Real TCP ingest transport | `include/jitter/transport.hpp`, `wire.hpp` |

## Explicitly not owned

* **Traffic routing.** Nothing in this runtime changes a route, a queue, a shaper or a
  forwarding decision. The hop descriptors in a path are declarations about what the
  runtime was told the path is; they are never used to program anything.
* **Objective enforcement.** There are no SLOs, budgets, alerts or actions. A threshold
  in an instability policy produces a label, never a change to the system under
  observation.
* **Latency causality.** The runtime never claims that a hop or a component caused the
  end to end latency. Per hop values are hop local timing observations, and the
  attribution report carries that sentence verbatim.
* **Fault classification.** The strongest statement the runtime makes is
  `elevated` or `unstable` against a declared policy. There is no `fault` level, no
  severity escalation and no inference from a value to a cause.
* **Hardware truth.** No switch, ASIC, RDMA, InfiniBand, NVLink or multi host claim is
  made anywhere. Hop device notes are free text supplied by a source.

## Consequences a caller should expect

* Absence of evidence never produces a positive statement. `EvidenceState::Missing`
  classifies as `InstabilityLevel::Missing`, never as `stable`.
* Evidence that contradicts itself blocks a positive statement unless the policy
  explicitly tolerates a declared number of contradictions.
* Evidence whose age cannot be established on a comparable clock is
  `EvidenceState::Unsupported` or `Unknown`, never `Fresh`.
* A route generation change segments history. Nothing is aggregated across generations,
  and a comparison across generations is refused rather than approximated.
* Persisted evidence is not admitted as current evidence unless the caller and the
  configuration both ask for it.
