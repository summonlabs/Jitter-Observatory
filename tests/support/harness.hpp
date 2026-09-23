// Jitter Observatory - first party test harness.
// Copyright 2026 Summon Software Labs.
//
// The harness has no timers of any kind: a test either completes or it does not, and
// no assertion depends on wall clock progress.
#pragma once

#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <typeinfo>
#include <type_traits>
#include <utility>
#include <vector>

#include <jitter/classification.hpp>
#include <jitter/error.hpp>
#include <jitter/evidence.hpp>
#include <jitter/hash.hpp>
#include <jitter/id.hpp>
#include <jitter/metrics.hpp>
#include <jitter/provenance.hpp>
#include <jitter/source.hpp>
#include <jitter/time.hpp>

namespace jitter {

// Streaming helpers so that assertion failures can print domain values.
template <class Tag>
std::ostream& operator<<(std::ostream& stream, const StrongId<Tag>& id) {
    return stream << id.label();
}
inline std::ostream& operator<<(std::ostream& stream, const Digest& digest) {
    return stream << digest.hex();
}
inline std::ostream& operator<<(std::ostream& stream, ErrorCode code) {
    return stream << to_string(code);
}
inline std::ostream& operator<<(std::ostream& stream, EvidenceOrigin origin) {
    return stream << to_string(origin);
}
inline std::ostream& operator<<(std::ostream& stream, SourceAuthority authority) {
    return stream << to_string(authority);
}
inline std::ostream& operator<<(std::ostream& stream, ComparabilityVerdict verdict) {
    return stream << to_string(verdict);
}
inline std::ostream& operator<<(std::ostream& stream, TimeUnit unit) {
    return stream << to_string(unit);
}
inline std::ostream& operator<<(std::ostream& stream, ClockKind kind) {
    return stream << to_string(kind);
}
inline std::ostream& operator<<(std::ostream& stream, EvidenceState state) {
    return stream << to_string(state);
}
inline std::ostream& operator<<(std::ostream& stream, InstabilityLevel level) {
    return stream << to_string(level);
}
inline std::ostream& operator<<(std::ostream& stream, SequenceVerdict verdict) {
    return stream << to_string(verdict);
}
inline std::ostream& operator<<(std::ostream& stream, MetricKey key) {
    return stream << metric_registry().definition(key).name;
}
inline std::ostream& operator<<(std::ostream& stream, MetricUnit unit) {
    return stream << unit_label(unit);
}

}  // namespace jitter

namespace jitter::test {

struct Failure {
    std::string file;
    int line = 0;
    std::string expression;
    std::string detail;
};

class Context {
public:
    static Context& current();

    void record(const char* file, int line, std::string expression, std::string detail);
    void reset();
    bool clean() const noexcept { return failures_.empty(); }
    std::size_t failure_count() const noexcept { return failures_.size(); }
    const std::vector<Failure>& failures() const noexcept { return failures_; }

private:
    std::vector<Failure> failures_;
};

bool check(bool condition, const char* expression, const char* file, int line);

template <class T, class = void>
struct is_streamable : std::false_type {};

template <class T>
struct is_streamable<T, std::void_t<decltype(std::declval<std::ostream&>() << std::declval<const T&>())>>
    : std::true_type {};

// Renders a value for a failure message when the type supports streaming, and says so
// explicitly when it does not, so that an assertion never fails to compile just because
// a domain type has no stream operator.
template <class T>
std::string describe_value(const T& value) {
    if constexpr (is_streamable<T>::value) {
        std::ostringstream stream;
        stream << value;
        return stream.str();
    } else {
        return std::string("<") + typeid(T).name() + " has no stream operator>";
    }
}

template <class A, class B>
bool check_eq(const A& left, const B& right, const char* left_text, const char* right_text,
              const char* file, int line) {
    if (left == right) {
        return true;
    }
    std::string detail = std::string(left_text) + " == " + right_text + " | left=" +
                         describe_value(left) + " right=" + describe_value(right);
    Context::current().record(file, line, std::string(left_text) + " == " + right_text, detail);
    return false;
}

bool check_ok(const Status& status, const char* expression, const char* file, int line);

template <class T>
bool check_ok(const Result<T>& result, const char* expression, const char* file, int line) {
    if (result.ok()) {
        return true;
    }
    Context::current().record(file, line, expression,
                              "unexpected failure: " + result.error().to_text());
    return false;
}

bool check_error(const Status& status, ErrorCode expected, const char* expression, const char* file,
                 int line);

template <class T>
bool check_error(const Result<T>& result, ErrorCode expected, const char* expression,
                 const char* file, int line) {
    if (!result.ok() && result.code() == expected) {
        return true;
    }
    std::ostringstream detail;
    detail << "expected " << to_string(expected) << " but got ";
    if (result.ok()) {
        detail << "success";
    } else {
        detail << result.error().to_text();
    }
    Context::current().record(file, line, expression, detail.str());
    return false;
}

using TestFunction = std::function<void()>;

struct TestCase {
    std::string suite;
    std::string name;
    TestFunction function;
};

class Registry {
public:
    static Registry& instance();
    void add(std::string suite, std::string name, TestFunction function);
    const std::vector<TestCase>& cases() const noexcept { return cases_; }

private:
    std::vector<TestCase> cases_;
};

struct Registrar {
    Registrar(const char* suite, const char* name, TestFunction function);
};

int run_all(const std::vector<std::string>& filters);

}  // namespace jitter::test

#define JITTER_TEST(suite_name, case_name)                                              \
    static void jitter_test_##suite_name##_##case_name();                               \
    static const ::jitter::test::Registrar jitter_registrar_##suite_name##_##case_name( \
        #suite_name, #case_name, &jitter_test_##suite_name##_##case_name);              \
    static void jitter_test_##suite_name##_##case_name()

#define CHECK(condition)     static_cast<void>(::jitter::test::check((condition), #condition, __FILE__, __LINE__))

#define CHECK_EQ(left, right)     static_cast<void>(::jitter::test::check_eq((left), (right), #left, #right, __FILE__, __LINE__))

#define CHECK_OK(expression)     static_cast<void>(::jitter::test::check_ok((expression), #expression, __FILE__, __LINE__))

#define CHECK_ERR(expression, expected)                                              \
    static_cast<void>(                                                               \
        ::jitter::test::check_error((expression), (expected), #expression, __FILE__, __LINE__))

#define REQUIRE(condition)                                    \
    do {                                                      \
        if (!::jitter::test::check((condition), #condition, __FILE__, __LINE__)) { \
            return;                                           \
        }                                                     \
    } while (false)

#define REQUIRE_OK(expression)                                                       \
    do {                                                                             \
        if (!::jitter::test::check_ok((expression), #expression, __FILE__, __LINE__)) { \
            return;                                                                  \
        }                                                                            \
    } while (false)
