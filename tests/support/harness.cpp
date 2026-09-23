// Jitter Observatory - first party test harness.
// Copyright 2026 Summon Software Labs.
#include "support/harness.hpp"

#include <algorithm>

namespace jitter::test {

Context& Context::current() {
    static Context instance;
    return instance;
}

void Context::record(const char* file, int line, std::string expression, std::string detail) {
    Failure failure;
    failure.file = file;
    failure.line = line;
    failure.expression = std::move(expression);
    failure.detail = std::move(detail);
    failures_.push_back(std::move(failure));
    std::cout << "    FAIL " << file << ":" << line << " " << failures_.back().expression << " | "
              << failures_.back().detail << "\n" << std::flush;
}

void Context::reset() { failures_.clear(); }

bool check(bool condition, const char* expression, const char* file, int line) {
    if (condition) {
        return true;
    }
    Context::current().record(file, line, expression, "condition is false");
    return false;
}

bool check_ok(const Status& status, const char* expression, const char* file, int line) {
    if (status.ok()) {
        return true;
    }
    Context::current().record(file, line, expression,
                              "unexpected failure: " + status.error().to_text());
    return false;
}

bool check_error(const Status& status, ErrorCode expected, const char* expression, const char* file,
                 int line) {
    if (!status.ok() && status.code() == expected) {
        return true;
    }
    std::ostringstream detail;
    detail << "expected " << to_string(expected) << " but got ";
    if (status.ok()) {
        detail << "success";
    } else {
        detail << status.error().to_text();
    }
    Context::current().record(file, line, expression, detail.str());
    return false;
}

Registry& Registry::instance() {
    static Registry instance;
    return instance;
}

void Registry::add(std::string suite, std::string name, TestFunction function) {
    cases_.push_back(TestCase{std::move(suite), std::move(name), std::move(function)});
}

Registrar::Registrar(const char* suite, const char* name, TestFunction function) {
    Registry::instance().add(suite, name, std::move(function));
}

int run_all(const std::vector<std::string>& filters) {
    const std::vector<TestCase>& cases = Registry::instance().cases();
    std::size_t passed = 0;
    std::size_t failed = 0;
    std::size_t skipped = 0;

    for (const TestCase& test_case : cases) {
        const std::string full_name = test_case.suite + "." + test_case.name;
        if (!filters.empty()) {
            bool matched = false;
            for (const std::string& filter : filters) {
                if (full_name.find(filter) != std::string::npos) {
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                ++skipped;
                continue;
            }
        }

        Context::current().reset();
        std::cout << "[ RUN  ] " << full_name << "\n" << std::flush;
        test_case.function();
        if (Context::current().clean()) {
            ++passed;
            std::cout << "[  OK  ] " << full_name << "\n" << std::flush;
        } else {
            ++failed;
            std::cout << "[ FAIL ] " << full_name << " (" << Context::current().failure_count()
                      << " assertion failures)\n" << std::flush;
        }
    }

    std::cout << "\ntests: " << (passed + failed) << " passed: " << passed << " failed: " << failed
              << " filtered_out: " << skipped << "\n" << std::flush;
    if (failed > 127) {
        return 127;
    }
    return static_cast<int>(failed);
}

}  // namespace jitter::test

int main(int argc, char** argv) {
    std::vector<std::string> filters;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--list") {
            for (const jitter::test::TestCase& test_case : jitter::test::Registry::instance().cases()) {
                std::cout << test_case.suite << "." << test_case.name << "\n" << std::flush;
            }
            return 0;
        }
        filters.push_back(argument);
    }
    return jitter::test::run_all(filters);
}
