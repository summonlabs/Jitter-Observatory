// Jitter Observatory - canonical JSON document building.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <jitter/error.hpp>
#include <jitter/hash.hpp>

namespace jitter {

// A JSON value tree with canonical serialisation: object members are always emitted in
// ascending key order and numbers are emitted with the shortest round-trip
// representation. Two trees built in different orders serialise to identical bytes,
// which is what makes exports comparable and hashable.
class JsonValue {
public:
    enum class Kind : std::uint8_t { Null = 0, Boolean = 1, Number = 2, String = 3, Array = 4, Object = 5 };

    JsonValue() = default;
    JsonValue(const JsonValue& other);
    JsonValue(JsonValue&& other) noexcept;
    JsonValue& operator=(const JsonValue& other);
    JsonValue& operator=(JsonValue&& other) noexcept;
    ~JsonValue();

    static JsonValue make_object();
    static JsonValue make_array();
    static JsonValue make_string(std::string_view value);
    static JsonValue make_bool(bool value);
    static JsonValue make_u64(std::uint64_t value);
    static JsonValue make_i64(std::int64_t value);
    static JsonValue make_double(double value);
    static JsonValue make_null();

    Kind kind() const noexcept { return kind_; }
    bool is_null() const noexcept { return kind_ == Kind::Null; }
    bool is_object() const noexcept { return kind_ == Kind::Object; }
    bool is_array() const noexcept { return kind_ == Kind::Array; }

    // Object member. Returns false when this value is not an object.
    bool set(std::string_view key, JsonValue value);
    // Array element. Returns false when this value is not an array.
    bool push(JsonValue value);

    const JsonValue* member(std::string_view key) const;
    std::size_t size() const noexcept;

    // Canonical compact form: no insignificant whitespace, sorted keys.
    std::string dump() const;
    // Canonical form with two space indentation, for human inspection only.
    std::string dump_pretty() const;

    Digest digest() const;

private:
    void dump_into(std::string& out, int indent, int depth) const;

    Kind kind_ = Kind::Null;
    bool boolean_ = false;
    double number_ = 0.0;
    std::string text_;
    std::vector<JsonValue> array_;
    std::map<std::string, JsonValue> object_;
};

}  // namespace jitter
