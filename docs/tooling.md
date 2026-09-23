# Tooling

## Library

```cpp
#include <jitter/engine.hpp>
#include <jitter/scenario.hpp>   // deterministic synthetic workloads

jitter::EngineConfig config;
config.policy = jitter::default_instability_policy();
config.boot_id = 1;

jitter::Observatory observatory(config);
if (!observatory.initialize().ok()) { /* inspect the reason code */ }

jitter::ScenarioPlan plan;                 // SYNTHETIC workload
plan.samples = 4096;
auto registered = jitter::register_scenario(observatory, plan, "lab/forward");
if (!registered.ok()) { /* ... */ }

jitter::IngestContext context;
context.now_utc_ns = /* the instant you want the evidence judged at */;
jitter::ingest_plan(observatory, registered.value(), context);

jitter::SummaryRequest request;
request.series = registered.value().series;
request.path = registered.value().path;
request.generation = registered.value().generation;
request.generation_ordinal = registered.value().ordinal;
// window, policy and metrics default to the engine's own configuration when omitted
request.now_utc_ns = context.now_utc_ns;

auto summary = observatory.summarize(request, context, /*track_episodes=*/true);
```

Every entry point returns `Status` or `Result<T>`, and every failure carries an
`ErrorCode` plus a human readable reason. There is no exception path in the library.

## `jitterctl`

```
jitterctl version                                   version document
jitterctl metrics                                   every metric definition
jitterctl demo      --store F [--samples N] [...]   build a SYNTHETIC store and report it
jitterctl catalog   --store F                       declared clocks, sources, series, paths, generations
jitterctl summarize --store F --series HEX          deterministic summary document
jitterctl explain   --store F --series HEX          why the runtime said what it said
jitterctl baseline  --store F --series HEX --name N capture a named baseline
jitterctl compare   --store F --series HEX --baseline HEX
jitterctl history   --store F --series HEX --path HEX   route generation segmented history
jitterctl attribute --store F --series HEX [--metric NAME]
jitterctl verify    --store F                       store integrity and recovery report
jitterctl serve     --store F --port N [--connections K]
```

Common options: `--pretty` (indented JSON), `--now N` (explicit current instant in UTC
nanoseconds) and `--admit-persisted`.

Without `--admit-persisted` the tool demonstrates the conservative default honestly:
a freshly loaded store reports its evidence as `stale` with the reason
`restored_evidence_is_not_admitted_as_current` rather than presenting yesterday's
evidence as today's state. Without `--now` the tool uses the newest receive time it
holds plus one nanosecond, so no command reads the ambient clock.

`verify` exits 0 for a clean store and 3 for a damaged one, and prints every recovery
flag it found. It never repairs.

## `jitter-ingest-probe`

An independent process that speaks the ingest protocol.

```
jitter-ingest-probe --plan FILE --host H --port P

exit codes: 0 all batches acknowledged, 2 handshake rejected,
            3 a batch was not accepted, 4 transport failure, 5 bad arguments
```

The plan file is a flat `key value` list produced by `write_plan_file`. Unknown keys are
rejected rather than ignored, so a plan cannot silently half-apply.

## Wire protocol

```
frame
  magic "JOW1" (4)  version (u32)  type (u16)  flags (u16)
  payload length (u32)  payload CRC-32C (u32)  payload
```

Messages: `hello`, `hello_ack`, `batch`, `batch_ack`, `bye`, `error`. The receiver
validates the magic, the protocol version, the reserved flags, the payload bound and the
checksum before decoding anything, and every batch is acknowledged with the ingestion
verdict so a producer can tell acceptance from refusal.

The listener has no idle timer. It accepts a connection and reads it to completion; a
message of a type a client must not send first is refused with a typed error frame and the
connection is closed. `--connections K` bounds the sessions a `serve` invocation will
handle, so its lifetime is bounded by work rather than by elapsed time.

## Benchmarks

```
bench_observatory   metric computation, batch ingest, summary and export (counts completed
                    work items and verifies a checksum over the results)
bench_store         save, load and load-and-restore of a populated store
```

Every benchmark reports iterations, work items, nanoseconds per item and a checksum, so a
number is only meaningful together with the work it measured.

## Examples

```
example_summarize            build a synthetic scenario and print the canonical summary
example_deterministic_replay determinism across arrival orders and conservative replay handling
example_lock_audit           multi threaded ingest with the lock audit report
```
