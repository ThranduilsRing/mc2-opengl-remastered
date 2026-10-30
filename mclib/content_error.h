#pragma once

#include <cstddef>
#include <string_view>

namespace mc2::content {
enum class Kind { Texture, Shape, ObjectType, WeaponAmmo, MoveType, Sound, MissionData };

// Recoverable authored-data failures, counted once per (kind, path, message).
void error(Kind kind, std::string_view path, std::string_view message);
std::size_t count(Kind kind);
void resetMission();
void reportMission(std::string_view mission);
} // namespace mc2::content
