# Evidence model

Every observation carries the answers to six questions: what was observed, by which
source, for which generation, at what observation and receive times, under which source
incarnation, and whether the evidence is admissible now. Nothing in the runtime is
computed from an observation whose admissibility is unknown.

## States

| State | Meaning |
| --- | --- |
| `fresh` | inside every declared age budget and unabridged |
| `stale` | present but older than the freshness budget |
| `expired` | older than the expiry budget; history only |
| `conflicting` | contradicted by another admissible observation |
| `incomplete` | present but not covering the requested scope |
| `unsupported` | the question cannot be answered with the available inputs |
| `unknown` | present but its age cannot be established |
| `missing` | no evidence at all |

These are never merged. In particular, "we have no data" and "we have calm data" are
different answers, and only `fresh` can support a positive instability statement.

## Age is two measurements, not one

* **Ingest age** is `now - received_at`, entirely on the runtime's own clock domain. It
  is always computable and is never a substitute for the observation age.
* **Observation age** is `now - observed_at`, which requires the observation clock
  domain to be comparable with the runtime's local clock. When it is not, the age is
  **unknown**: not zero, not small.
* **In-domain span** is `received_at - observed_at` expressed inside the observation
  clock. It is a transport observation, never an age.

Relaxing `require_observation_age` can turn an unregistered clock domain into `fresh`
on the strength of the ingest age alone. It cannot turn a *known incomparable* domain
into anything but `unsupported`: that is a structural limit, not a policy choice.

## Comparability

Two clock domains are comparable when they are the same domain, or when an operator or
an authoritative source has declared an equivalence with a bounded offset. Equivalences
are never inferred. A declaration is refused when its authority is below the configured
minimum, when its claimed offset exceeds the accepted bound, when it has expired, or when
either domain declares a unit with no rate (`counter_ticks` can be differenced inside its
own domain but can never be converted into time).

## Provenance, authority and replay fencing

Each source publishes a monotonic sequence and declares an incarnation (a boot identity)
and a configuration epoch. The runtime keeps a per source guard and decides, purely as a
function of the guard, the incoming provenance and the content digest:

| Verdict | Meaning |
| --- | --- |
| `accept` | new, admissible evidence |
| `duplicate_idempotent` | same sequence, byte identical content: already stored |
| `stale_incarnation` | from an older incarnation of the source |
| `stale_epoch` | from an older configuration epoch |
| `stale_sequence` | a rewind or replay of an older position |
| `sequence_conflict` | same position, different content: a contradiction |
| `historical` | explicitly declared archive loading |

Authority can never be self asserted. A batch that claims an authority other than the one
registered for its source, or an origin stronger than its source declared, is refused. A
synthetic source can never claim real evidence; a real source may label its evidence
synthetic, because that is a downgrade and therefore honest.

## Conflicts

A contradiction is recorded with both contents, both sources, both authorities and the
time it was seen, and it is recorded at most once per identity. Resolution uses authority
only: a higher authority wins, equal authority is never broken by arrival order, value
magnitude or recency, and the same source contradicting itself is reported as such. An
unresolved contradiction blocks a positive classification unless the policy declares how
many contradictions it tolerates.

## Historical ingest

Archival ingestion is explicitly marked. It never advances a source's guard, and the
observations it stores are subject to the same age rules as anything else, which is why
old evidence is reported as `stale` or `expired` rather than quietly becoming current.
