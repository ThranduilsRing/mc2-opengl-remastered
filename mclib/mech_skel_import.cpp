// mech_skel_import.cpp — shared skeleton/FK importer math (see header).
// The one place the FK chain lives; harness and game both link this TU.
#include "mech_skel_import.h"
#include "clip_sample.h"

#include <assimp/scene.h>
#include <assimp/anim.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <span>
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

// Sample one node-animation channel at tick time t -> local transform.
// When `rotationOnly` is set, translation+scale come from `nodeDefault` (the bind
// pose) and only rotation is taken from the channel (falling back to nodeDefault's
// rotation if the channel has no rotation keys).
struct LocalTRS { aiVector3D scl; aiQuaternion rot; aiVector3D pos; };
LocalTRS sampleTRS(const aiNodeAnim* ch, double t,
                   const aiMatrix4x4& nodeDefault, bool rotationOnly, double secPerTick) {
    aiVector3D pos = ch->mNumPositionKeys
        ? clip::sampleVec3<aiVectorKey, aiVector3D>(std::span<const aiVectorKey>(ch->mPositionKeys, ch->mNumPositionKeys), t, secPerTick)
        : aiVector3D(0, 0, 0);
    aiVector3D scl = ch->mNumScalingKeys
        ? clip::sampleVec3<aiVectorKey, aiVector3D>(std::span<const aiVectorKey>(ch->mScalingKeys, ch->mNumScalingKeys), t, secPerTick)
        : aiVector3D(1, 1, 1);
    aiQuaternion rot;  // no rotation keys: identity unless rotation-only fallback fills it below
    if (ch->mNumRotationKeys)
        rot = clip::sampleQuat<aiQuatKey, aiQuaternion>(
            std::span<const aiQuatKey>(ch->mRotationKeys, ch->mNumRotationKeys), t, secPerTick,
            [](aiQuaternion& out, const aiQuaternion& a, const aiQuaternion& b, float f) {
                aiQuaternion::Interpolate(out, a, b, f);
                out.Normalize();
            });
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
        return {defScl, rot, defPos};
    }
    return {scl, rot, pos};
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

// ---------------------------------------------------------------------------
// SkelPlan: the node tree flattened once (preorder, parent before child), the
// bone -> node table, and a per-animation node -> channel table. Per-frame
// evaluation is then index arithmetic: no std::map / std::string work.
// ---------------------------------------------------------------------------
struct SkelPlan {
    const aiScene* scene = nullptr;
    std::vector<const aiNode*> nodes;        // preorder
    std::vector<int> parent;                 // node index of parent, -1 for the root
    std::vector<int> boneNode;               // parallel to boneNames; -1 = not a node (identity)
    std::vector<std::vector<const aiNodeAnim*>> chanByNode;  // [anim][node]; nullptr = no channel
};

namespace {
void flatten(const aiNode* n, int parent, SkelPlan& p) {
    const int me = static_cast<int>(p.nodes.size());
    p.nodes.push_back(n);
    p.parent.push_back(parent);
    for (unsigned i = 0; i < n->mNumChildren; ++i) flatten(n->mChildren[i], me, p);
}
}  // namespace

std::shared_ptr<const SkelPlan> BuildPlan(const aiScene* scene, const std::vector<std::string>& boneNames) {
    auto p = std::make_shared<SkelPlan>();
    p->scene = scene;
    flatten(scene->mRootNode, -1, *p);
    // Same lookup semantics as the old per-frame maps: when several nodes share a name the
    // LAST one in the walk owns the bone, and the last channel with a given name wins.
    std::map<std::string, int> nodeByName;
    for (size_t i = 0; i < p->nodes.size(); ++i) nodeByName[p->nodes[i]->mName.C_Str()] = static_cast<int>(i);
    p->boneNode.resize(boneNames.size(), -1);
    for (size_t b = 0; b < boneNames.size(); ++b) {
        auto it = nodeByName.find(boneNames[b]);
        if (it != nodeByName.end()) {
            const auto& [name, node] = *it;
            p->boneNode[b] = node;
        }
    }
    p->chanByNode.resize(scene->mNumAnimations);
    for (unsigned a = 0; a < scene->mNumAnimations; ++a) {
        const aiAnimation* anim = scene->mAnimations[a];
        std::map<std::string, const aiNodeAnim*> chans;
        for (unsigned c = 0; c < anim->mNumChannels; ++c) chans[anim->mChannels[c]->mNodeName.C_Str()] = anim->mChannels[c];
        auto& tbl = p->chanByNode[a];
        tbl.assign(p->nodes.size(), nullptr);
        for (size_t i = 0; i < p->nodes.size(); ++i) {
            auto it = chans.find(p->nodes[i]->mName.C_Str());
            if (it != chans.end()) {
                const auto& [name, channel] = *it;
                tbl[i] = channel;
            }
        }
    }
    return p;
}

namespace {
std::optional<int> findAnim(const aiScene* scene, std::string_view clip) {
    for (unsigned a = 0; a < scene->mNumAnimations; ++a)
        if (clip == scene->mAnimations[a]->mName.C_Str()) return static_cast<int>(a);
    return std::nullopt;
}

void emitBones(const SkelPlan& plan, const std::vector<aiMatrix4x4>& globals, std::vector<GpuBone>& out) {
    out.assign(plan.boneNode.size(), GpuBone{});
    for (size_t i = 0; i < plan.boneNode.size(); ++i)
        rowMajor(plan.boneNode[i] >= 0 ? globals[plan.boneNode[i]] : aiMatrix4x4(), out[i].m);
}

// Clip time (ticks) for a 30 fps frame index, clamped to the clip.
double clipTicks(const aiAnimation* anim, float frame) {
    const double tps = anim->mTicksPerSecond != 0.0 ? anim->mTicksPerSecond : 1000.0;
    double t = static_cast<double>(frame) / 30.0 * tps;
    t = std::min(t, anim->mDuration);
    t = std::max<double>(t, 0);
    return t;
}
double secPerTickOf(const aiAnimation* anim) {
    return 1.0 / (anim->mTicksPerSecond != 0.0 ? anim->mTicksPerSecond : 1000.0);
}
}  // namespace

LocalRotation ResolveTorsoTwist(const SkelPlan& plan) {
    LocalRotation result;
    float largestYaw = 0.000174533f; // 0.01 degrees
    // BT2018's torso is Spine2. Root/pelvis sway and lower-spine pitch
    // channels in the twist clips must not override the named joint.
    for (size_t i = 0; i < plan.nodes.size(); ++i)
        if (std::string_view(plan.nodes[i]->mName.C_Str()) == "j_Spine2") {
            result.node = static_cast<int>(i);
            break;
        }
    const bool namedTorso = result.node >= 0;
    for (size_t i = 0; i < plan.nodes.size(); ++i) {
        if (namedTorso) break;
        if (!std::string_view(plan.nodes[i]->mName.C_Str()).starts_with("j_Spine")) continue;
        bool belowPelvis = false;
        for (int p = plan.parent[i]; p >= 0; p = plan.parent[p]) {
            const std::string_view name(plan.nodes[p]->mName.C_Str());
            if (name == "j_Pelvis" || name == "pelvis") belowPelvis = true;
        }
        if (!belowPelvis) continue;
        aiMatrix4x4 global = plan.nodes[i]->mTransformation;
        for (int p = plan.parent[i]; p >= 0; p = plan.parent[p])
            global = plan.nodes[p]->mTransformation * global;
        aiMatrix3x3 inverse(global);
        aiVector3D yawAxis = inverse.Inverse() * aiVector3D(0, 1, 0);
        yawAxis.Normalize();
        aiVector3D scale, pos; aiQuaternion rest;
        plan.nodes[i]->mTransformation.Decompose(scale, rest, pos);
        for (const char* name : {"atlas_torsoTwistLeft", "atlas_torsoTwistRight"}) {
            const auto anim = findAnim(plan.scene, name);
            const aiNodeAnim* ch = anim ? plan.chanByNode[*anim][i] : nullptr;
            if (!ch) continue;
            for (unsigned k = 0; k < ch->mNumRotationKeys; ++k) {
                aiQuaternion inverse = rest;
                aiQuaternion delta = inverse.Conjugate() * ch->mRotationKeys[k].mValue;
                delta.Normalize();
                // Twist about world-up, excluding pitch/roll and quaternion sign.
                const float yaw = std::abs(2.0f * std::atan2(
                    aiVector3D(delta.x, delta.y, delta.z) * yawAxis, std::abs(delta.w)));
                if (yaw <= largestYaw) continue;
                result.node = static_cast<int>(i);
                largestYaw = yaw;
            }
        }
    }
    if (result.node >= 0) {
        aiMatrix4x4 global = plan.nodes[result.node]->mTransformation;
        for (int p = plan.parent[result.node]; p >= 0; p = plan.parent[p])
            global = plan.nodes[p]->mTransformation * global;
        aiMatrix3x3 inverse(global);
        aiVector3D axis = inverse.Inverse() * aiVector3D(0, 1, 0);
        axis.Normalize();
        result.axis = {axis.x, axis.y, axis.z};
    }
    return result;
}

namespace {
void applyLocalRotation(aiMatrix4x4& local, size_t node, const LocalRotation* extra) {
    if (!extra || extra->node != static_cast<int>(node) || extra->degrees == 0.0f) return;
    const aiVector3D axis(extra->axis[0], extra->axis[1], extra->axis[2]);
    const aiMatrix4x4 rotation(aiQuaternion(axis, extra->degrees * 0.01745329252f).GetMatrix());
    local = local * rotation;
}
}  // namespace

bool EvaluateClipGpuBones(const SkelPlan& plan,
                          std::string_view clip,
                          float frame,
                          std::vector<GpuBone>& out,
                          double* outTimeTicks,
                          double* outDurTicks, const LocalRotation* extra) {
    const aiScene* scene = plan.scene;
    // DIAGNOSTIC forced-pose branch (clip-independent), same math as the scene-based overload.
    if (int fp = forcePoseMode()) {
        aiMatrix4x4 forced;  // identity
        const float a = 1.57079633f;  // 90 deg
        if (fp == 1) aiMatrix4x4::RotationX(a, forced);
        else if (fp == 2) aiMatrix4x4::RotationY(a, forced);
        else if (fp == 3) aiMatrix4x4::RotationZ(a, forced);
        const std::string target = forcePoseBone();
        std::vector<aiMatrix4x4> g(plan.nodes.size());
        for (size_t i = 0; i < plan.nodes.size(); ++i) {
            aiMatrix4x4 local = plan.nodes[i]->mTransformation;
            if (target == plan.nodes[i]->mName.C_Str()) local = local * forced;
            applyLocalRotation(local, i, extra);
            g[i] = plan.parent[i] >= 0 ? g[plan.parent[i]] * local : local;
        }
        emitBones(plan, g, out);
        if (outTimeTicks) *outTimeTicks = 0.0;
        if (outDurTicks) *outDurTicks = 1.0;
        return true;
    }
    const std::optional<int> aiOpt = findAnim(scene, clip);
    if (!aiOpt) {
        return false;
    }
    const int ai = *aiOpt;
    const aiAnimation* anim = scene->mAnimations[ai];
    const double t = clipTicks(anim, frame);
    if (outTimeTicks) *outTimeTicks = t;
    if (outDurTicks) *outDurTicks = anim->mDuration;

    const double sp = secPerTickOf(anim);
    const bool rotOnly = rotationOnlyEnabled();
    const auto& chans = plan.chanByNode[ai];
    thread_local std::vector<aiMatrix4x4> globals;
    globals.resize(plan.nodes.size());
    // Return the joint GLOBAL (FK world) matrix per bone, NOT global*inverseBind: the BT2018 rig
    // has a different inverse-bind per mesh-part for the same bone, so the caller bakes each
    // part's own offset into its vertices (skin = global(bone) * (offset_part * vertex)).
    for (size_t i = 0; i < plan.nodes.size(); ++i) {
        const aiNode* n = plan.nodes[i];
        aiMatrix4x4 local = n->mTransformation;
        if (const aiNodeAnim* ch = chans[i]) {
            const LocalTRS l = sampleTRS(ch, t, n->mTransformation, rotOnly, sp);
            local = aiMatrix4x4(l.scl, l.rot, l.pos);
        }
        applyLocalRotation(local, i, extra);
        globals[i] = plan.parent[i] >= 0 ? globals[plan.parent[i]] * local : local;
    }
    emitBones(plan, globals, out);
    return true;
}

bool EvaluateClipBlendGpuBones(const SkelPlan& plan,
                               std::string_view clipA, float frameA,
                               std::string_view clipB, float frameB,
                               float weightB,
                               std::vector<GpuBone>& out, const LocalRotation* extra) {
    if (weightB <= 0.0f) return EvaluateClipGpuBones(plan, clipA, frameA, out, nullptr, nullptr, extra);
    if (weightB >= 1.0f) return EvaluateClipGpuBones(plan, clipB, frameB, out, nullptr, nullptr, extra);
    const aiScene* scene = plan.scene;
    const std::optional<int> iaOpt = findAnim(scene, clipA), ibOpt = findAnim(scene, clipB);
    if (!iaOpt || !ibOpt) return false;
    const int ia = *iaOpt, ib = *ibOpt;
    const aiAnimation* aa = scene->mAnimations[ia];
    const aiAnimation* ab = scene->mAnimations[ib];
    const double ta = clipTicks(aa, frameA), tb = clipTicks(ab, frameB);
    const double spa = secPerTickOf(aa), spb = secPerTickOf(ab);
    const bool rotOnly = rotationOnlyEnabled();
    thread_local std::vector<aiMatrix4x4> globals;
    globals.resize(plan.nodes.size());
    for (size_t i = 0; i < plan.nodes.size(); ++i) {
        const aiNode* n = plan.nodes[i];
        const aiNodeAnim* ca = plan.chanByNode[ia][i];
        const aiNodeAnim* cb = plan.chanByNode[ib][i];
        aiMatrix4x4 local = n->mTransformation;
        if (ca || cb) {
            // A clip with no channel for this node contributes the node's rest TRS.
            aiVector3D rs, rp; aiQuaternion rr;
            n->mTransformation.Decompose(rs, rr, rp);
            const LocalTRS A = ca ? sampleTRS(ca, ta, n->mTransformation, rotOnly, spa) : LocalTRS{rs, rr, rp};
            const LocalTRS B = cb ? sampleTRS(cb, tb, n->mTransformation, rotOnly, spb) : LocalTRS{rs, rr, rp};
            aiQuaternion q;
            aiQuaternion::Interpolate(q, A.rot, B.rot, weightB);
            q.Normalize();
            local = aiMatrix4x4(A.scl + (B.scl - A.scl) * weightB, q, A.pos + (B.pos - A.pos) * weightB);
        }
        applyLocalRotation(local, i, extra);
        globals[i] = plan.parent[i] >= 0 ? globals[plan.parent[i]] * local : local;
    }
    emitBones(plan, globals, out);
    return true;
}

void EvaluateRestGpuBones(const SkelPlan& plan, std::vector<GpuBone>& out) {
    thread_local std::vector<aiMatrix4x4> globals;
    globals.resize(plan.nodes.size());
    for (size_t i = 0; i < plan.nodes.size(); ++i)
        globals[i] = plan.parent[i] >= 0 ? globals[plan.parent[i]] * plan.nodes[i]->mTransformation : plan.nodes[i]->mTransformation;
    emitBones(plan, globals, out);
}

// ---- scene-based entry points (harness, one-shot callers): build a plan per call ----
bool EvaluateClipGpuBones(const aiScene* scene,
                          std::string_view clip,
                          float frame,
                          const std::vector<std::string>& boneNames,
                          std::vector<GpuBone>& out,
                          double* outTimeTicks,
                          double* outDurTicks) {
    return EvaluateClipGpuBones(*BuildPlan(scene, boneNames), clip, frame, out, outTimeTicks, outDurTicks);
}

void EvaluateRestGpuBones(const aiScene* scene,
                          const std::vector<std::string>& boneNames,
                          std::vector<GpuBone>& out) {
    EvaluateRestGpuBones(*BuildPlan(scene, boneNames), out);
}

}  // namespace mc2skel
