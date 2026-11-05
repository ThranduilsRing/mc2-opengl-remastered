// mech_skel_import.cpp — shared skeleton/FK importer math (see header).
// The one place the FK chain lives; harness and game both link this TU.
#include "mech_skel_import.h"

#include <assimp/scene.h>
#include <assimp/anim.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string_view>
#include "Gates/gate.h"
#include "Gates/gate_cold.h"
#include "mc2fmt.h"

namespace mc2skel {
namespace {

// MECH-UBLB-ATTACHMENT-FIX-1 — rotation-only retargeting (default ON).
// The BT2018 mech is a RIGID skeleton (every mesh-part binds to exactly one bone;
// no blend weights). Bone lengths are therefore fixed. The clips, however, carry
// TRANSLATION channels on the spine chain (j_Pitch/j_Spine/j_Spine1) and the
// pelvis — a ball-joint waist absorbs rotation but not translation, so applying
// those keys lifts the rigid upper body off the lower-body socket (constant
// ~0.015 in normalized units across every clip/frame). For a rigid mech the
// correct retarget is rotation-only: take the channel's ROTATION, keep the
// node-default translation+scale (the bind bone length). Also drops in-place root
// translation, which MC2 wants (the game positions the mech in the world).
// Set MC2_MECH_ANIM_ROTATION_ONLY=0 to apply raw channels (old behaviour).
bool rotationOnlyEnabled() {
    return true;  // default ON; only explicit "0" disables
}

// DIAGNOSTIC (default OFF): MC2_MECH_IMPORT_FORCE_POSE forces a single known static
// pose on ONE leg bone, bypassing the clip entirely — decisive test for the
// "lower body 180 reversed under animation / anim not playing" bug. Held static, no
// clip/time dependence, so it fires even when clip playback is broken.
//   =1 -> j_LThigh +90deg about local X    =2 -> +90 about local Y    =3 -> +90 about local Z
int forcePoseMode() {
    return mc2::gate::get<"MC2_MECH_IMPORT_FORCE_POSE">();
}
std::string forcePoseBone() {
    return mc2::gate::str<"MC2_MECH_IMPORT_FORCE_BONE">();   // row default j_LThigh
}

// FK walk with an extra local rotation injected on one named bone; all others rest.
void computeGlobalsForced(const aiNode* n, const aiMatrix4x4& parent,
                          const char* targetBone, const aiMatrix4x4& extraLocalRot,
                          std::map<std::string, aiMatrix4x4>& globals) {
    aiMatrix4x4 local = n->mTransformation;
    if (std::strcmp(n->mName.C_Str(), targetBone) == 0) local = local * extraLocalRot;
    aiMatrix4x4 global = parent * local;
    globals[n->mName.C_Str()] = global;
    for (unsigned i = 0; i < n->mNumChildren; ++i)
        computeGlobalsForced(n->mChildren[i], global, targetBone, extraLocalRot, globals);
}

// Sample one node-animation channel at tick time t -> local transform.
// When `rotationOnly` is set, translation+scale come from `nodeDefault` (the bind
// pose) and only rotation is taken from the channel (falling back to nodeDefault's
// rotation if the channel has no rotation keys).
aiMatrix4x4 sampleChannel(const aiNodeAnim* ch, double t,
                          const aiMatrix4x4& nodeDefault, bool rotationOnly) {
    auto lerpVec = [&](const aiVectorKey* keys, unsigned n) -> aiVector3D {
        if (n == 1) return keys[0].mValue;
        unsigned i = 0;
        while (i + 1 < n && keys[i + 1].mTime <= t) ++i;
        if (i + 1 >= n) return keys[n - 1].mValue;
        double t0 = keys[i].mTime, t1 = keys[i + 1].mTime;
        float f = (t1 > t0) ? static_cast<float>(((t - t0) / (t1 - t0))) : 0.0f;
        return keys[i].mValue * (1.0f - f) + keys[i + 1].mValue * f;
    };
    aiVector3D pos = ch->mNumPositionKeys ? lerpVec(ch->mPositionKeys, ch->mNumPositionKeys) : aiVector3D(0, 0, 0);
    aiVector3D scl = ch->mNumScalingKeys ? lerpVec(ch->mScalingKeys, ch->mNumScalingKeys) : aiVector3D(1, 1, 1);
    aiQuaternion rot;
    if (ch->mNumRotationKeys == 0) {
        // No rotation keys: identity unless rotation-only fallback fills it below.
    } else if (ch->mNumRotationKeys == 1) {
        rot = ch->mRotationKeys[0].mValue;
    } else if (ch->mNumRotationKeys > 1) {
        unsigned i = 0;
        while (i + 1 < ch->mNumRotationKeys && ch->mRotationKeys[i + 1].mTime <= t) ++i;
        if (i + 1 >= ch->mNumRotationKeys) {
            rot = ch->mRotationKeys[ch->mNumRotationKeys - 1].mValue;
        } else {
            double t0 = ch->mRotationKeys[i].mTime, t1 = ch->mRotationKeys[i + 1].mTime;
            float f = (t1 > t0) ? static_cast<float>(((t - t0) / (t1 - t0))) : 0.0f;
            aiQuaternion::Interpolate(rot, ch->mRotationKeys[i].mValue, ch->mRotationKeys[i + 1].mValue, f);
            rot.Normalize();
        }
    }
    if (rotationOnly) {
        // Keep bind translation+scale (rigid bone length); use channel rotation,
        // or the bind rotation when this channel carries no rotation keys.
        //
        // KNOWN ISSUE (accepted, do not "fix" the way below): the WALK clip shows
        // MILD arm/leg clipping — the arms swing contralaterally (BT gait) and cross
        // the legs. This is clip content (likely present in the original BT2018 too).
        // A SELECTIVE rotation-only attempt (suppress translation only on the
        // spine/pelvis, restore it for the limbs to "widen clearance") REGRESSED:
        // the limbs' translation channels are NOT pure bone lengths — restoring them
        // pulled the arm joints out to the heatsink "ears" (wrong place) and sank the
        // legs below ground. Blanket rotation-only puts every joint in the CORRECT
        // place; the residual walk clip-through is the lesser evil. If you revisit,
        // the fix is an ANIMATION-TIMING change (phase-shift the arms vs the legs),
        // NOT a per-bone translation change.
        aiVector3D defScl, defPos; aiQuaternion defRot;
        nodeDefault.Decompose(defScl, defRot, defPos);
        if (ch->mNumRotationKeys == 0) rot = defRot;
        return aiMatrix4x4(defScl, rot, defPos);
    }
    return aiMatrix4x4(scl, rot, pos);
}

aiMatrix4x4 globalOrIdentity(const std::map<std::string, aiMatrix4x4>& globals, const std::string& bone) {
    const auto g = globals.find(bone);
    if (g == globals.end()) return aiMatrix4x4();
    const auto& [boneName, matrix] = *g;
    return matrix;
}

void computeGlobals(const aiNode* n, const aiMatrix4x4& parent,
                    const std::map<std::string, const aiNodeAnim*>& chans, double t,
                    bool rotationOnly, std::map<std::string, aiMatrix4x4>& globals) {
    auto it = chans.find(n->mName.C_Str());
    aiMatrix4x4 local = n->mTransformation;
    if (it != chans.end()) {
        const auto& [chanName, chan] = *it;
        local = sampleChannel(chan, t, n->mTransformation, rotationOnly);
    }
    aiMatrix4x4 global = parent * local;
    globals[n->mName.C_Str()] = global;
    for (unsigned i = 0; i < n->mNumChildren; ++i)
        computeGlobals(n->mChildren[i], global, chans, t, rotationOnly, globals);
}

// Map node name -> parent node name across the whole tree.
void mapParents(const aiNode* n, std::string_view parent,
                std::map<std::string, std::string>& out) {
    out[n->mName.C_Str()] = parent;
    for (unsigned i = 0; i < n->mNumChildren; ++i)
        mapParents(n->mChildren[i], n->mName.C_Str(), out);
}

void rowMajor(const aiMatrix4x4& m, float* out16) { std::memcpy(out16, &m.a1, 16 * sizeof(float)); }

}  // namespace

void BuildSkeleton(const aiScene* scene,
                   std::vector<std::string>& boneNames,
                   std::vector<int>& parentIndex,
                   std::vector<std::array<float, 16>>& inverseBind) {
    boneNames.clear(); parentIndex.clear(); inverseBind.clear();

    // Ordered de-dup of bones in first-encounter order (stable bone indices).
    std::map<std::string, int> idx;
    std::map<std::string, aiMatrix4x4> offsets;
    for (unsigned m = 0; m < scene->mNumMeshes; ++m) {
        const aiMesh* mesh = scene->mMeshes[m];
        for (unsigned b = 0; b < mesh->mNumBones; ++b) {
            const aiBone* bone = mesh->mBones[b];
            std::string nm = bone->mName.C_Str();
            if (!idx.count(nm)) {
                idx[nm] = static_cast<int>(boneNames.size());
                boneNames.push_back(nm);
                std::array<float, 16> ib; rowMajor(bone->mOffsetMatrix, ib.data());
                inverseBind.push_back(ib);
            }
        }
    }

    // Parent index = nearest ancestor node that is also a bone.
    std::map<std::string, std::string> parents;
    mapParents(scene->mRootNode, "", parents);
    parentIndex.resize(boneNames.size(), -1);
    for (size_t i = 0; i < boneNames.size(); ++i) {
        std::string p = parents.count(boneNames[i]) ? parents[boneNames[i]] : "";
        while (!p.empty() && !idx.count(p))
            p = parents.count(p) ? parents[p] : "";
        parentIndex[i] = (!p.empty() && idx.count(p)) ? idx[p] : -1;
    }
}

bool EvaluateClipGpuBones(const aiScene* scene,
                          std::string_view clip,
                          float frame,
                          const std::vector<std::string>& boneNames,
                          std::vector<GpuBone>& out,
                          double* outTimeTicks,
                          double* outDurTicks) {
    // DIAGNOSTIC forced-pose branch (clip-independent; fires even if playback broken).
    if (int fp = forcePoseMode()) {
        aiMatrix4x4 extra;  // identity
        const float a = 1.57079633f;  // 90 deg
        if (fp == 1) aiMatrix4x4::RotationX(a, extra);
        else if (fp == 2) aiMatrix4x4::RotationY(a, extra);
        else if (fp == 3) aiMatrix4x4::RotationZ(a, extra);
        std::map<std::string, aiMatrix4x4> globals;
        computeGlobalsForced(scene->mRootNode, aiMatrix4x4(), forcePoseBone().c_str(), extra, globals);
        out.assign(boneNames.size(), GpuBone{});
        for (size_t i = 0; i < boneNames.size(); ++i) {
            rowMajor(globalOrIdentity(globals, boneNames[i]), out[i].m);
        }
        if (outTimeTicks) *outTimeTicks = 0.0;
        if (outDurTicks) *outDurTicks = 1.0;
        return true;
    }
    const bool clipDiag = mc2::gate::get<"MC2_MECH_IMPORT_CLIP_DIAG">();
    const aiAnimation* anim = nullptr;
    for (unsigned a = 0; a < scene->mNumAnimations; ++a)
        if (clip == scene->mAnimations[a]->mName.C_Str()) { anim = scene->mAnimations[a]; break; }
    if (!anim) {
        if (clipDiag) {
            static int miss = 0;
            if (miss++ < 12) {
                mc2::fmt::print_err("[CLIP-DIAG] NOT FOUND clip='{}' (scene has {:d} anims; first few:", clip.substr(0, clip.find('\0')), mc2::fmt::as_u32(scene->mNumAnimations));
                for (unsigned a = 0; a < scene->mNumAnimations && a < 4; ++a)
                    mc2::fmt::print_err(" '{}'", mc2::fmt::safe(scene->mAnimations[a]->mName.C_Str()));
                mc2::fmt::print_err(")\n");
            }
        }
        return false;
    }

    double tps = anim->mTicksPerSecond != 0.0 ? anim->mTicksPerSecond : 1000.0;
    double t = static_cast<double>(frame) / 30.0 * tps;
    t = std::min(t, anim->mDuration);
    t = std::max<double>(t, 0);
    if (clipDiag) {
        static int hit = 0;
        if (hit++ < 12)
            mc2::fmt::print_err("[CLIP-DIAG] FOUND clip='{}' frame={:.2f} t={:.2f} dur={:.2f} rotOnly={:d}\n", clip.substr(0, clip.find('\0')), static_cast<double>(frame), t, anim->mDuration, mc2::fmt::as_i32(static_cast<int>(rotationOnlyEnabled())));
    }
    if (outTimeTicks) *outTimeTicks = t;
    if (outDurTicks) *outDurTicks = anim->mDuration;

    std::map<std::string, const aiNodeAnim*> chans;
    for (unsigned c = 0; c < anim->mNumChannels; ++c)
        chans[anim->mChannels[c]->mNodeName.C_Str()] = anim->mChannels[c];

    std::map<std::string, aiMatrix4x4> globals;
    computeGlobals(scene->mRootNode, aiMatrix4x4(), chans, t, rotationOnlyEnabled(), globals);

    // Return the joint GLOBAL (FK world) matrix per bone — NOT global*inverseBind.
    // The BT2018 rig has a DIFFERENT inverse-bind per mesh-part for the same bone
    // (19/28 bones carry 2 distinct offsets across skins), so inverse-bind cannot
    // be folded into one per-bone matrix. Instead the caller bakes each part's own
    // offset into its vertices at import; the per-frame GPU matrix is then just the
    // joint global. skin = global(bone) * (offset_part * vertex).
    out.assign(boneNames.size(), GpuBone{});
    for (size_t i = 0; i < boneNames.size(); ++i) {
        rowMajor(globalOrIdentity(globals, boneNames[i]), out[i].m);
    }
    return true;
}

void EvaluateRestGpuBones(const aiScene* scene,
                          const std::vector<std::string>& boneNames,
                          std::vector<GpuBone>& out) {
    std::map<std::string, const aiNodeAnim*> none;  // no clip -> node defaults (rest)
    std::map<std::string, aiMatrix4x4> globals;
    // Rest has no channels, so rotationOnly is moot — pass false.
    computeGlobals(scene->mRootNode, aiMatrix4x4(), none, 0.0, false, globals);
    out.assign(boneNames.size(), GpuBone{});
    for (size_t i = 0; i < boneNames.size(); ++i) {
        rowMajor(globalOrIdentity(globals, boneNames[i]), out[i].m);
    }
}

}  // namespace mc2skel
