// Jitter Observatory - paths, hops and route generations.
// Copyright 2026 Summon Software Labs.
#include <jitter/path.hpp>

#include <algorithm>

#include <jitter/bytes.hpp>
#include <jitter/text.hpp>

namespace jitter {
namespace {

constexpr std::uint64_t kMaxPathNameBytes = 128;
constexpr std::uint64_t kMaxHopNameBytes = 128;
constexpr std::uint64_t kMaxDeviceNoteBytes = 256;
constexpr std::uint64_t kMaxPathDescriptionBytes = 512;

}  // namespace

HopId make_hop_id(const HopDescriptor& descriptor, PathId path) {
    DigestBuilder builder(kDomainHop);
    builder.id(path);
    builder.u16(descriptor.index);
    builder.str(descriptor.name);
    builder.optional_i64(descriptor.timing_clock.has_value()
                             ? std::optional<std::int64_t>(0)
                             : std::optional<std::int64_t>());
    if (descriptor.timing_clock.has_value()) {
        builder.id(descriptor.timing_clock.value());
    }
    builder.u8(static_cast<std::uint8_t>(descriptor.origin));
    builder.str(descriptor.device_note);
    return builder.as_id<HopTag>();
}

PathId make_path_id(const PathDescriptor& descriptor) {
    // The identity of a path is its stable name, not its momentary route. Route
    // changes are expressed as generations of the same path.
    DigestBuilder builder(kDomainPath);
    builder.str(descriptor.name);
    builder.u8(static_cast<std::uint8_t>(descriptor.origin));
    builder.str(descriptor.description);
    return builder.as_id<PathTag>();
}

Status validate_path(const PathDescriptor& descriptor) {
    if (descriptor.name.empty()) {
        return Status::failure(ErrorCode::InvalidArgument, "path name must not be empty");
    }
    if (descriptor.name.size() > kMaxPathNameBytes) {
        return Status::failure(ErrorCode::LimitExceeded, "path name is too long");
    }
    if (descriptor.description.size() > kMaxPathDescriptionBytes) {
        return Status::failure(ErrorCode::LimitExceeded, "path description is too long");
    }
    if (descriptor.hops.size() > Limits::kMaxHopsPerPath) {
        return Status::failure(ErrorCode::LimitExceeded, "path declares more hops than supported",
                               std::to_string(descriptor.hops.size()));
    }
    for (std::size_t i = 0; i < descriptor.hops.size(); ++i) {
        const HopDescriptor& hop = descriptor.hops[i];
        if (hop.index != i) {
            return Status::failure(ErrorCode::InvalidArgument,
                                   "hop index must equal its position on the path",
                                   "position=" + std::to_string(i) +
                                       " index=" + std::to_string(hop.index));
        }
        if (hop.name.empty()) {
            return Status::failure(ErrorCode::InvalidArgument, "hop name must not be empty");
        }
        if (hop.name.size() > kMaxHopNameBytes) {
            return Status::failure(ErrorCode::LimitExceeded, "hop name is too long");
        }
        if (hop.device_note.size() > kMaxDeviceNoteBytes) {
            return Status::failure(ErrorCode::LimitExceeded, "hop device note is too long");
        }
    }
    const PathId derived = make_path_id(descriptor);
    if (!descriptor.id.is_nil() && descriptor.id != derived) {
        return Status::failure(ErrorCode::InvalidArgument,
                               "path id does not match its content addressed identity",
                               "declared=" + descriptor.id.hex() + " derived=" + derived.hex());
    }
    return Status::success();
}

PathTopology make_topology(const std::vector<HopId>& hops) {
    DigestBuilder builder(kDomainTopology);
    builder.u32(static_cast<std::uint32_t>(hops.size()));
    for (const HopId& hop : hops) {
        builder.id(hop);
    }
    PathTopology topology;
    topology.digest = builder.digest();
    topology.hops = hops;
    return topology;
}

PathTopology make_topology(const PathDescriptor& descriptor) {
    std::vector<HopId> hops;
    hops.reserve(descriptor.hops.size());
    const PathId path = descriptor.id.is_nil() ? make_path_id(descriptor) : descriptor.id;
    for (const HopDescriptor& hop : descriptor.hops) {
        hops.push_back(make_hop_id(hop, path));
    }
    return make_topology(hops);
}

GenerationId make_generation_id(PathId path, const Ordinal& ordinal, const Digest& topology) {
    DigestBuilder builder(kDomainGeneration);
    builder.id(path);
    builder.u64(ordinal.value());
    builder.raw(std::span<const std::uint8_t>(topology.data(), Digest::kBytes));
    return builder.as_id<GenerationTag>();
}

Status PathCatalog::register_path(PathDescriptor& descriptor) {
    return register_path_impl(descriptor, true);
}

Status PathCatalog::register_path_only(PathDescriptor& descriptor) {
    return register_path_impl(descriptor, false);
}

Status PathCatalog::register_path_impl(PathDescriptor& descriptor, bool open_generation) {
    if (descriptor.id.is_nil()) {
        descriptor.id = make_path_id(descriptor);
    }
    for (HopDescriptor& hop : descriptor.hops) {
        if (hop.id.is_nil()) {
            hop.id = make_hop_id(hop, descriptor.id);
        }
    }
    JITTER_TRY(validate_path(descriptor));

    const auto existing = paths_.find(descriptor.id);
    if (existing != paths_.end()) {
        if (existing->second.hops.empty() && !descriptor.hops.empty()) {
            existing->second = descriptor;
            if (open_generation) {
                const PathTopology topology = make_topology(descriptor);
                auto outcome = observe_topology(descriptor.id, topology, 0, "path_registered");
                if (!outcome.ok()) {
                    return outcome.error();
                }
            }
            return Status::success();
        }
        descriptor = existing->second;
        return Status::success();
    }
    if (paths_.size() >= Limits::kMaxPaths) {
        return Status::failure(ErrorCode::CapacityExceeded, "path catalog is full",
                               std::to_string(paths_.size()));
    }
    paths_.emplace(descriptor.id, descriptor);

    if (open_generation && !descriptor.hops.empty()) {
        const PathTopology topology = make_topology(descriptor);
        auto outcome = observe_topology(descriptor.id, topology, 0, "path_registered");
        if (!outcome.ok()) {
            return outcome.error();
        }
    }
    return Status::success();
}

const PathDescriptor* PathCatalog::find(PathId id) const noexcept {
    const auto it = paths_.find(id);
    return it == paths_.end() ? nullptr : &it->second;
}

const HopDescriptor* PathCatalog::find_hop(PathId path, HopId hop) const noexcept {
    const PathDescriptor* descriptor = find(path);
    if (descriptor == nullptr) {
        return nullptr;
    }
    for (const HopDescriptor& candidate : descriptor->hops) {
        if (make_hop_id(candidate, path) == hop) {
            return &candidate;
        }
    }
    return nullptr;
}

std::vector<PathDescriptor> PathCatalog::paths() const {
    std::vector<PathDescriptor> copy;
    copy.reserve(paths_.size());
    for (const auto& entry : paths_) {
        copy.push_back(entry.second);
    }
    return copy;
}

Result<GenerationOutcome> PathCatalog::observe_topology(PathId path, const PathTopology& topology,
                                                        std::int64_t now_utc_ns,
                                                        std::string_view cause) {
    const PathDescriptor* descriptor = find(path);
    if (descriptor == nullptr) {
        return Result<GenerationOutcome>::fail(ErrorCode::NotFound,
                                               "cannot open a generation for an unregistered path",
                                               path.hex());
    }
    if (topology.hops.size() > Limits::kMaxHopsPerPath) {
        return Result<GenerationOutcome>::fail(ErrorCode::LimitExceeded,
                                               "topology declares more hops than supported");
    }
    std::vector<PathGeneration>& history = generations_[path];
    if (!history.empty()) {
        PathGeneration& current = history.back();
        if (current.topology == topology.digest) {
            GenerationOutcome outcome;
            outcome.generation = current;
            outcome.created_new = false;
            outcome.reason = "topology_unchanged";
            return outcome;
        }
        if (current.is_open()) {
            current.closed_at_utc_ns = now_utc_ns < current.opened_at_utc_ns ? current.opened_at_utc_ns
                                                                             : now_utc_ns;
            current.close_reason = "topology_changed";
        }
    }

    PathGeneration generation;
    generation.path = path;
    Ordinal ordinal = history.empty() ? Ordinal(1u) : history.back().ordinal.next();
    generation.ordinal = ordinal;
    generation.topology = topology.digest;
    generation.opened_at_utc_ns = now_utc_ns;
    generation.hop_count = static_cast<std::uint32_t>(topology.hops.size());
    generation.cause = std::string(cause);
    generation.id = make_generation_id(path, ordinal, topology.digest);

    history.push_back(generation);
    while (history.size() > Limits::kMaxGenerationHistoryPerPath) {
        // Bounded retention. Dropped generations are counted by identity in exports;
        // they are never silently rewound, and ordinals never repeat.
        history.erase(history.begin());
    }

    GenerationOutcome outcome;
    outcome.generation = generation;
    outcome.created_new = true;
    outcome.reason = std::string(cause);
    return outcome;
}

Status PathCatalog::bump_revision(PathId path, std::string_view cause) {
    const auto it = generations_.find(path);
    if (it == generations_.end() || it->second.empty()) {
        return Status::failure(ErrorCode::NotFound, "cannot revise a path without a generation",
                               path.hex());
    }
    PathGeneration& generation = it->second.back();
    generation.revision = generation.revision.next();
    generation.cause = std::string(cause);
    return Status::success();
}

Status PathCatalog::close_current(PathId path, std::int64_t now_utc_ns, std::string_view reason) {
    const auto it = generations_.find(path);
    if (it == generations_.end() || it->second.empty()) {
        return Status::failure(ErrorCode::NotFound, "cannot close a path without a generation",
                               path.hex());
    }
    PathGeneration& generation = it->second.back();
    if (generation.is_open()) {
        generation.closed_at_utc_ns = now_utc_ns;
        generation.close_reason = std::string(reason);
    }
    return Status::success();
}

Status PathCatalog::import_generation(const PathGeneration& generation) {
    if (find(generation.path) == nullptr) {
        return Status::failure(ErrorCode::NotFound,
                               "cannot import a generation for an unregistered path",
                               generation.path.hex());
    }
    if (generation.ordinal.is_unset()) {
        return Status::failure(ErrorCode::InvalidArgument,
                               "imported generation must carry a non zero ordinal");
    }
    std::vector<PathGeneration>& history = generations_[generation.path];
    if (!history.empty() && generation.ordinal <= history.back().ordinal) {
        return Status::failure(ErrorCode::Conflict,
                               "imported generation would rewind or repeat a path ordinal",
                               generation.id.hex());
    }
    history.push_back(generation);
    while (history.size() > Limits::kMaxGenerationHistoryPerPath) {
        history.erase(history.begin());
    }
    return Status::success();
}

const PathGeneration* PathCatalog::current(PathId path) const noexcept {
    const auto it = generations_.find(path);
    if (it == generations_.end() || it->second.empty()) {
        return nullptr;
    }
    return &it->second.back();
}

const PathGeneration* PathCatalog::find_generation(GenerationId id) const noexcept {
    for (const auto& entry : generations_) {
        for (const PathGeneration& generation : entry.second) {
            if (generation.id == id) {
                return &generation;
            }
        }
    }
    return nullptr;
}

std::vector<PathGeneration> PathCatalog::generations(PathId path) const {
    const auto it = generations_.find(path);
    if (it == generations_.end()) {
        return {};
    }
    return it->second;
}

std::vector<PathGeneration> PathCatalog::all_generations() const {
    std::vector<PathGeneration> all;
    for (const auto& entry : generations_) {
        all.insert(all.end(), entry.second.begin(), entry.second.end());
    }
    std::sort(all.begin(), all.end(), [](const PathGeneration& a, const PathGeneration& b) {
        if (a.path != b.path) {
            return a.path < b.path;
        }
        return a.ordinal < b.ordinal;
    });
    return all;
}

bool PathCatalog::is_current(GenerationId id) const noexcept {
    const PathGeneration* generation = find_generation(id);
    return generation != nullptr && generation->is_open();
}

std::size_t PathCatalog::generation_count() const noexcept {
    std::size_t total = 0;
    for (const auto& entry : generations_) {
        total += entry.second.size();
    }
    return total;
}

}  // namespace jitter
