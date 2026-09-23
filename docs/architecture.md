# Architecture

```
        declarations                 observations                    questions
  clocks, sources, series,   LatencyBatch (framed, versioned)   SummaryRequest
  paths, generations,        ingest -> fencing -> window        CompareRequest
  equivalences, policies                                        HistoryRequest
            |                          |                        AttributionRequest
            v                          v                                |
     +--------------------------------------------------+                |
     |                    Observatory                   |<---------------+
     |  ClockModel  SourceRegistry  SeriesCatalog        |
     |  PathCatalog  SampleWindow[]  SourceGuard[]       |
     |  EpisodeLog  ConflictLog  Baseline[]              |
     +--------------------------------------------------+
            |                    |                     |
       canonical JSON      versioned store        bounded runtime
       + explanations      + recovery             + cancellation
```

## Layers

| Layer | Headers | Responsibility |
| --- | --- | --- |
| Primitives | `error.hpp`, `checked.hpp`, `hash.hpp`, `bytes.hpp`, `id.hpp`, `text.hpp`, `json.hpp` | reason codes, checked arithmetic, SHA-256 and CRC-32C, canonical big-endian encoding, content addressed identities, canonical JSON |
| Domain | `provenance.hpp`, `time.hpp`, `source.hpp`, `series.hpp`, `path.hpp`, `sample.hpp` | identities, clock comparability, generations, observations and batches |
| Analysis | `metrics.hpp`, `window.hpp`, `evidence.hpp`, `classification.hpp`, `episode.hpp`, `baseline.hpp`, `attribution.hpp`, `summary.hpp`, `report.hpp` | bounded windows, named metrics, admissibility, classification, episodes, comparison, attribution, deterministic summaries and explanations |
| Integration | `engine.hpp` | one synchronized observation engine |
| Delivery | `persistence.hpp`, `wire.hpp`, `transport.hpp`, `runtime.hpp`, `render.hpp`, `scenario.hpp` | store, protocol, TCP transport, worker runtime, JSON rendering, deterministic synthetic workloads |

## Identity

Identity is content addressed everywhere. `make_clock_domain_id`, `make_source_id`,
`make_series_id`, `make_path_id`, `make_hop_id`, `make_generation_id`,
`make_metric_id`, `make_window_policy_id`, `make_freshness_policy_id`,
`make_instability_policy_id`, `make_episode_id` and the measurement, batch and summary
digests all derive a 128-bit identity from a domain separated SHA-256 digest of the
canonical encoding of their content.

The registration entry points fill in the identity of a descriptor whose identity is
still nil, so a caller always ends up holding the identity the runtime actually
registered, and registering the same descriptor twice is idempotent.

A path's identity is its stable name, not its momentary route. Hops and their order form
the topology, and a topology change opens a new generation instead of creating a new
path. That is what makes "route generation changed" a first-class event rather than an
invisible discontinuity.

## Determinism

* No pure function reads the ambient clock. Ages, windows, classifications, summaries and
  explanations all take `now` explicitly, which is why replay driven runs are
  reproducible and why the tests never need a timer.
* Canonical JSON sorts object members and renders numbers with the shortest round-trip
  form, so two runs that agree on content agree on bytes and on the digest of the
  document.
* A summary carries two identities: `digest` over everything including the operational
  counters that record how evidence arrived, and `measurement_digest` over the
  measurement content alone. Two runs over the same observations agree on the second even
  when the first honestly differs.

## Concurrency and lock ownership

The engine owns exactly one lock. Every public entry point takes it; no public entry point
calls another public entry point, so the lock is never acquired twice by one thread. The
unlocked implementations are private and are the only things the composed operations call.

The worker runtime takes its own queue lock, and a worker releases the queue lock before
it runs a job, so a thread never holds two runtime locks. The documented order
(`Queue` then `Engine`) exists so that a future change has a rule to break and an
auditor to catch it.

`JITTER_LOCK_AUDIT` (on by default) instruments every audited lock acquisition and
records nested acquisitions, reentrant acquisitions and the maximum hold depth. The
concurrency tests assert the audit is violation free after a multi threaded workload, and
the `example_lock_audit` walkthrough prints the same figures.

Catalog accessors return references to engine owned state rather than copies, so a pointer
obtained from a lookup always points at the engine's own storage. A caller that inspects
catalogs while another thread ingests must serialise that access, exactly as it would for
any other shared state.

## Bounds

Every externally influenced size is bound checked: batch bytes and sample counts, metadata
entries and sizes, hop counts, window capacities, watched metrics, episodes, conflicts,
baselines, generations retained per path, result rows, store records and bytes, record
payloads, queue depth, worker count and wire frame size. Sizes that are derived from
external input are computed with checked arithmetic and rejected on overflow.
