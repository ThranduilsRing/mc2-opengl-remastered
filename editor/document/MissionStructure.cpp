#include "MissionDocument.h"

#include <algorithm>
#include <unordered_map>

namespace editor::document {
namespace {
std::vector<std::string> sectionKeys(const Syntax& syntax,
    const std::unordered_map<std::string, std::string>& renamed)
{
    std::unordered_map<std::uint64_t, std::string> paths;
    std::unordered_map<std::string, std::size_t> occurrences;
    std::vector<std::string> keys;
    for (const auto& section : syntax.sections) {
        auto name = identifier(section.name);
        if (!section.parent) {
            if (auto found = renamed.find(name); found != renamed.end()) name = found->second;
        }
        const auto component = std::to_string(name.size()) + ":" + name;
        auto path = section.parent ? paths.at(section.parent->value) + "/" + component : component;
        path += "#" + std::to_string(occurrences[path]++);
        paths.emplace(section.id.value, path);
        keys.push_back(std::move(path));
    }
    return keys;
}
}

StructuralState MissionDocument::structuralState() const
{
    return {syntax_, entities_, objectives_, placements_};
}

std::expected<void, LoadError> MissionDocument::restore(const StructuralState& state)
{
    auto encoded = encodePlacements(pak_, state.placements);
    if (!encoded) return std::unexpected(encoded.error());
    syntax_ = state.syntax;
    entities_ = state.entities;
    objectives_ = state.objectives;
    placements_ = state.placements;
    encodedPak_ = std::move(*encoded);
    rebuildIndex();
    for (const auto& section : syntax_.sections) nextNodeId_ = std::max(nextNodeId_, section.id.value + 1);
    for (const auto& field : syntax_.fields) nextNodeId_ = std::max(nextNodeId_, field.id.value + 1);
    ++revision_;
    return {};
}

std::expected<StructuralState, LoadError> MissionDocument::structuralCandidate(std::string fit,
    std::vector<std::byte> placements, const std::vector<EntityId>& ids,
    const std::unordered_map<std::string, std::string>& renamed)
{
    auto encoded = encodePlacements(pak_, placements);
    if (!encoded) return std::unexpected(encoded.error());
    auto candidate = decode(std::move(fit), std::move(*encoded), mission_, {}, markerObjects_);
    if (!candidate) return std::unexpected(candidate.error());
    if (candidate->entities_.size() != ids.size()) return std::unexpected(LoadError{"command", "entity_count_mismatch", 0});
    const auto oldKeys = sectionKeys(syntax_, renamed), newKeys = sectionKeys(candidate->syntax_, {});
    std::unordered_map<std::string, const Section*> oldSections;
    for (std::size_t i = 0; i < oldKeys.size(); ++i) oldSections.emplace(oldKeys[i], &syntax_.sections[i]);
    std::unordered_map<std::uint64_t, NodeId> remap;
    auto nextNode = nextNodeId_;
    for (std::size_t i = 0; i < newKeys.size(); ++i) {
        auto& section = candidate->syntax_.sections[i];
        const auto found = oldSections.find(newKeys[i]);
        const auto* old = found == oldSections.end() ? nullptr : found->second;
        remap.emplace(section.id.value, old ? old->id : NodeId{nextNode++});
        for (auto id : section.fields) {
            const auto* field = candidate->field(id);
            const auto* previous = old ? find(old->id, field->name) : nullptr;
            remap.emplace(id.value, previous ? previous->id : NodeId{nextNode++});
        }
    }
    for (auto& section : candidate->syntax_.sections) {
        section.id = remap.at(section.id.value);
        if (section.parent) section.parent = remap.at(section.parent->value);
        for (auto& id : section.fields) id = remap.at(id.value);
    }
    for (auto& field : candidate->syntax_.fields) field.id = remap.at(field.id.value);
    for (std::size_t i = 0; i < ids.size(); ++i) {
        auto& entity = candidate->entities_[i];
        entity.id = ids[i];
        for (auto* id : {&entity.section, &entity.warrior, &entity.brain}) if (*id) *id = remap.at((*id)->value);
    }
    for (auto& objective : candidate->objectives_) {
        objective.section = remap.at(objective.section.value);
        for (auto& list : objective.entries) for (auto& entry : list) entry.section = remap.at(entry.section.value);
        for (auto& scalar : objective.scalars) if (scalar.source) scalar.source = remap.at(scalar.source->value);
    }
    return StructuralState{std::move(candidate->syntax_), std::move(candidate->entities_),
        std::move(candidate->objectives_), std::move(placements)};
}
}
