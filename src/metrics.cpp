// Jitter Observatory - metric definitions, identities and estimators.
// Copyright 2026 Summon Software Labs.
#include <jitter/metrics.hpp>

#include <algorithm>
#include <cmath>

#include <jitter/bytes.hpp>
#include <jitter/checked.hpp>
#include <jitter/text.hpp>

namespace jitter {
namespace {

constexpr double kEwmaAlpha = 1.0 / 16.0;

// Neumaier compensated summation. Fixed order in, fixed bits out: the algorithm has
// no data dependent branching that could reorder the accumulation.
class CompensatedSum {
public:
    void add(double value) noexcept {
        const double t = sum_ + value;
        if (std::fabs(sum_) >= std::fabs(value)) {
            compensation_ += (sum_ - t) + value;
        } else {
            compensation_ += (value - t) + sum_;
        }
        sum_ = t;
    }

    double total() const noexcept { return sum_ + compensation_; }

private:
    double sum_ = 0.0;
    double compensation_ = 0.0;
};

std::vector<double> to_double(std::span<const std::int64_t> values) {
    std::vector<double> out;
    out.reserve(values.size());
    for (const std::int64_t value : values) {
        out.push_back(static_cast<double>(value));
    }
    return out;
}

// Successive absolute deltas, computed in exact integer arithmetic so that a constant
// translation of every reading leaves them bit identical.
Result<std::vector<std::int64_t>> absolute_deltas(std::span<const std::int64_t> ordered) {
    if (ordered.size() < 2) {
        return Result<std::vector<std::int64_t>>::fail(
            ErrorCode::InsufficientEvidence, "an absolute delta metric needs at least two readings");
    }
    std::vector<std::int64_t> deltas;
    deltas.reserve(ordered.size() - 1);
    for (std::size_t i = 1; i < ordered.size(); ++i) {
        std::int64_t delta = 0;
        if (!checked_sub_i64(ordered[i], ordered[i - 1], delta)) {
            return Result<std::vector<std::int64_t>>::fail(
                ErrorCode::Overflow, "successive delta overflowed the signed 64 bit range");
        }
        deltas.push_back(delta < 0 ? -delta : delta);
    }
    return deltas;
}

Result<std::vector<std::int64_t>> signed_deltas(std::span<const std::int64_t> ordered) {
    if (ordered.size() < 2) {
        return Result<std::vector<std::int64_t>>::fail(
            ErrorCode::InsufficientEvidence, "a signed delta metric needs at least two readings");
    }
    std::vector<std::int64_t> deltas;
    deltas.reserve(ordered.size() - 1);
    for (std::size_t i = 1; i < ordered.size(); ++i) {
        std::int64_t delta = 0;
        if (!checked_sub_i64(ordered[i], ordered[i - 1], delta)) {
            return Result<std::vector<std::int64_t>>::fail(
                ErrorCode::Overflow, "successive delta overflowed the signed 64 bit range");
        }
        deltas.push_back(delta);
    }
    return deltas;
}

double mean_of(const std::vector<double>& values) {
    if (values.empty()) {
        return 0.0;
    }
    CompensatedSum sum;
    for (const double value : values) {
        sum.add(value);
    }
    return sum.total() / static_cast<double>(values.size());
}

// Linear interpolation between closest ranks (the "type 7" definition used by common
// statistical packages). The rule is fixed: index = p * (n - 1).
double quantile_of(std::vector<double> sorted, double p) {
    if (sorted.empty()) {
        return 0.0;
    }
    std::sort(sorted.begin(), sorted.end());
    if (sorted.size() == 1) {
        return sorted.front();
    }
    const double rank = p * static_cast<double>(sorted.size() - 1);
    const double floored = std::floor(rank);
    const auto lower = static_cast<std::size_t>(floored);
    const double fraction = rank - floored;
    if (lower + 1 >= sorted.size()) {
        return sorted.back();
    }
    return sorted[lower] + (fraction * (sorted[lower + 1] - sorted[lower]));
}

// Two pass sample variance: the mean is computed first and the squared deviations are
// accumulated afterwards, which keeps the result stable for large offsets.
Result<double> sample_variance_of(const std::vector<double>& values, std::uint64_t min_samples) {
    if (values.size() < min_samples) {
        return Result<double>::fail(ErrorCode::InsufficientEvidence,
                                    "not enough readings for a sample variance",
                                    "have=" + std::to_string(values.size()) +
                                        " need=" + std::to_string(min_samples));
    }
    if (values.size() < 2) {
        return Result<double>::fail(ErrorCode::InsufficientEvidence,
                                    "sample variance needs at least two readings");
    }
    const double mean = mean_of(values);
    CompensatedSum sum;
    for (const double value : values) {
        const double deviation = value - mean;
        sum.add(deviation * deviation);
    }
    return sum.total() / static_cast<double>(values.size() - 1);
}

double peak_to_peak_of(const std::vector<double>& values) {
    if (values.empty()) {
        return 0.0;
    }
    const auto bounds = std::minmax_element(values.begin(), values.end());
    return *bounds.second - *bounds.first;
}

double ewma16_of(const std::vector<double>& deltas) {
    if (deltas.empty()) {
        return 0.0;
    }
    double state = deltas.front();
    for (std::size_t i = 1; i < deltas.size(); ++i) {
        state = state + (kEwmaAlpha * (deltas[i] - state));
    }
    return state;
}

}  // namespace

std::string_view to_string(MetricFamily family) noexcept {
    switch (family) {
        case MetricFamily::AbsoluteDelta: return "absolute_delta";
        case MetricFamily::SignedDelta: return "signed_delta";
        case MetricFamily::Variance: return "variance";
        case MetricFamily::Dispersion: return "dispersion";
        case MetricFamily::OrderStatistic: return "order_statistic";
        case MetricFamily::PeakToPeak: return "peak_to_peak";
        case MetricFamily::Ewma: return "ewma";
        case MetricFamily::Ratio: return "ratio";
        case MetricFamily::CentralTendency: return "central_tendency";
    }
    return "absolute_delta";
}

std::string_view to_string(MetricUnit unit) noexcept {
    switch (unit) {
        case MetricUnit::Nanoseconds: return "nanoseconds";
        case MetricUnit::NanosecondsSquared: return "nanoseconds_squared";
        case MetricUnit::Dimensionless: return "dimensionless";
        case MetricUnit::Count: return "count";
    }
    return "nanoseconds";
}

std::string_view unit_label(MetricUnit unit) noexcept {
    switch (unit) {
        case MetricUnit::Nanoseconds: return "ns";
        case MetricUnit::NanosecondsSquared: return "ns^2";
        case MetricUnit::Dimensionless: return "1";
        case MetricUnit::Count: return "count";
    }
    return "ns";
}

std::string_view to_string(Estimator estimator) noexcept {
    switch (estimator) {
        case Estimator::Mean: return "mean";
        case Estimator::Median: return "median";
        case Estimator::Maximum: return "maximum";
        case Estimator::P95: return "p95";
        case Estimator::P99: return "p99";
        case Estimator::P50: return "p50";
        case Estimator::P25: return "p25";
        case Estimator::P75: return "p75";
        case Estimator::SampleStandardDeviation: return "sample_standard_deviation";
        case Estimator::SampleVariance: return "sample_variance";
        case Estimator::EwmaAlpha1Over16: return "ewma_alpha_1_over_16";
        case Estimator::Span: return "span";
        case Estimator::RatioOfDeviationToCenter: return "ratio_of_deviation_to_center";
    }
    return "mean";
}

std::string_view to_string(MetricInput input) noexcept {
    switch (input) {
        case MetricInput::Latency: return "latency";
        case MetricInput::AbsoluteSuccessiveDelta: return "absolute_successive_delta";
        case MetricInput::SignedSuccessiveDelta: return "signed_successive_delta";
    }
    return "latency";
}

MetricId make_metric_id(const MetricDefinition& definition) {
    DigestBuilder builder(kDomainMetric);
    builder.str(definition.name);
    builder.u32(definition.version);
    builder.u8(static_cast<std::uint8_t>(definition.family));
    builder.u8(static_cast<std::uint8_t>(definition.unit));
    builder.u8(static_cast<std::uint8_t>(definition.estimator));
    builder.u8(static_cast<std::uint8_t>(definition.input));
    builder.boolean(definition.requires_ordered_samples);
    builder.u64(definition.min_samples);
    builder.u64(definition.min_terms);
    builder.str(definition.formula);
    return builder.as_id<MetricTag>();
}

namespace {

struct MetricTableEntry {
    MetricKey key;
    MetricDefinition definition;
};

std::vector<MetricTableEntry> build_table() {
    auto entry = [](MetricKey key, std::string_view name, std::string_view formula, MetricFamily family,
                    MetricUnit unit, Estimator estimator, MetricInput input, std::uint64_t min_samples,
                    std::uint64_t min_terms) {
        MetricTableEntry result;
        result.key = key;
        result.definition.key = key;
        result.definition.name = name;
        result.definition.formula = formula;
        result.definition.version = 1;
        result.definition.family = family;
        result.definition.unit = unit;
        result.definition.estimator = estimator;
        result.definition.input = input;
        result.definition.requires_ordered_samples = true;
        result.definition.min_samples = min_samples;
        result.definition.min_terms = min_terms;
        result.definition.id = make_metric_id(result.definition);
        return result;
    };

    std::vector<MetricTableEntry> table;
    table.push_back(entry(MetricKey::AbsoluteDeltaMean, "jitter.absolute_delta.mean",
                          "mean over i in [1,n-1] of |x_i - x_(i-1)|", MetricFamily::AbsoluteDelta,
                          MetricUnit::Nanoseconds, Estimator::Mean,
                          MetricInput::AbsoluteSuccessiveDelta, 2, 1));
    table.push_back(entry(MetricKey::AbsoluteDeltaMedian, "jitter.absolute_delta.median",
                          "median over i in [1,n-1] of |x_i - x_(i-1)|", MetricFamily::AbsoluteDelta,
                          MetricUnit::Nanoseconds, Estimator::Median,
                          MetricInput::AbsoluteSuccessiveDelta, 2, 1));
    table.push_back(entry(MetricKey::AbsoluteDeltaP95, "jitter.absolute_delta.p95",
                          "95th percentile of |x_i - x_(i-1)| using type 7 interpolation",
                          MetricFamily::AbsoluteDelta, MetricUnit::Nanoseconds, Estimator::P95,
                          MetricInput::AbsoluteSuccessiveDelta, 2, 1));
    table.push_back(entry(MetricKey::AbsoluteDeltaMax, "jitter.absolute_delta.max",
                          "maximum over i in [1,n-1] of |x_i - x_(i-1)|", MetricFamily::AbsoluteDelta,
                          MetricUnit::Nanoseconds, Estimator::Maximum,
                          MetricInput::AbsoluteSuccessiveDelta, 2, 1));
    table.push_back(entry(MetricKey::AbsoluteDeltaEwma16, "jitter.absolute_delta.ewma16",
                          "J_1 = |x_1 - x_0|; J_i = J_(i-1) + (|x_i - x_(i-1)| - J_(i-1))/16",
                          MetricFamily::Ewma, MetricUnit::Nanoseconds, Estimator::EwmaAlpha1Over16,
                          MetricInput::AbsoluteSuccessiveDelta, 2, 1));
    table.push_back(entry(MetricKey::IpdvMean, "jitter.ipdv.mean",
                          "mean over i in [1,n-1] of (x_i - x_(i-1)), preserving sign; the signed "
                          "successive delay variation of the latency series",
                          MetricFamily::SignedDelta, MetricUnit::Nanoseconds, Estimator::Mean,
                          MetricInput::SignedSuccessiveDelta, 2, 1));
    table.push_back(entry(MetricKey::IpdvStdDev, "jitter.ipdv.stddev",
                          "sample standard deviation of the signed successive deltas x_i - x_(i-1)",
                          MetricFamily::SignedDelta, MetricUnit::Nanoseconds,
                          Estimator::SampleStandardDeviation, MetricInput::SignedSuccessiveDelta, 3,
                          2));
    table.push_back(entry(MetricKey::SampleVariance, "jitter.variance.sample",
                          "sum of (x_i - mean)^2 divided by (n-1) over the latency readings",
                          MetricFamily::Variance, MetricUnit::NanosecondsSquared,
                          Estimator::SampleVariance, MetricInput::Latency, 2, 2));
    table.push_back(entry(MetricKey::SampleStdDev, "jitter.stddev.sample",
                          "square root of jitter.variance.sample over the latency readings",
                          MetricFamily::Variance, MetricUnit::Nanoseconds,
                          Estimator::SampleStandardDeviation, MetricInput::Latency, 2, 2));
    table.push_back(entry(MetricKey::CoefficientOfVariation, "jitter.cv",
                          "jitter.stddev.sample divided by the arithmetic mean of the readings",
                          MetricFamily::Ratio, MetricUnit::Dimensionless,
                          Estimator::RatioOfDeviationToCenter, MetricInput::Latency, 2, 2));
    table.push_back(entry(MetricKey::MeanAbsoluteDeviation, "jitter.mad.mean",
                          "mean of |x_i - mean(x)| over the latency readings",
                          MetricFamily::Dispersion, MetricUnit::Nanoseconds, Estimator::Mean,
                          MetricInput::Latency, 1, 1));
    table.push_back(entry(MetricKey::MedianAbsoluteDeviation, "jitter.mad.median",
                          "median of |x_i - median(x)| over the latency readings",
                          MetricFamily::Dispersion, MetricUnit::Nanoseconds, Estimator::Median,
                          MetricInput::Latency, 1, 1));
    table.push_back(entry(MetricKey::PeakToPeak, "jitter.peak_to_peak",
                          "max(x) - min(x) over the latency readings", MetricFamily::PeakToPeak,
                          MetricUnit::Nanoseconds, Estimator::Span, MetricInput::Latency, 1, 1));
    table.push_back(entry(MetricKey::InterquartileRange, "jitter.iqr",
                          "p75(x) - p25(x) over the latency readings", MetricFamily::OrderStatistic,
                          MetricUnit::Nanoseconds, Estimator::Span, MetricInput::Latency, 2, 2));
    table.push_back(entry(MetricKey::LatencyMean, "latency.mean",
                          "arithmetic mean of the latency readings; a central tendency, not a "
                          "jitter metric",
                          MetricFamily::CentralTendency, MetricUnit::Nanoseconds, Estimator::Mean,
                          MetricInput::Latency, 1, 1));
    table.push_back(entry(MetricKey::LatencyP50, "latency.p50",
                          "median latency reading using type 7 interpolation",
                          MetricFamily::CentralTendency, MetricUnit::Nanoseconds, Estimator::P50,
                          MetricInput::Latency, 1, 1));
    table.push_back(entry(MetricKey::LatencyP95, "latency.p95",
                          "95th percentile latency reading using type 7 interpolation",
                          MetricFamily::CentralTendency, MetricUnit::Nanoseconds, Estimator::P95,
                          MetricInput::Latency, 1, 1));
    table.push_back(entry(MetricKey::LatencyP99, "latency.p99",
                          "99th percentile latency reading using type 7 interpolation",
                          MetricFamily::CentralTendency, MetricUnit::Nanoseconds, Estimator::P99,
                          MetricInput::Latency, 1, 1));
    return table;
}

const std::vector<MetricTableEntry>& table() {
    static const std::vector<MetricTableEntry> instance = build_table();
    return instance;
}

}  // namespace

MetricRegistry::MetricRegistry() {
    for (const MetricTableEntry& item : table()) {
        table_.push_back(item.definition);
    }
}

const MetricRegistry& MetricRegistry::instance() {
    static const MetricRegistry registry;
    return registry;
}

const MetricRegistry& metric_registry() { return MetricRegistry::instance(); }

std::size_t MetricRegistry::size() const noexcept { return table_.size(); }

const MetricDefinition& MetricRegistry::definition(MetricKey key) const {
    for (const MetricDefinition& candidate : table_) {
        if (candidate.key == key) {
            return candidate;
        }
    }
    return table_.front();
}

const MetricDefinition* MetricRegistry::find(MetricId id) const noexcept {
    for (const MetricDefinition& candidate : table_) {
        if (candidate.id == id) {
            return &candidate;
        }
    }
    return nullptr;
}

const MetricDefinition* MetricRegistry::find_by_name(std::string_view name) const noexcept {
    for (const MetricDefinition& candidate : table_) {
        if (candidate.name == name) {
            return &candidate;
        }
    }
    return nullptr;
}

std::vector<MetricDefinition> MetricRegistry::definitions() const {
    std::vector<MetricDefinition> copy = table_;
    std::sort(copy.begin(), copy.end(), [](const MetricDefinition& a, const MetricDefinition& b) {
        if (a.name != b.name) {
            return a.name < b.name;
        }
        return a.id < b.id;
    });
    return copy;
}

Status verify_metric_table() {
    const auto& items = table();
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (items[i].definition.id != make_metric_id(items[i].definition)) {
            return Status::failure(ErrorCode::IntegrityFailure,
                                   "metric identity does not match its definition",
                                   std::string(items[i].definition.name));
        }
        for (std::size_t j = i + 1; j < items.size(); ++j) {
            if (items[i].definition.name == items[j].definition.name) {
                return Status::failure(ErrorCode::Duplicate, "two metric definitions share a name",
                                       std::string(items[i].definition.name));
            }
            if (items[i].definition.id == items[j].definition.id) {
                return Status::failure(ErrorCode::Duplicate,
                                       "two metric definitions share an identity",
                                       std::string(items[i].definition.name) + "/" +
                                           std::string(items[j].definition.name));
            }
            if (items[i].key == items[j].key) {
                return Status::failure(ErrorCode::Duplicate, "two metric definitions share a key");
            }
        }
        if (items[i].definition.min_terms == 0 || items[i].definition.min_samples == 0) {
            return Status::failure(ErrorCode::IntegrityFailure,
                                   "metric definition declared a zero minimum",
                                   std::string(items[i].definition.name));
        }
    }
    return Status::success();
}

Result<MetricValue> MetricRegistry::compute(MetricId id,
                                            std::span<const std::int64_t> ordered) const {
    const MetricDefinition* definition_ptr = find(id);
    if (definition_ptr == nullptr) {
        return Result<MetricValue>::fail(ErrorCode::NotFound, "unknown metric identity", id.hex());
    }
    return compute(definition_ptr->key, ordered);
}

Result<MetricValue> MetricRegistry::compute(MetricKey key,
                                            std::span<const std::int64_t> ordered) const {
    const MetricDefinition& def = definition(key);

    MetricValue value;
    value.id = def.id;
    value.key = def.key;
    value.name = def.name;
    value.version = def.version;
    value.unit = def.unit;
    value.sample_count = static_cast<std::uint64_t>(ordered.size());

    const std::uint64_t samples = static_cast<std::uint64_t>(ordered.size());
    if (samples == 0) {
        return Result<MetricValue>::fail(ErrorCode::NoEvidence, "no readings in the window",
                                         std::string(def.name));
    }
    if (samples < def.min_samples) {
        return Result<MetricValue>::fail(ErrorCode::InsufficientEvidence,
                                         "not enough readings for this metric",
                                         std::string(def.name) + " have=" + std::to_string(samples) +
                                             " need=" + std::to_string(def.min_samples));
    }

    switch (def.input) {
        case MetricInput::Latency: {
            const std::vector<double> values = to_double(ordered);
            switch (def.estimator) {
                case Estimator::Mean: {
                    value.value = mean_of(values);
                    value.term_count = samples;
                    break;
                }
                case Estimator::Median: {
                    value.value = quantile_of(values, 0.5);
                    value.term_count = samples;
                    break;
                }
                case Estimator::P50: {
                    value.value = quantile_of(values, 0.5);
                    value.term_count = samples;
                    break;
                }
                case Estimator::P95: {
                    value.value = quantile_of(values, 0.95);
                    value.term_count = samples;
                    break;
                }
                case Estimator::P99: {
                    value.value = quantile_of(values, 0.99);
                    value.term_count = samples;
                    break;
                }
                case Estimator::P25: {
                    value.value = quantile_of(values, 0.25);
                    value.term_count = samples;
                    break;
                }
                case Estimator::P75: {
                    value.value = quantile_of(values, 0.75);
                    value.term_count = samples;
                    break;
                }
                case Estimator::SampleVariance: {
                    auto variance = sample_variance_of(values, def.min_samples);
                    if (!variance.ok()) {
                        return variance.error();
                    }
                    value.value = variance.value();
                    value.term_count = samples;
                    break;
                }
                case Estimator::SampleStandardDeviation: {
                    auto variance = sample_variance_of(values, def.min_samples);
                    if (!variance.ok()) {
                        return variance.error();
                    }
                    value.value = std::sqrt(variance.value());
                    value.term_count = samples;
                    break;
                }
                case Estimator::RatioOfDeviationToCenter: {
                    auto variance = sample_variance_of(values, def.min_samples);
                    if (!variance.ok()) {
                        return variance.error();
                    }
                    const double mean = mean_of(values);
                    if (!(mean > 0.0)) {
                        return Result<MetricValue>::fail(
                            ErrorCode::Unsupported,
                            "coefficient of variation requires a strictly positive mean",
                            std::string(def.name) + " mean=" + text::double_to_string(mean));
                    }
                    value.value = std::sqrt(variance.value()) / mean;
                    value.term_count = samples;
                    break;
                }
                case Estimator::Span: {
                    value.value = peak_to_peak_of(values);
                    value.term_count = samples;
                    break;
                }
                case Estimator::Maximum: {
                    const auto bounds = std::minmax_element(values.begin(), values.end());
                    value.value = *bounds.second;
                    value.term_count = samples;
                    break;
                }
                case Estimator::EwmaAlpha1Over16: {
                    return Result<MetricValue>::fail(
                        ErrorCode::Unsupported,
                        "the exponentially weighted estimator is only defined over successive deltas",
                        std::string(def.name));
                }
            }
            break;
        }
        case MetricInput::AbsoluteSuccessiveDelta: {
            if (def.estimator == Estimator::Span) {
                auto deltas = absolute_deltas(ordered);
                if (!deltas.ok()) {
                    return deltas.error();
                }
                const std::vector<double> values = to_double(deltas.value());
                value.value = peak_to_peak_of(values);
                value.term_count = static_cast<std::uint64_t>(values.size());
                break;
            }
            auto deltas = absolute_deltas(ordered);
            if (!deltas.ok()) {
                return deltas.error();
            }
            if (value.sample_count < def.min_samples) {
                return Result<MetricValue>::fail(ErrorCode::InsufficientEvidence,
                                                 "not enough readings for this delta metric",
                                                 std::string(def.name));
            }
            const std::vector<double> values = to_double(deltas.value());
            value.term_count = static_cast<std::uint64_t>(values.size());
            switch (def.estimator) {
                case Estimator::Mean: value.value = mean_of(values); break;
                case Estimator::Median: value.value = quantile_of(values, 0.5); break;
                case Estimator::P95: value.value = quantile_of(values, 0.95); break;
                case Estimator::Maximum: {
                    const auto bounds = std::minmax_element(values.begin(), values.end());
                    value.value = *bounds.second;
                    break;
                }
                case Estimator::EwmaAlpha1Over16: value.value = ewma16_of(values); break;
                default:
                    return Result<MetricValue>::fail(
                        ErrorCode::Unsupported,
                        "estimator is not defined for absolute successive deltas",
                        std::string(def.name));
            }
            break;
        }
        case MetricInput::SignedSuccessiveDelta: {
            auto deltas = signed_deltas(ordered);
            if (!deltas.ok()) {
                return deltas.error();
            }
            const std::vector<double> values = to_double(deltas.value());
            value.term_count = static_cast<std::uint64_t>(values.size());
            switch (def.estimator) {
                case Estimator::Mean: value.value = mean_of(values); break;
                case Estimator::SampleStandardDeviation: {
                    auto variance = sample_variance_of(values, def.min_samples - 1);
                    if (!variance.ok()) {
                        return variance.error();
                    }
                    value.value = std::sqrt(variance.value());
                    break;
                }
                default:
                    return Result<MetricValue>::fail(
                        ErrorCode::Unsupported,
                        "estimator is not defined for signed successive deltas",
                        std::string(def.name));
            }
            break;
        }
    }

    return value;
}

MetricRegistry::ComputeAllResult MetricRegistry::compute_all(
    const std::vector<MetricKey>& keys, std::span<const std::int64_t> ordered) const {
    ComputeAllResult result;
    for (const MetricKey key : keys) {
        auto value = compute(key, ordered);
        if (!value.ok()) {
            result.unavailable.push_back(std::string(definition(key).name) + ":" +
                                         std::string(to_string(value.code())));
            continue;
        }
        result.values.insert(std::move(value.value()));
    }
    std::sort(result.unavailable.begin(), result.unavailable.end());
    return result;
}

void MetricSet::insert(MetricValue value) { values_[value.id] = value; }

const MetricValue* MetricSet::find(MetricId metric_id) const noexcept {
    const auto it = values_.find(metric_id);
    return it == values_.end() ? nullptr : &it->second;
}

const MetricValue* MetricSet::find(MetricKey metric_key) const noexcept {
    for (const auto& entry : values_) {
        if (entry.second.key == metric_key) {
            return &entry.second;
        }
    }
    return nullptr;
}

std::vector<MetricValue> MetricSet::ordered() const {
    std::vector<MetricValue> out;
    out.reserve(values_.size());
    for (const auto& entry : values_) {
        out.push_back(entry.second);
    }
    std::sort(out.begin(), out.end(), [](const MetricValue& a, const MetricValue& b) {
        if (a.name != b.name) {
            return a.name < b.name;
        }
        return a.id < b.id;
    });
    return out;
}

Status check_comparable(const MetricValue& a, const MetricValue& b) {
    if (a.id != b.id) {
        return Status::failure(ErrorCode::MetricMismatch,
                               "two different metric definitions were compared",
                               std::string(a.name) + " vs " + std::string(b.name));
    }
    if (a.version != b.version) {
        return Status::failure(ErrorCode::VersionUnsupported,
                               "the same metric name was produced by different definition versions",
                               std::string(a.name));
    }
    if (a.unit != b.unit) {
        return Status::failure(ErrorCode::UnitMismatch, "metric units differ",
                               std::string(unit_label(a.unit)) + " vs " +
                                   std::string(unit_label(b.unit)));
    }
    return Status::success();
}

void sort_canonical(std::vector<std::int64_t>& values) noexcept {
    std::sort(values.begin(), values.end());
}

}  // namespace jitter
