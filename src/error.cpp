// Jitter Observatory - error code rendering.
// Copyright 2026 Summon Software Labs.
#include <jitter/error.hpp>

namespace jitter {

std::string_view to_string(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::Ok: return "ok";
        case ErrorCode::InvalidArgument: return "invalid_argument";
        case ErrorCode::OutOfRange: return "out_of_range";
        case ErrorCode::Overflow: return "overflow";
        case ErrorCode::NotFound: return "not_found";
        case ErrorCode::Duplicate: return "duplicate";
        case ErrorCode::Conflict: return "conflict";
        case ErrorCode::StaleEpoch: return "stale_epoch";
        case ErrorCode::StaleIncarnation: return "stale_incarnation";
        case ErrorCode::StaleSequence: return "stale_sequence";
        case ErrorCode::StaleGeneration: return "stale_generation";
        case ErrorCode::StaleEvidence: return "stale_evidence";
        case ErrorCode::Incomparable: return "incomparable";
        case ErrorCode::MetricMismatch: return "metric_mismatch";
        case ErrorCode::UnitMismatch: return "unit_mismatch";
        case ErrorCode::Unsupported: return "unsupported";
        case ErrorCode::PolicyViolation: return "policy_violation";
        case ErrorCode::CapacityExceeded: return "capacity_exceeded";
        case ErrorCode::LimitExceeded: return "limit_exceeded";
        case ErrorCode::IntegrityFailure: return "integrity_failure";
        case ErrorCode::VersionUnsupported: return "version_unsupported";
        case ErrorCode::Corrupt: return "corrupt";
        case ErrorCode::Cancelled: return "cancelled";
        case ErrorCode::ShuttingDown: return "shutting_down";
        case ErrorCode::Backpressure: return "backpressure";
        case ErrorCode::ProtocolViolation: return "protocol_violation";
        case ErrorCode::IoFailure: return "io_failure";
        case ErrorCode::NoEvidence: return "no_evidence";
        case ErrorCode::InsufficientEvidence: return "insufficient_evidence";
        case ErrorCode::NotReady: return "not_ready";
        case ErrorCode::Internal: return "internal";
    }
    return "unknown";
}

bool is_staleness_code(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::StaleEpoch:
        case ErrorCode::StaleIncarnation:
        case ErrorCode::StaleSequence:
        case ErrorCode::StaleGeneration:
        case ErrorCode::StaleEvidence:
            return true;
        default:
            return false;
    }
}

std::string Error::to_text() const {
    std::string out;
    out.reserve(message.size() + context.size() + 32);
    out.append(to_string(code));
    out.append(": ");
    out.append(message);
    if (!context.empty()) {
        out.append(" [");
        out.append(context);
        out.append("]");
    }
    return out;
}

}  // namespace jitter
