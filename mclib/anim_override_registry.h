// anim_override_registry.h — ANIM-OVERRIDE-MVP-1.
// Engine-independent parse of anim_overrides/anims.json. No TG_*/GameOS deps.
// Declarative gesture-clip remap keyed by "<mechName>:<gesture>". Lets a mod
// point a mech's gesture (Walk/Run/Idle/...) at an arbitrarily-named .ase/.agl
// clip in the mod's own dir, instead of relying on the stock filename
// convention (<mech><GestureSuffix>). Additive over a base manifest; mod entries
// win on dup key. Sibling of model_override_registry.h (same pattern). Invalid
// entries are logged + dropped, never fatal. Default-off: with no manifest,
// resolve() returns null and the loader uses the stock convention path.
// See docs/modding-animations.md + docs/ppc-flight-and-fx-anim-moddability-recon.md.
#pragma once
#include "mc2_czstring.h"
#include <string>
#include <string_view>
#include <vector>

struct AnimOverrideRecord {
    std::string mechName;     // from "replaces" after "mech:" — NORMALIZED lowercase
    std::string gesture;      // gesture suffix (Walk/Run/...) — NORMALIZED lowercase
    std::string sourceBase;   // clip basename WITHOUT extension, relative to manifestDir
    std::string manifestDir;  // dir the override clip resolves from (mods/<id>/data/anim_overrides)
    // MVP invariants (validated at load; entry dropped + logged if violated):
    //   type=="anim", fallback=="stock", replaces=="mech:<name>", gesture non-empty,
    //   safe relative .ase/.agl source. Duplicate key: first wins (mod merge: mod wins).
};

class AnimOverrideRegistry {
public:
    // Clears registry then loads from manifestPath. Returns count.
    int loadFromFile(std::string_view manifestPath,
                     std::string_view manifestDir);
    // Additive: loads manifestPath without clearing. Mod entries WIN on dup key.
    // Returns count merged in (not total).
    int mergeFromFile(std::string_view manifestPath,
                      std::string_view manifestDir);
    // Returns the override clip for (mechName, gesture), or nullptr for none.
    // Both keys are NUL-terminated; nullptr means no override.
    const AnimOverrideRecord* resolve(mc2::czstring mechName,
                                      mc2::czstring gesture) const;
    int count() const { return static_cast<int>(records_.size()); }
    // Lazy singleton: loads base data/anim_overrides/anims.json then merges the
    // active mod's manifest (MC2_ACTIVE_MOD) on first use. Cheap no-op when
    // neither manifest exists.
    static AnimOverrideRegistry& instance();
private:
    std::vector<AnimOverrideRecord> records_;
    std::string manifestDir_;
};
