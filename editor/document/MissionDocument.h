#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
#include <unordered_map>

namespace editor::document {

inline constexpr std::size_t fitByteLimit = 16 * 1024 * 1024;
inline constexpr std::size_t pakByteLimit = 128 * 1024 * 1024;
inline constexpr std::size_t packetByteLimit = 64 * 1024 * 1024;
inline constexpr std::size_t decodedPakByteLimit = 256 * 1024 * 1024;
inline constexpr std::size_t textByteLimit = 1024 * 1024;
inline constexpr std::size_t nodeLimit = 250000;
inline constexpr std::size_t nestingLimit = 32;

struct NodeId {
    std::uint64_t value{};
    auto operator<=>(const NodeId&) const = default;
};
using Scalar = std::variant<std::int64_t, double, bool, std::string>;
using Value = std::vector<Scalar>;
struct Span { std::size_t begin{}, end{}; };
struct LoadError { std::string field, reason; std::size_t offset{}; };
using Diagnostic = std::function<void(const std::string&)>;

enum class SectionKind { settings, terrain, entity, team, player, objective, brain, camera, environment, extension };
struct Field {
    NodeId id;
    std::string name, prefix;
    Span syntax;
    Value original, value;
    bool opaque{};
};
struct Section {
    NodeId id;
    std::optional<NodeId> parent;
    std::string name;
    SectionKind kind = SectionKind::extension;
    std::vector<NodeId> fields;
    Span syntax, nameSyntax;
};
struct Syntax {
    std::string originalBytes;
    std::vector<Section> sections;
    std::vector<Field> fields;
    std::size_t endMarker{};
};
struct Packet {
    std::uint8_t storage{};
    Span encoded;
    std::vector<std::byte> decoded;
};
struct PacketArchive {
    std::vector<std::byte> originalBytes;
    std::vector<Packet> packets;
};
struct TerrainTile {
    std::array<float, 3> normal{};
    float elevation{};
    std::uint32_t texture{}, light{}, material{};
    std::array<std::uint8_t, 4> flags{};
};
struct EntityId {
    std::uint64_t value{};
    auto operator<=>(const EntityId&) const = default;
};
struct Entity {
    EntityId id;
    std::optional<std::uint32_t> exportedPart;
    std::optional<NodeId> section;
    std::optional<std::size_t> packetRecord;
    std::uint32_t objectNumber{};
    std::array<double, 3> position{};
    double rotation{};
    std::int64_t team{};
    std::optional<std::uint32_t> controlType, controlDataType;
    std::optional<std::string> objectProfile, csvFile;
    std::optional<NodeId> brain;
    std::optional<NodeId> warrior;
};

struct Transform {
    std::array<double, 3> position{};
    double rotation{};
};
struct UnitPlacement {
    std::uint32_t objectNumber{}, controlType = 2, controlDataType = 1;
    Transform transform;
    std::int64_t team{};
    std::int32_t commander{};
    bool playerPart{};
    std::string objectProfile, csvFile, warriorProfile, legacyBrain;
    std::uint32_t variant{}, baseColor = 0x00ffffff, highlightColor1 = 0x00c0c0c0,
        highlightColor2 = 0x00808080;
    std::int32_t damage{};
    bool selfRepair = true;
    bool writeElevation = true;
    std::optional<std::uint32_t> squad;
    struct Cell { std::int32_t number{}, type{}; double value{}; };
    std::vector<Cell> cells;
    std::int32_t staticVars{};
};
struct BuildingPlacement {
    std::uint32_t objectNumber{};
    Transform transform;
    std::int32_t team{}, damage{};
    std::uint32_t parentCell = 0xffffffffU;
    std::int32_t forestInstance = -1, scale{};
    std::optional<double> markerRadius;
};
enum class ConditionSpecies : std::uint8_t {
    destroyAll, destroyNumber, destroyGroup, destroyUnit, destroyStructure,
    captureOrDestroyAll, captureOrDestroyNumber, captureOrDestroyGroup, captureOrDestroyUnit, captureOrDestroyStructure,
    deadOrFledAll, deadOrFledNumber, deadOrFledGroup, deadOrFledUnit,
    captureUnit, captureStructure, guardUnit, guardStructure,
    moveAny, moveAll, moveSurvivors, moveMechs, flag, elapsed
};
enum class ActionSpecies : std::uint8_t { bik, wav, text, resourceText, flag, technology };
struct ObjectiveEntry { NodeId section; std::variant<ConditionSpecies, ActionSpecies> species; };
struct Objective {
    NodeId section;
    std::uint32_t team{}, exportedIndex{};
    std::array<std::vector<ObjectiveEntry>, 4> entries;
};

// Reversible authored state excludes terrain caches and unchanged encoded packets.
struct StructuralState {
    Syntax syntax;
    std::vector<Entity> entities;
    std::vector<Objective> objectives;
    std::vector<std::byte> placements;
};

inline std::string identifier(std::string_view text) {
    std::string result(text);
    for (auto& c : result) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return result;
}

std::expected<Syntax, LoadError> decodeFit(std::string bytes);
std::expected<PacketArchive, LoadError> decodePak(std::vector<std::byte> bytes);
std::expected<std::vector<std::byte>, LoadError> encodePlacements(
    const PacketArchive& source, std::span<const std::byte> placements);
std::string formatValue(const Field& field, const Value& value);

class CommandHistory;

// The document owns imported syntax and semantics; views use stable IDs, never engine objects.
class MissionDocument {
public:
    static std::expected<MissionDocument, LoadError> read(
        const std::filesystem::path& fit, const std::filesystem::path& pak, const Diagnostic& log,
        std::span<const std::uint32_t> markerObjects = {});
    static std::expected<MissionDocument, LoadError> decode(
        std::string fit, std::vector<std::byte> pak, std::string mission, const Diagnostic& log,
        std::span<const std::uint32_t> markerObjects = {});
    const Syntax& syntax() const { return syntax_; }
    const PacketArchive& archive() const { return pak_; }
    const std::vector<TerrainTile>& terrain() const { return terrain_; }
    std::size_t terrainSide() const { return terrainSide_; }
    const std::vector<Entity>& entities() const { return entities_; }
    std::span<const std::uint32_t> markerObjects() const { return markerObjects_; }
    const std::vector<Objective>& objectives() const { return objectives_; }
    const Field* field(NodeId id) const;
    const Section* section(NodeId id) const;
    const Section* childSection(NodeId parent, std::string_view name) const;
    const Field* find(NodeId section, std::string_view name) const;
    const Field* find(std::string_view section, std::string_view name) const;
    const Entity* entity(EntityId id) const;
    bool hasMarkerReference(EntityId id) const;
    std::expected<std::optional<NodeId>, LoadError> markerReference(EntityId id) const;
    std::string fitBytes() const;
    std::span<const std::byte> pakBytes() const { return encodedPak_.empty() ? pak_.originalBytes : encodedPak_; }
    bool modified() const;
    std::uint64_t revision() const { return revision_; }
    std::expected<void, LoadError> save(const std::filesystem::path& stem, const Diagnostic& log);

private:
    friend class CommandHistory;
    Syntax syntax_;
    PacketArchive pak_;
    std::vector<TerrainTile> terrain_;
    std::vector<Entity> entities_;
    std::vector<std::uint32_t> markerObjects_;
    std::vector<Objective> objectives_;
    std::string savedFit_;
    std::vector<std::byte> savedPlacements_, placements_, encodedPak_;
    std::unordered_map<std::string, std::unordered_map<std::string, NodeId>> index_;
    std::unordered_map<std::uint64_t, std::size_t> fieldIndex_;
    std::string mission_;
    std::size_t terrainSide_{};
    std::uint64_t revision_{};
    std::uint64_t nextEntityId_ = 1, nextNodeId_ = 2 * nodeLimit + 1;
    void rebuildIndex();
    bool isMarkerPacket(const Entity& entity) const;
    StructuralState structuralState() const;
    std::expected<void, LoadError> restore(const StructuralState& state);
    std::expected<StructuralState, LoadError> structuralCandidate(std::string fit,
        std::vector<std::byte> placements, const std::vector<EntityId>& ids,
        const std::unordered_map<std::string, std::string>& renamed = {});
    std::expected<void, LoadError> validateSemantics();
    std::expected<void, LoadError> assign(NodeId id, Value value);
};

} // namespace editor::document
