# Persistence

The store is an append only file with a versioned header, per record checksums and a
SHA-256 integrity chain. Recovery is conservative: damage is named, never silently
repaired.

## Layout

```
header  80 bytes
  magic "JITTEROB" (8)  format version (u32)  header size (u32)
  store epoch (u64)     created at UTC (i64)  boot id (u64)
  store identity (32)   reserved = 0 (u32)    header CRC-32C (u32)

record
  length (u32)  type (u16)  flags (u16)  payload CRC-32C (u32)
  payload (length bytes)
  record CRC-32C (u32)  chain digest (32)
```

The record CRC covers the length, type, flags and payload. The chain digest is
`SHA-256(previous chain || the same bytes)`, seeded with the digest of the header block,
so a reordered, duplicated or removed record breaks the chain even when every individual
checksum still matches.

All integers are big-endian. The header carries the process incarnation (`boot id`) that
created the store.

## Record types

`clock_domain`, `equivalence`, `source`, `series`, `path`, `generation`, `sample`,
`baseline`, `episode`, `conflict`, `marker`, `trailer`, `source_guard`.

The source guard record is what makes a replay after a restart recognisable as a replay
rather than as a contradiction: whole batch positions cannot be reconstructed from
individual samples, so the guard is persisted explicitly.

## Recovery

`load_store` streams the file and reports every deviation it finds:

| Flag | Meaning |
| --- | --- |
| `truncated_tail` | the last record is incomplete; the bytes after the last intact record are discarded |
| `corrupt_tail_record` | the last record fails its checksums or its chain link |
| `mid_file_corruption` | a record fails while data follows it: the store is refused, never repaired |
| `trailer_present`, `trailer_mismatch` | the closing record is present, and whether its record count and chain root agree |

`clean` is true only when none of the damaging flags is set. Reopening for append
requires explicit authority for a damaged tail, and reports how many bytes were removed.
A superseded trailer is removed on reopen because a new record would otherwise be written
behind it.

## What a restart means

* Restored observations keep their original observation and receive times; nothing is
  re-timestamped.
* Restored evidence is **not** admitted as current evidence unless both the caller and the
  configuration ask for it. By default it is retained for history and reported as
  `stale` with the reason `restored_evidence_is_not_admitted_as_current`.
* A store written by a different process incarnation is reported as such
  (`foreign_incarnation`); that fact alone does not change the age arithmetic, because
  receive times are absolute UTC nanoseconds.
* An episode that was open when the store was written can no longer be confirmed as
  ongoing, so it is imported as closed with the reason `evidence_lost` rather than left
  running.
* The source guard is rebuilt from the persisted guard records, so replaying stored
  evidence after a restart is still fenced.

## Bounds

Record payloads, total record count, total store bytes, samples per batch, metadata
entries and values, hop counts, window capacities, episode and conflict retention are all
bounded, and every externally derived size is computed with checked arithmetic.
