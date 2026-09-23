// Jitter Observatory - explicit outcome and reason codes.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cassert>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace jitter {

// Reason codes are part of the public contract: summaries, explanations and exports
// surface them verbatim so that a caller can distinguish "no evidence" from
// "evidence that says stable" from "evidence we refuse to use".
enum class ErrorCode : std::uint16_t {
    Ok = 0,
    InvalidArgument,
    OutOfRange,
    Overflow,
    NotFound,
    Duplicate,
    Conflict,
    StaleEpoch,
    StaleIncarnation,
    StaleSequence,
    StaleGeneration,
    StaleEvidence,
    Incomparable,
    MetricMismatch,
    UnitMismatch,
    Unsupported,
    PolicyViolation,
    CapacityExceeded,
    LimitExceeded,
    IntegrityFailure,
    VersionUnsupported,
    Corrupt,
    Cancelled,
    ShuttingDown,
    Backpressure,
    ProtocolViolation,
    IoFailure,
    NoEvidence,
    InsufficientEvidence,
    NotReady,
    Internal,
};

std::string_view to_string(ErrorCode code) noexcept;

// True for the family of codes that mean "evidence exists but is not admissible now".
bool is_staleness_code(ErrorCode code) noexcept;

struct Error {
    ErrorCode code = ErrorCode::Ok;
    std::string message;
    std::string context;

    Error() = default;
    Error(ErrorCode c, std::string m, std::string ctx = {})
        : code(c), message(std::move(m)), context(std::move(ctx)) {}

    bool ok() const noexcept { return code == ErrorCode::Ok; }
    std::string to_text() const;
};

inline Error make_error(ErrorCode code, std::string message, std::string context = {}) {
    return Error(code, std::move(message), std::move(context));
}

// Unit type for operations that report success or failure without a value.
class Status {
public:
    Status() = default;
    Status(Error error) : error_(std::move(error)) {}  // NOLINT(google-explicit-constructor)

    static Status success() { return Status{}; }
    static Status failure(ErrorCode code, std::string message, std::string context = {}) {
        return Status(Error(code, std::move(message), std::move(context)));
    }

    bool ok() const noexcept { return error_.code == ErrorCode::Ok; }
    const Error& error() const noexcept { return error_; }
    ErrorCode code() const noexcept { return error_.code; }

private:
    Error error_{};
};

// Value-or-error. The payload only exists when the call succeeded; reading it after a
// failure is a programming error caught by the debug assertion.
template <class T>
class Result {
public:
    Result() = default;
    Result(T value) : value_(std::move(value)), ok_(true) {}          // NOLINT
    Result(Error error) : error_(std::move(error)), ok_(false) {}     // NOLINT

    static Result ok_result(T value) { return Result(std::move(value)); }
    static Result fail(ErrorCode code, std::string message, std::string context = {}) {
        return Result(Error(code, std::move(message), std::move(context)));
    }

    bool ok() const noexcept { return ok_; }
    const Error& error() const noexcept { return error_; }
    ErrorCode code() const noexcept { return ok_ ? ErrorCode::Ok : error_.code; }

    // Callers must test ok() first; the assertion documents and enforces that in debug
    // builds.
    const T& value() const noexcept {
        assert(value_.has_value());
        return *value_;
    }
    T& value() noexcept {
        assert(value_.has_value());
        return *value_;
    }
    T&& take() noexcept {
        assert(value_.has_value());
        return std::move(*value_);
    }

    const T* operator->() const noexcept { return &value(); }
    T* operator->() noexcept { return &value(); }
    const T& operator*() const noexcept { return value(); }
    T& operator*() noexcept { return value(); }

private:
    std::optional<T> value_;
    Error error_{};
    bool ok_ = false;
};

#define JITTER_TRY(expr)                        \
    do {                                        \
        const ::jitter::Status jitter_status_ = (expr); \
        if (!jitter_status_.ok()) {             \
            return jitter_status_.error();      \
        }                                       \
    } while (false)

// Each expansion gets a unique local name, so several may appear in one scope.
#define JITTER_DETAIL_JOIN_IMPL(a, b) a##b
#define JITTER_DETAIL_JOIN(a, b) JITTER_DETAIL_JOIN_IMPL(a, b)

#define JITTER_TRY_ASSIGN_ID(dest, expr, unique)               \
    auto JITTER_DETAIL_JOIN(jitter_result_, unique) = (expr);  \
    if (!JITTER_DETAIL_JOIN(jitter_result_, unique).ok()) {    \
        return JITTER_DETAIL_JOIN(jitter_result_, unique).error(); \
    }                                                          \
    (dest) = std::move(JITTER_DETAIL_JOIN(jitter_result_, unique).value())

#define JITTER_TRY_ASSIGN(dest, expr) JITTER_TRY_ASSIGN_ID(dest, expr, __COUNTER__)

// Declares a new local of the given type from a Result<T> expression.
#define JITTER_TRY_DECL_ID(type, name, expr, unique)          \
    auto JITTER_DETAIL_JOIN(jitter_result_, unique) = (expr); \
    if (!JITTER_DETAIL_JOIN(jitter_result_, unique).ok()) {   \
        return JITTER_DETAIL_JOIN(jitter_result_, unique).error(); \
    }                                                         \
    type name = std::move(JITTER_DETAIL_JOIN(jitter_result_, unique).value())

#define JITTER_TRY_DECL(type, name, expr) JITTER_TRY_DECL_ID(type, name, expr, __COUNTER__)

#define JITTER_TRY_RETURN(expr)              \
    do {                                     \
        auto jitter_result_ = (expr);        \
        if (!jitter_result_.ok()) {          \
            return jitter_result_.error();   \
        }                                    \
        return std::move(jitter_result_.value()); \
    } while (false)

}  // namespace jitter
