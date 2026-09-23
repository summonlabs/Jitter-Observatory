// Jitter Observatory - helpers for driving real child processes from tests.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#if !defined(_WIN32)
#include <sys/wait.h>
#endif

namespace jitter::test {

inline std::string environment_value(const std::string& name) {
#if defined(_WIN32)
    char* buffer = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&buffer, &size, name.c_str()) == 0 && buffer != nullptr) {
        std::string value(buffer);
        std::free(buffer);
        return value;
    }
    return {};
#else
    const char* value = std::getenv(name.c_str());
    return value == nullptr ? std::string{} : std::string(value);
#endif
}

inline std::string quote_argument(const std::string& value) { return "\"" + value + "\""; }

// Runs a command line and returns the child's exit code. This is deliberately
// synchronous: the test never polls and never waits on a timer.
//
// On Windows the command is wrapped in one extra pair of quotes. cmd.exe strips the
// first and last quote of a command line that starts with a quote, which corrupts a
// line that also contains a redirection unless it is wrapped.
inline int run_command(const std::string& command) {
#if defined(_WIN32)
    const std::string wrapped = "\"" + command + "\"";
    const int raw = std::system(wrapped.c_str());
#else
    const int raw = std::system(command.c_str());
#endif
#if defined(_WIN32)
    return raw;
#else
    if (raw == -1) {
        return -1;
    }
    return WIFEXITED(raw) ? WEXITSTATUS(raw) : -1;
#endif
}

inline std::string read_text_file(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

inline std::string scratch_directory(const std::string& leaf) {
    const std::filesystem::path base =
        std::filesystem::temp_directory_path() / "jitter-observatory-tests" / leaf;
    std::error_code error;
    std::filesystem::create_directories(base, error);
    return base.string();
}

// Extracts the value of a key from a document produced by the canonical writer. The
// value is returned without its surrounding quotes when it is a string, and verbatim
// when it is a number or a boolean. Whitespace around the value is trimmed, so both
// compact and indented output can be read. The tests use this instead of a full parser
// so that they stay dependency free.
inline std::string find_json_token(const std::string& document, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    const std::size_t start = document.find(needle);
    if (start == std::string::npos) {
        return {};
    }
    std::size_t cursor = start + needle.size();
    while (cursor < document.size() && (document[cursor] == ' ' || document[cursor] == ':')) {
        ++cursor;
    }
    const bool quoted = cursor < document.size() && document[cursor] == '"';
    if (quoted) {
        ++cursor;
    }
    const std::size_t value_start = cursor;
    while (cursor < document.size()) {
        const char current = document[cursor];
        if (quoted) {
            if (current == '"') {
                break;
            }
        } else if (current == ',' || current == '}' || current == '\n' || current == '\r' ||
                   current == ' ') {
            break;
        }
        ++cursor;
    }
    return document.substr(value_start, cursor - value_start);
}

inline std::string find_json_string(const std::string& document, const std::string& key) {
    return find_json_token(document, key);
}

inline std::string find_json_number(const std::string& document, const std::string& key) {
    return find_json_token(document, key);
}

}  // namespace jitter::test
