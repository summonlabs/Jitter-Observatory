// Jitter Observatory - canonical JSON document building.
// Copyright 2026 Summon Software Labs.
#include <jitter/json.hpp>

#include <cmath>

#include <jitter/text.hpp>

namespace jitter {

JsonValue::JsonValue(const JsonValue& other)
    : kind_(other.kind_),
      boolean_(other.boolean_),
      number_(other.number_),
      text_(other.text_),
      array_(other.array_),
      object_(other.object_) {}

JsonValue::JsonValue(JsonValue&& other) noexcept
    : kind_(other.kind_),
      boolean_(other.boolean_),
      number_(other.number_),
      text_(std::move(other.text_)),
      array_(std::move(other.array_)),
      object_(std::move(other.object_)) {
    other.kind_ = Kind::Null;
}

JsonValue& JsonValue::operator=(const JsonValue& other) {
    if (this != &other) {
        kind_ = other.kind_;
        boolean_ = other.boolean_;
        number_ = other.number_;
        text_ = other.text_;
        array_ = other.array_;
        object_ = other.object_;
    }
    return *this;
}

JsonValue& JsonValue::operator=(JsonValue&& other) noexcept {
    if (this != &other) {
        kind_ = other.kind_;
        boolean_ = other.boolean_;
        number_ = other.number_;
        text_ = std::move(other.text_);
        array_ = std::move(other.array_);
        object_ = std::move(other.object_);
        other.kind_ = Kind::Null;
    }
    return *this;
}

JsonValue::~JsonValue() = default;

JsonValue JsonValue::make_object() {
    JsonValue value;
    value.kind_ = Kind::Object;
    return value;
}

JsonValue JsonValue::make_array() {
    JsonValue value;
    value.kind_ = Kind::Array;
    return value;
}

JsonValue JsonValue::make_string(std::string_view text) {
    JsonValue value;
    value.kind_ = Kind::String;
    value.text_ = std::string(text);
    return value;
}

JsonValue JsonValue::make_bool(bool input) {
    JsonValue value;
    value.kind_ = Kind::Boolean;
    value.boolean_ = input;
    return value;
}

JsonValue JsonValue::make_u64(std::uint64_t input) {
    JsonValue value;
    value.kind_ = Kind::Number;
    value.text_ = text::u64_to_string(input);
    return value;
}

JsonValue JsonValue::make_i64(std::int64_t input) {
    JsonValue value;
    value.kind_ = Kind::Number;
    value.text_ = text::i64_to_string(input);
    return value;
}

JsonValue JsonValue::make_double(double input) {
    JsonValue value;
    if (!std::isfinite(input)) {
        // A non finite number has no JSON representation. It is emitted as null so that
        // the document stays parseable, and callers that care must not rely on a number
        // being present.
        value.kind_ = Kind::Null;
        return value;
    }
    value.kind_ = Kind::Number;
    value.text_ = text::double_to_string(input);
    return value;
}

JsonValue JsonValue::make_null() { return JsonValue(); }

bool JsonValue::set(std::string_view key, JsonValue value) {
    if (kind_ != Kind::Object) {
        return false;
    }
    object_[std::string(key)] = std::move(value);
    return true;
}

bool JsonValue::push(JsonValue value) {
    if (kind_ != Kind::Array) {
        return false;
    }
    array_.push_back(std::move(value));
    return true;
}

const JsonValue* JsonValue::member(std::string_view key) const {
    if (kind_ != Kind::Object) {
        return nullptr;
    }
    const auto it = object_.find(std::string(key));
    return it == object_.end() ? nullptr : &it->second;
}

std::size_t JsonValue::size() const noexcept {
    if (kind_ == Kind::Array) {
        return array_.size();
    }
    if (kind_ == Kind::Object) {
        return object_.size();
    }
    return 0;
}

void JsonValue::dump_into(std::string& out, int indent, int depth) const {
    const bool pretty = indent > 0;
    switch (kind_) {
        case Kind::Null:
            out.append("null");
            return;
        case Kind::Boolean:
            out.append(boolean_ ? "true" : "false");
            return;
        case Kind::Number:
            out.append(text_);
            return;
        case Kind::String:
            out.push_back('"');
            out.append(text::escape_json(text_));
            out.push_back('"');
            return;
        case Kind::Array: {
            out.push_back('[');
            bool first = true;
            for (const JsonValue& element : array_) {
                if (!first) {
                    out.push_back(',');
                }
                first = false;
                if (pretty) {
                    out.push_back('\n');
                    out.append(static_cast<std::size_t>((depth + 1) * indent), ' ');
                }
                element.dump_into(out, indent, depth + 1);
            }
            if (pretty && !array_.empty()) {
                out.push_back('\n');
                out.append(static_cast<std::size_t>(depth * indent), ' ');
            }
            out.push_back(']');
            return;
        }
        case Kind::Object: {
            out.push_back('{');
            bool first = true;
            for (const auto& entry : object_) {
                if (!first) {
                    out.push_back(',');
                }
                first = false;
                if (pretty) {
                    out.push_back('\n');
                    out.append(static_cast<std::size_t>((depth + 1) * indent), ' ');
                }
                out.push_back('"');
                out.append(text::escape_json(entry.first));
                out.append("\":");
                if (pretty) {
                    out.push_back(' ');
                }
                entry.second.dump_into(out, indent, depth + 1);
            }
            if (pretty && !object_.empty()) {
                out.push_back('\n');
                out.append(static_cast<std::size_t>(depth * indent), ' ');
            }
            out.push_back('}');
            return;
        }
    }
}

std::string JsonValue::dump() const {
    std::string out;
    dump_into(out, 0, 0);
    return out;
}

std::string JsonValue::dump_pretty() const {
    std::string out;
    dump_into(out, 2, 0);
    return out;
}

Digest JsonValue::digest() const { return Digest::of(dump()); }

}  // namespace jitter
