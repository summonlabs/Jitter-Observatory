// Jitter Observatory - locale independent text conversion helpers.
// Copyright 2026 Summon Software Labs.
#include <jitter/text.hpp>

#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <system_error>

namespace jitter::text {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

char lower_ascii(char c) {
    if (c >= 'A' && c <= 'Z') {
        return static_cast<char>(c - 'A' + 'a');
    }
    return c;
}

}  // namespace

std::string u64_to_string(std::uint64_t value) {
    std::array<char, 24> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return std::string(buffer.data(), static_cast<std::size_t>(result.ptr - buffer.data()));
}

std::string i64_to_string(std::int64_t value) {
    std::array<char, 24> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return std::string(buffer.data(), static_cast<std::size_t>(result.ptr - buffer.data()));
}

std::string u32_to_string(std::uint32_t value) { return u64_to_string(value); }

std::string double_to_string(double value) {
    if (std::isnan(value)) {
        return "nan";
    }
    if (std::isinf(value)) {
        return value > 0.0 ? "inf" : "-inf";
    }
    std::array<char, 64> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (result.ec != std::errc{}) {
        return "nan";
    }
    return std::string(buffer.data(), static_cast<std::size_t>(result.ptr - buffer.data()));
}

std::optional<std::uint64_t> parse_u64(std::string_view in) {
    if (in.empty()) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    const auto result = std::from_chars(in.data(), in.data() + in.size(), value);
    if (result.ec != std::errc{} || result.ptr != in.data() + in.size()) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::int64_t> parse_i64(std::string_view in) {
    if (in.empty()) {
        return std::nullopt;
    }
    std::int64_t value = 0;
    const auto result = std::from_chars(in.data(), in.data() + in.size(), value);
    if (result.ec != std::errc{} || result.ptr != in.data() + in.size()) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::uint32_t> parse_u32(std::string_view in) {
    const auto value = parse_u64(in);
    if (!value.has_value() || value.value() > 0xFFFFFFFFull) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(value.value());
}

std::optional<double> parse_double(std::string_view in) {
    if (in.empty()) {
        return std::nullopt;
    }
    double value = 0.0;
    const auto result = std::from_chars(in.data(), in.data() + in.size(), value);
    if (result.ec != std::errc{} || result.ptr != in.data() + in.size()) {
        return std::nullopt;
    }
    return value;
}

std::optional<bool> parse_bool(std::string_view in) {
    if (in == "true" || in == "1") {
        return true;
    }
    if (in == "false" || in == "0") {
        return false;
    }
    return std::nullopt;
}

std::string hex_encode(std::span<const std::uint8_t> bytes) {
    std::string out;
    out.resize(bytes.size() * 2);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        out[i * 2] = kHexDigits[(bytes[i] >> 4) & 0x0fu];
        out[(i * 2) + 1] = kHexDigits[bytes[i] & 0x0fu];
    }
    return out;
}

std::optional<std::uint8_t> hex_nibble(char c) {
    if (c >= '0' && c <= '9') {
        return static_cast<std::uint8_t>(c - '0');
    }
    const char lowered = lower_ascii(c);
    if (lowered >= 'a' && lowered <= 'f') {
        return static_cast<std::uint8_t>(lowered - 'a' + 10);
    }
    return std::nullopt;
}

std::string_view trim(std::string_view in) {
    std::size_t begin = 0;
    std::size_t end = in.size();
    while (begin < end && (in[begin] == ' ' || in[begin] == '\t' || in[begin] == '\r' || in[begin] == '\n')) {
        ++begin;
    }
    while (end > begin && (in[end - 1] == ' ' || in[end - 1] == '\t' || in[end - 1] == '\r' || in[end - 1] == '\n')) {
        --end;
    }
    return in.substr(begin, end - begin);
}

std::vector<std::string_view> split(std::string_view in, char separator) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (start <= in.size()) {
        const std::size_t next = in.find(separator, start);
        if (next == std::string_view::npos) {
            parts.push_back(in.substr(start));
            break;
        }
        parts.push_back(in.substr(start, next - start));
        start = next + 1;
    }
    return parts;
}

bool equals_ascii_ci(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lower_ascii(a[i]) != lower_ascii(b[i])) {
            return false;
        }
    }
    return true;
}

bool starts_with(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool ends_with(std::string_view value, std::string_view suffix) {
    return value.size() >= suffix.size() && value.substr(value.size() - suffix.size()) == suffix;
}

std::string join(const std::vector<std::string>& parts, std::string_view separator) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) {
            out.append(separator);
        }
        out.append(parts[i]);
    }
    return out;
}

std::string escape_json(std::string_view in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (const char c : in) {
        const auto byte = static_cast<unsigned char>(c);
        switch (c) {
            case '"': out.append("\\\""); break;
            case '\\': out.append("\\\\"); break;
            case '\b': out.append("\\b"); break;
            case '\f': out.append("\\f"); break;
            case '\n': out.append("\\n"); break;
            case '\r': out.append("\\r"); break;
            case '\t': out.append("\\t"); break;
            default:
                if (byte < 0x20u) {
                    std::array<char, 8> buffer{};
                    const int written = std::snprintf(buffer.data(), buffer.size(), "\\u%04x",
                                                      static_cast<unsigned>(byte));
                    if (written > 0) {
                        out.append(buffer.data(), static_cast<std::size_t>(written));
                    }
                } else {
                    out.push_back(c);
                }
                break;
        }
    }
    return out;
}

}  // namespace jitter::text
