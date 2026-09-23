// Jitter Observatory - provenance rendering.
// Copyright 2026 Summon Software Labs.
#include <jitter/provenance.hpp>

#include <jitter/text.hpp>

namespace jitter {

std::string_view to_string(EvidenceOrigin origin) noexcept {
    switch (origin) {
        case EvidenceOrigin::Unknown: return "unknown";
        case EvidenceOrigin::Real: return "real";
        case EvidenceOrigin::Synthetic: return "synthetic";
        case EvidenceOrigin::Unsupported: return "unsupported";
    }
    return "unknown";
}

bool parse_evidence_origin(std::string_view value, EvidenceOrigin& out) noexcept {
    if (value == "unknown") { out = EvidenceOrigin::Unknown; return true; }
    if (value == "real") { out = EvidenceOrigin::Real; return true; }
    if (value == "synthetic") { out = EvidenceOrigin::Synthetic; return true; }
    if (value == "unsupported") { out = EvidenceOrigin::Unsupported; return true; }
    return false;
}

std::string_view to_string(SourceAuthority authority) noexcept {
    switch (authority) {
        case SourceAuthority::Unknown: return "unknown";
        case SourceAuthority::Simulated: return "simulated";
        case SourceAuthority::Advisory: return "advisory";
        case SourceAuthority::Corroborating: return "corroborating";
        case SourceAuthority::Authoritative: return "authoritative";
    }
    return "unknown";
}

bool parse_source_authority(std::string_view value, SourceAuthority& out) noexcept {
    if (value == "unknown") { out = SourceAuthority::Unknown; return true; }
    if (value == "simulated") { out = SourceAuthority::Simulated; return true; }
    if (value == "advisory") { out = SourceAuthority::Advisory; return true; }
    if (value == "corroborating") { out = SourceAuthority::Corroborating; return true; }
    if (value == "authoritative") { out = SourceAuthority::Authoritative; return true; }
    return false;
}

std::uint8_t authority_rank(SourceAuthority authority) noexcept {
    return static_cast<std::uint8_t>(authority);
}

template <class Tag>
std::string Counter<Tag>::to_string_value() const {
    return text::u64_to_string(value_);
}

template class Counter<IncarnationTag>;
template class Counter<EpochTag>;
template class Counter<SequenceTag>;
template class Counter<RevisionTag>;
template class Counter<BootTag>;
template class Counter<OrdinalTag>;

std::string Provenance::describe() const {
    std::string out;
    out.append("source=");
    out.append(source.hex());
    out.append(" incarnation=");
    out.append(incarnation.to_string_value());
    out.append(" epoch=");
    out.append(epoch.to_string_value());
    out.append(" sequence=");
    out.append(sequence.to_string_value());
    out.append(" authority=");
    out.append(jitter::to_string(authority));
    out.append(" origin=");
    out.append(jitter::to_string(origin));
    if (!ingest_path.empty()) {
        out.append(" path=");
        out.append(ingest_path);
    }
    return out;
}

}  // namespace jitter
