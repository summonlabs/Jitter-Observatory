// Jitter Observatory - checked arithmetic helpers.
// Copyright 2026 Summon Software Labs.
#include <jitter/checked.hpp>

namespace jitter {

Result<std::uint64_t> add_u64(std::uint64_t a, std::uint64_t b, std::string_view context) {
    std::uint64_t out = 0;
    if (!checked_add_u64(a, b, out)) {
        return Result<std::uint64_t>::fail(ErrorCode::Overflow,
                                           std::string(context) + ": unsigned addition overflow",
                                           "checked_add_u64");
    }
    return out;
}

Result<std::uint64_t> mul_u64(std::uint64_t a, std::uint64_t b, std::string_view context) {
    std::uint64_t out = 0;
    if (!checked_mul_u64(a, b, out)) {
        return Result<std::uint64_t>::fail(ErrorCode::Overflow,
                                           std::string(context) + ": unsigned multiplication overflow",
                                           "checked_mul_u64");
    }
    return out;
}

Result<std::int64_t> add_i64(std::int64_t a, std::int64_t b, std::string_view context) {
    std::int64_t out = 0;
    if (!checked_add_i64(a, b, out)) {
        return Result<std::int64_t>::fail(ErrorCode::Overflow,
                                          std::string(context) + ": signed addition overflow",
                                          "checked_add_i64");
    }
    return out;
}

Result<std::int64_t> sub_i64(std::int64_t a, std::int64_t b, std::string_view context) {
    std::int64_t out = 0;
    if (!checked_sub_i64(a, b, out)) {
        return Result<std::int64_t>::fail(ErrorCode::Overflow,
                                          std::string(context) + ": signed subtraction overflow",
                                          "checked_sub_i64");
    }
    return out;
}

Result<std::size_t> to_size(std::uint64_t value, std::string_view context) {
    std::size_t out = 0;
    if (!checked_to_size(value, out)) {
        return Result<std::size_t>::fail(ErrorCode::OutOfRange,
                                         std::string(context) + ": value does not fit size_t",
                                         "to_size");
    }
    return out;
}

Status require_within(std::uint64_t value, std::uint64_t bound, std::string_view what) {
    if (value > bound) {
        std::string message(what);
        message.append(" is outside the supported bound");
        return Status::failure(ErrorCode::LimitExceeded, std::move(message),
                               std::string("value=") + std::to_string(value) +
                                   " bound=" + std::to_string(bound));
    }
    return Status::success();
}

}  // namespace jitter
