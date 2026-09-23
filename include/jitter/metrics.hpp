// Jitter Observatory - named metric definitions and their identities.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <jitter/error.hpp>
#include <jitter/id.hpp>

namespace jitter {

// What mathematical object the number is. Two metrics from different families are
// never interchangeable even when their units coincide.
enum class MetricFamily : std::uint8_t {
    AbsoluteDelta = 0,
    SignedDelta = 1,
    Variance = 2,
    Dispersion = 3,
    OrderStatistic = 4,
    PeakToPeak = 5,
    Ewma = 6,
    Ratio = 7,
    CentralTendency = 8,
};

// The unit of the produced number. Variance is nanoseconds squared and can never be
// compared with a nanosecond value without an explicit conversion.
enum class MetricUnit : std::uint8_t {
    Nanoseconds = 0,
    NanosecondsSquared = 1,
    Dimensionless = 2,
    Count = 3,
};

std::string_view to_string(MetricFamily family) noexcept;
std::string_view to_string(MetricUnit unit) noexcept;
std::string_view unit_label(MetricUnit unit) noexcept;

enum class Estimator : std::uint8_t {
    Mean = 0,
    Median = 1,
    Maximum = 2,
    P95 = 3,
    P99 = 4,
    P50 = 5,
    P25 = 6,
    P75 = 7,
    SampleStandardDeviation = 8,
    SampleVariance = 9,
    EwmaAlpha1Over16 = 10,
    Span = 11,
    RatioOfDeviationToCenter = 12,
};

std::string_view to_string(Estimator estimator) noexcept;

// Which derived input the estimator consumes.
enum class MetricInput : std::uint8_t {
    // The latency readings themselves, in canonical order.
    Latency = 0,
    // The successive absolute deltas |x_i - x_(i-1)|.
    AbsoluteSuccessiveDelta = 1,
    // The successive signed deltas x_i - x_(i-1), also known as IPDV.
    SignedSuccessiveDelta = 2,
};

std::string_view to_string(MetricInput input) noexcept;

// Every metric the runtime can compute. The enumerator is a compile time handle; the
// durable identity is the MetricId derived from the definition.
enum class MetricKey : std::uint8_t {
    AbsoluteDeltaMean = 0,
    AbsoluteDeltaMedian = 1,
    AbsoluteDeltaP95 = 2,
    AbsoluteDeltaMax = 3,
    AbsoluteDeltaEwma16 = 4,
    IpdvMean = 5,
    IpdvStdDev = 6,
    SampleVariance = 7,
    SampleStdDev = 8,
    CoefficientOfVariation = 9,
    MeanAbsoluteDeviation = 10,
    MedianAbsoluteDeviation = 11,
    PeakToPeak = 12,
    InterquartileRange = 13,
    LatencyMean = 14,
    LatencyP50 = 15,
    LatencyP95 = 16,
    LatencyP99 = 17,
    Count = 18,
};

struct MetricDefinition {
    MetricId id;
    MetricKey key = MetricKey::Count;
    std::string_view name;
    std::string_view formula;
    std::uint32_t version = 1;
    MetricFamily family = MetricFamily::AbsoluteDelta;
    MetricUnit unit = MetricUnit::Nanoseconds;
    Estimator estimator = Estimator::Mean;
    MetricInput input = MetricInput::Latency;
    // Delta metrics are only meaningful over a canonically ordered series; the flag is
    // part of the identity so that an unordered consumer cannot silently reuse them.
    bool requires_ordered_samples = true;
    // Minimum number of latency readings, not terms, needed for the metric to exist.
    std::uint64_t min_samples = 1;
    // Minimum number of terms (deltas or readings) the estimator must combine.
    std::uint64_t min_terms = 1;
};

MetricId make_metric_id(const MetricDefinition& definition);

struct MetricValue {
    MetricId id;
    MetricKey key = MetricKey::Count;
    std::string_view name;
    std::uint32_t version = 0;
    MetricUnit unit = MetricUnit::Nanoseconds;
    double value = 0.0;
    // Readings present in the window that fed this metric.
    std::uint64_t sample_count = 0;
    // Terms actually combined (n for central tendency, n-1 for delta metrics).
    std::uint64_t term_count = 0;

    const char* unit_text() const noexcept { return unit_label(unit).data(); }
};

// A set of metric values keyed by their durable identity. There is no way to obtain a
// value without naming the exact metric, so two metrics can never be confused.
class MetricSet {
public:
    void insert(MetricValue value);
    const MetricValue* find(MetricId metric_id) const noexcept;
    const MetricValue* find(MetricKey metric_key) const noexcept;
    bool contains(MetricId id) const noexcept { return find(id) != nullptr; }
    std::size_t size() const noexcept { return values_.size(); }
    bool empty() const noexcept { return values_.empty(); }
    // Sorted by metric name, then identity: the canonical presentation order.
    std::vector<MetricValue> ordered() const;
    const std::map<MetricId, MetricValue>& raw() const noexcept { return values_; }
    void clear() { values_.clear(); }

private:
    std::map<MetricId, MetricValue> values_;
};

// Verifies that no two definitions share a name, a key or an identity. A failure here
// is a programming error in the table itself and is reported, never ignored.
Status verify_metric_table();

class MetricRegistry {
public:
    static const MetricRegistry& instance();

    const MetricDefinition& definition(MetricKey key) const;
    const MetricDefinition* find(MetricId id) const noexcept;
    const MetricDefinition* find_by_name(std::string_view name) const noexcept;
    std::vector<MetricDefinition> definitions() const;
    std::size_t size() const noexcept;

    // Computes one metric over canonically ordered latency readings. The caller must
    // supply readings ordered by observation time; the window does this so that
    // insertion order never influences a result.
    Result<MetricValue> compute(MetricKey key, std::span<const std::int64_t> ordered_latency_ns) const;
    Result<MetricValue> compute(MetricId id, std::span<const std::int64_t> ordered_latency_ns) const;

    // Computes every requested metric. A metric that cannot be produced is reported in
    // the accompanying diagnostics rather than silently dropped.
    struct ComputeAllResult {
        MetricSet values;
        std::vector<std::string> unavailable;
    };
    ComputeAllResult compute_all(const std::vector<MetricKey>& keys,
                                 std::span<const std::int64_t> ordered_latency_ns) const;

private:
    MetricRegistry();
    std::vector<MetricDefinition> table_;
};

const MetricRegistry& metric_registry();

// Rejects an attempt to compare, subtract or average two values that are not the same
// metric at the same definition version and unit.
Status check_comparable(const MetricValue& a, const MetricValue& b);

// Canonical ordering of latency readings: by reading value order is irrelevant; the
// caller passes readings already sorted by observation time. This helper sorts a
// caller supplied sequence deterministically and is used by tests to prove that
// insertion order does not matter.
void sort_canonical(std::vector<std::int64_t>& values) noexcept;

}  // namespace jitter
