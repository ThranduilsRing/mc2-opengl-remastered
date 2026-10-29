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
    std::string_view warning;
    bool array{};
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

// Authoring values reference Parts by identity; ABL literal scans never create these.
struct PatrolPoint {
    std::uint32_t id{};
    std::array<double, 3> position{};
    bool operator==(const PatrolPoint&) const = default;
};
struct PatrolPath {
    std::uint32_t id{};
    std::string name;
    bool loop{};
    std::int32_t stance = 2;
    std::vector<PatrolPoint> points;
    bool operator==(const PatrolPath&) const = default;
};
struct BrainCell {
    std::uint32_t number{}, type{};
    double value{};
    bool operator==(const BrainCell&) const = default;
};
struct BrainAssignment {
    EntityId entity;
    std::optional<std::string> module;
    std::optional<std::uint32_t> staticVars;
    std::optional<std::vector<BrainCell>> cells;
    // Complete, lossless Brain block, independent of the legacy module and cells.
    std::optional<std::string> definition;
    bool operator==(const BrainAssignment&) const = default;
};
enum class ConditionSpecies : std::uint8_t {
    destroyAll, destroyNumber, destroyGroup, destroyUnit, destroyStructure,
    captureOrDestroyAll, captureOrDestroyNumber, captureOrDestroyGroup, captureOrDestroyUnit, captureOrDestroyStructure,
    deadOrFledAll, deadOrFledNumber, deadOrFledGroup, deadOrFledUnit,
    captureUnit, captureStructure, guardUnit, guardStructure,
    moveAny, moveAll, moveSurvivors, moveMechs, flag, elapsed
};
enum class ActionSpecies : std::uint8_t { bik, wav, text, resourceText, flag, technology };
// Unsupported entries are opaque data, never a fallback executable species.
enum class ReferenceKind { none, unit, building };
enum class ReferenceStatus { Resolved, Missing, Ambiguous, RuntimeOnlyWid, Unsupported };
struct ReferenceSource {
    NodeId node;
    std::string_view key;
    std::string lexeme;
};
struct ObjectiveReference {
    ReferenceKind kind = ReferenceKind::none;
    ReferenceStatus status = ReferenceStatus::Unsupported;
    std::optional<EntityId> target;
    std::string_view reason = "no supported entity reference";
    std::vector<ReferenceSource> sources;
};
inline std::size_t referenceStorage(const ObjectiveReference& reference) {
    std::size_t bytes = reference.sources.capacity() * sizeof(ReferenceSource);
    for (const auto& source : reference.sources) bytes += source.lexeme.capacity();
    return bytes;
}
struct ObjectiveEntry {
    NodeId section;
    std::variant<ConditionSpecies, ActionSpecies, std::monostate> species;
    ObjectiveReference reference;
    struct Flag {
        std::optional<std::string> id;
        bool value = true;
        bool indexFallback{};
        std::optional<NodeId> idSource, valueSource;
        std::string warning;
    };
    std::optional<Flag> flag;
    std::vector<Scalar> payload;
    std::string_view policy;
};
inline constexpr std::size_t objectiveTextLimit = 2000;
struct TeamFlagRow {
    std::size_t index{};
    std::optional<std::string> id;
    bool value{};
    std::string_view status;
    std::optional<std::size_t> duplicateOf;
};
struct TeamFlags {
    std::optional<NodeId> section;
    std::size_t stop{};
    std::vector<TeamFlagRow> rows;
    const TeamFlagRow* resolve(std::string_view id) const;
};
struct ObjectiveFieldSpec {
    std::string_view key, prefix;
    std::size_t type;
    double minimum{}, maximum{};
    std::int64_t integerDefault{};
};
std::span<const ObjectiveFieldSpec> objectiveSchema();
struct ConditionSpec {
    ConditionSpecies species;
    std::string_view name;
    std::span<const ObjectiveFieldSpec> fields;
    bool reference{};
};
std::span<const ConditionSpec> conditionSchema();
struct ActionSpec {
    ActionSpecies species;
    std::string_view name;
    std::span<const ObjectiveFieldSpec> fields;
};
std::span<const ActionSpec> actionSchema(bool creation = false);
struct ActionDraft {
    ActionSpecies species = ActionSpecies::bik;
    std::vector<Scalar> payload;
    bool flagIndex{};
};
std::expected<std::string, LoadError> encodeActionDraft(const ActionDraft& draft);
std::expected<void, LoadError> validateObjectiveBlock(std::string_view block);
std::string objectivePayloadWire(const ObjectiveFieldSpec& field, const Scalar& value);
std::span<const ObjectiveFieldSpec> objectivePayloadSchema(const ObjectiveEntry& entry);
std::expected<void, LoadError> validateObjectiveScalar(const ObjectiveFieldSpec& spec, const Scalar& value);
struct ObjectiveScalar {
    std::string_view key;
    std::optional<NodeId> source;
    Scalar value;
    std::string_view warning;
};
struct Objective {
    NodeId section;
    std::uint32_t team{}, exportedIndex{};
    std::array<std::vector<ObjectiveEntry>, 4> entries;
    std::vector<ObjectiveScalar> scalars;
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
    std::optional<std::array<std::uint32_t, 2>> parentCell(EntityId id) const;
    const PacketArchive& archive() const { return pak_; }
    const std::vector<TerrainTile>& terrain() const { return terrain_; }
    std::size_t terrainSide() const { return terrainSide_; }
    const std::vector<Entity>& entities() const { return entities_; }
    std::span<const std::uint32_t> markerObjects() const { return markerObjects_; }
    const std::vector<Objective>& objectives() const { return objectives_; }
    TeamFlags teamFlags(std::uint32_t team) const;
    const Objective* objective(NodeId id) const;
    const ObjectiveScalar* objectiveScalar(NodeId objective, std::string_view key) const;
    const Field* field(NodeId id) const;
    const Section* section(NodeId id) const;
    const Section* childSection(NodeId parent, std::string_view name) const;
    const Field* find(NodeId section, std::string_view name) const;
    const Field* find(std::string_view section, std::string_view name) const;
    const Entity* entity(EntityId id) const;
    std::expected<std::optional<PatrolPath>, LoadError> patrolPath(EntityId id) const;
    std::expected<BrainAssignment, LoadError> brainAssignment(EntityId id) const;
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
    static std::expected<MissionDocument, LoadError> decodeSource(std::string fit,
        std::vector<std::byte> pak, std::string mission, const Diagnostic& log,
        std::span<const std::uint32_t> markerObjects, bool resolve);
    std::expected<void, LoadError> decodeReferences(bool resolve, const Diagnostic& log);
    bool referenceMatches(const Objective& objective, const ObjectiveEntry& entry, const Entity& entity) const;
    void rebuildIndex();
    bool isMarkerPacket(const Entity& entity) const;
    StructuralState structuralState() const;
    std::expected<void, LoadError> restore(const StructuralState& state);
    std::expected<StructuralState, LoadError> structuralCandidate(std::string fit,
        std::vector<std::byte> placements, const std::vector<EntityId>& ids,
        const std::unordered_map<std::string, std::string>& renamed = {},
        NodeId resolving = {}, EntityId target = {});
    std::expected<void, LoadError> validateSemantics();
    std::expected<void, LoadError> decodeObjectiveScalars(Objective& objective);
    void preserveObjectiveField(NodeId id, std::string_view warning);
    std::expected<void, LoadError> assign(NodeId id, Value value);
};

} // namespace editor::document
