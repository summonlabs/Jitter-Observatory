// Jitter Observatory - locale independent text conversion helpers.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace jitter::text {

std::string u64_to_string(std::uint64_t value);
std::string i64_to_string(std::int64_t value);
std::string u32_to_string(std::uint32_t value);

// Shortest round-trip representation of a finite double. Non-finite values render as
// "nan", "inf" or "-inf" and are rejected by the JSON writer.
std::string double_to_string(double value);

std::optional<std::uint64_t> parse_u64(std::string_view in);
std::optional<std::int64_t> parse_i64(std::string_view in);
std::optional<std::uint32_t> parse_u32(std::string_view in);
std::optional<double> parse_double(std::string_view in);
std::optional<bool> parse_bool(std::string_view in);

std::string hex_encode(std::span<const std::uint8_t> bytes);
std::optional<std::uint8_t> hex_nibble(char c);

std::string_view trim(std::string_view in);
std::vector<std::string_view> split(std::string_view in, char separator);
bool equals_ascii_ci(std::string_view a, std::string_view b);
bool starts_with(std::string_view value, std::string_view prefix);
bool ends_with(std::string_view value, std::string_view suffix);
std::string join(const std::vector<std::string>& parts, std::string_view separator);

// Escapes control characters and quotes so the result is safe inside a JSON string
// literal (the surrounding quotes are not added).
std::string escape_json(std::string_view in);

}  // namespace jitter::text
