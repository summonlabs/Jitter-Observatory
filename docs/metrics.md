# Metrics

Every metric is a named, versioned definition with its own content addressed identity.
There is no operation in this runtime that returns "the jitter" of a window: a caller
names a metric, and the result carries the identity of the definition that produced it.

## Identity rules

A metric identity is a SHA-256 based digest of the definition: name, version, family,
unit, estimator, input, the ordering requirement, the minimum sample and term counts, and
the formula text. Three consequences follow, and all three are covered by tests:

1. Two different definitions can never share an identity.
2. Changing a definition (including its formula text or version) always yields a new
   identity, so persisted results can never be silently reinterpreted.
3. Two values can never be compared unless they carry the same identity, definition
   version and unit. `check_comparable` enforces this and reports
   `metric_mismatch`, `version_unsupported` or `unit_mismatch`.

## Definitions

| Name | Family | Unit | Input | Formula |
| --- | --- | --- | --- | --- |
| `jitter.absolute_delta.mean` | absolute delta | ns | successive absolute deltas | mean of \|x_i - x_(i-1)\| |
| `jitter.absolute_delta.median` | absolute delta | ns | successive absolute deltas | median of \|x_i - x_(i-1)\|, type 7 interpolation |
| `jitter.absolute_delta.p95` | absolute delta | ns | successive absolute deltas | 95th percentile of \|x_i - x_(i-1)\|, type 7 interpolation |
| `jitter.absolute_delta.max` | absolute delta | ns | successive absolute deltas | maximum of \|x_i - x_(i-1)\| |
| `jitter.absolute_delta.ewma16` | exponentially weighted | ns | successive absolute deltas | J_1 = \|x_1 - x_0\|; J_i = J_(i-1) + (\|x_i - x_(i-1)\| - J_(i-1))/16 |
| `jitter.ipdv.mean` | signed delta | ns | signed successive deltas | mean of (x_i - x_(i-1)), sign preserved |
| `jitter.ipdv.stddev` | signed delta | ns | signed successive deltas | sample standard deviation of (x_i - x_(i-1)) |
| `jitter.variance.sample` | variance | ns^2 | latency readings | sum of (x_i - mean)^2 divided by (n-1) |
| `jitter.stddev.sample` | variance | ns | latency readings | square root of `jitter.variance.sample` |
| `jitter.cv` | ratio | 1 | latency readings | `jitter.stddev.sample` divided by the mean |
| `jitter.mad.mean` | dispersion | ns | latency readings | mean of \|x_i - mean\| |
| `jitter.mad.median` | dispersion | ns | latency readings | median of \|x_i - median\| |
| `jitter.peak_to_peak` | peak to peak | ns | latency readings | max(x) - min(x) |
| `jitter.iqr` | order statistic | ns | latency readings | p75(x) - p25(x) |
| `latency.mean` | central tendency | ns | latency readings | arithmetic mean |
| `latency.p50`, `latency.p95`, `latency.p99` | central tendency | ns | latency readings | order statistics with type 7 interpolation |

`jitter.absolute_delta.ewma16` is deliberately **not** called an RFC 3550 metric. RFC
3550 defines its interarrival jitter over RTP packet spacing, and this runtime consumes a
latency series, so the formula above is stated explicitly and the name claims nothing
about the protocol.

## Properties that are enforced

* **Order independence.** A window canonicalises its observations by observation time,
  then source sequence, then identity, before anything is computed. Two runs that receive
  the same observations in different orders inside their batches produce identical metric
  values and an identical measurement digest.
* **Exact delta arithmetic.** Latency is stored as signed integer nanoseconds, so
  successive deltas are exact and the absolute delta metrics are exactly translation
  invariant: adding a constant to every reading leaves every absolute delta metric bit
  identical.
* **Exact relations.** `jitter.stddev.sample` is literally the square root of
  `jitter.variance.sample` computed from the same accumulator, and `jitter.cv` is
  literally the standard deviation divided by the mean. The tests assert bit equality.
* **Deterministic accumulation.** Sums use Neumaier compensated summation over a fixed
  order, quantiles use a fixed interpolation rule, and no metric depends on hash order or
  thread scheduling.
* **Honest insufficiency.** A metric that cannot be produced is reported as
  `insufficient_evidence` or `no_evidence` with a reason code; it is never replaced by
  a zero, a default or a partial value.

## Units

Variance is nanoseconds squared. A variance and a standard deviation can never be
compared or mixed; the unit is part of the metric identity, so the mismatch is caught
before any arithmetic happens.
