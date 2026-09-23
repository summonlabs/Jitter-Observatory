// Jitter Observatory - paths, hops and route generations.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <jitter/error.hpp>
#include <jitter/hash.hpp>
#include <jitter/id.hpp>
#include <jitter/limits.hpp>
#include <jitter/provenance.hpp>
#include <jitter/time.hpp>

namespace jitter {

struct HopDescriptor {
    HopId id;
    // Position of the hop on the path, starting at zero. The descriptor is rejected
    // when the index does not equal the position in the path.
    std::uint16_t index = 0;
    std::string name;
    // Clock domain used by this hop's own timestamps. It is optional on purpose:
    // a hop that emits no timing is a hop that cannot be attributed.
    std::optional<ClockDomainId> timing_clock;
    // Declared by the source. Interpreted only as a declaration, never as a verified
    // hardware property of this runtime.
    EvidenceOrigin origin = EvidenceOrigin::Unknown;
    std::string device_note;
};

struct PathDescriptor {
    PathId id;
    std::string name;
    std::vector<HopDescriptor> hops;
    EvidenceOrigin origin = EvidenceOrigin::Unknown;
    std::string description;
};

PathId make_path_id(const PathDescriptor& descriptor);
HopId make_hop_id(const HopDescriptor& descriptor, PathId path);
Status validate_path(const PathDescriptor& descriptor);

// The ordered hop sequence that defines a route. A change to this digest is what
// segments history; nothing else does.
struct PathTopology {
    Digest digest;
    std::vector<HopId> hops;

    bool operator==(const PathTopology& other) const noexcept { return digest == other.digest; }
    bool operator!=(const PathTopology& other) const noexcept { return !(*this == other); }
};

PathTopology make_topology(const PathDescriptor& descriptor);
PathTopology make_topology(const std::vector<HopId>& hops);

struct PathGeneration {
    GenerationId id;
    PathId path;
    // Strictly increasing per path, never reused and never rewound.
    Ordinal ordinal;
    Digest topology;
    // Revision of generation metadata (for example a hop's declared clock domain).
    // A revision change never creates a new generation and never re-segments history.
    Revision revision;
    std::int64_t opened_at_utc_ns = 0;
    std::optional<std::int64_t> closed_at_utc_ns;
    std::uint32_t hop_count = 0;
    std::string cause;         // why this generation was opened
    std::string close_reason;  // why it was closed, empty while open

    bool is_open() const noexcept { return !closed_at_utc_ns.has_value(); }
};

GenerationId make_generation_id(PathId path, const Ordinal& ordinal, const Digest& topology);

struct GenerationOutcome {
    PathGeneration generation;
    bool created_new = false;
    std::string reason;
};

class PathCatalog {
public:
    explicit PathCatalog(const ClockModel& clocks) : clocks_(&clocks) {}

    // Registers a path, fills in its identity and its hops' identities when the caller
    // left them nil, and opens the first generation for the declared topology.
    Status register_path(PathDescriptor& descriptor);

    // Registers a path without opening a generation. Recovery uses it because the
    // generations themselves arrive as records and must be imported in order.
    Status register_path_only(PathDescriptor& descriptor);
    const PathDescriptor* find(PathId id) const noexcept;
    const HopDescriptor* find_hop(PathId path, HopId hop) const noexcept;
    bool has(PathId id) const noexcept { return find(id) != nullptr; }
    std::size_t size() const noexcept { return paths_.size(); }
    std::vector<PathDescriptor> paths() const;

    // Records an observed topology. An unchanged topology returns the existing
    // generation; a changed topology closes the current generation and opens the next
    // ordinal. History is therefore segmented by route change and by nothing else.
    Result<GenerationOutcome> observe_topology(PathId path, const PathTopology& topology,
                                               std::int64_t now_utc_ns, std::string_view cause);

    Status bump_revision(PathId path, std::string_view cause);

    Status close_current(PathId path, std::int64_t now_utc_ns, std::string_view reason);

    const PathGeneration* current(PathId path) const noexcept;
    const PathGeneration* find_generation(GenerationId id) const noexcept;
    std::vector<PathGeneration> generations(PathId path) const;
    std::vector<PathGeneration> all_generations() const;

    // Imports a persisted generation during recovery. The ordinal order is preserved
    // and a generation is never renumbered.
    Status import_generation(const PathGeneration& generation);

    // True when the generation is still the open generation of its path.
    bool is_current(GenerationId id) const noexcept;

    std::size_t generation_count() const noexcept;

private:
    Status register_path_impl(PathDescriptor& descriptor, bool open_generation);

    const ClockModel* clocks_;
    std::map<PathId, PathDescriptor> paths_;
    std::map<PathId, std::vector<PathGeneration>> generations_;
};

}  // namespace jitter
