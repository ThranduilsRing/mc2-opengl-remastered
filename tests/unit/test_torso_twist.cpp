#include "doctest.h"
#include "mech_skel_import.h"
#include "math/mc2_xform.h"
#include <assimp/scene.h>
#include <cmath>

namespace {
struct TwistScene {
    aiScene scene;
    std::vector<std::string> names{"j_Spine2", "muzzle", "j_LFoot", "j_RFoot"};
    TwistScene() {
        auto* root = scene.mRootNode = new aiNode("pelvis");
        root->mNumChildren = 3;
        root->mChildren = new aiNode*[3]{new aiNode("j_Spine2"), new aiNode("j_LFoot"), new aiNode("j_RFoot")};
        for (unsigned i = 0; i < 3; ++i) root->mChildren[i]->mParent = root;
        auto* spine = root->mChildren[0];
        // Nonidentity bind orientation: local X is source/MC2 +Y.
        aiMatrix4x4::RotationZ(1.5707963268f, spine->mTransformation);
        spine->mTransformation.b4 = 2;
        spine->mNumChildren = 1;
        spine->mChildren = new aiNode*[1]{new aiNode("muzzle")};
        spine->mChildren[0]->mParent = spine;
        spine->mChildren[0]->mTransformation.c4 = -1;
        root->mChildren[1]->mTransformation.a4 = -1;
        root->mChildren[2]->mTransformation.a4 = 1;
        scene.mNumAnimations = 2;
        scene.mAnimations = new aiAnimation*[2]{new aiAnimation, new aiAnimation};
        for (unsigned a = 0; a < 2; ++a) {
            auto* clip = scene.mAnimations[a];
            clip->mName = a == 0 ? "atlas_torsoTwistLeft" : "atlas_torsoTwistRight";
            clip->mDuration = 1; clip->mTicksPerSecond = 1;
            clip->mNumChannels = 1;
            auto* ch = new aiNodeAnim;
            clip->mChannels = new aiNodeAnim*[1]{ch};
            ch->mNodeName = "j_Spine2";
            ch->mNumRotationKeys = 2;
            aiVector3D s, p; aiQuaternion rest;
            spine->mTransformation.Decompose(s, rest, p);
            const auto key = rest * aiQuaternion(aiVector3D(1, 0, 0), a == 0 ? -0.8f : 0.8f);
            ch->mRotationKeys = new aiQuatKey[2]{{0, rest}, {1, key}};
        }
    }
};
}

TEST_CASE("torso twist: production single and blended FK rotate muzzle but preserve feet") {
    TwistScene fixture;
    const auto plan = mc2skel::BuildPlan(&fixture.scene, fixture.names);
    auto twist = mc2skel::ResolveTorsoTwist(*plan);
    REQUIRE(twist.node >= 0);
    CHECK(twist.axis[0] == doctest::Approx(1)); // negative Left key still matches stock positive yaw
    twist.degrees = 30;
    std::vector<mc2skel::GpuBone> rest, pose;
    REQUIRE(mc2skel::EvaluateClipGpuBones(*plan, "atlas_torsoTwistLeft", 0, rest));
    for (float blend : {-1.0f, 0.0f, 0.5f, 1.0f}) {
        if (blend < 0)
            REQUIRE(mc2skel::EvaluateClipGpuBones(*plan, "atlas_torsoTwistLeft", 0, pose, nullptr, nullptr, &twist));
        else
            REQUIRE(mc2skel::EvaluateClipBlendGpuBones(*plan, "atlas_torsoTwistLeft", 0,
                "atlas_torsoTwistRight", 0, blend, pose, &twist));
        // Case 2 maps source -Z to shape +Z; stock uses row vectors and radians.
        const auto stock = glm::vec4(0, 0, 1, 0) * mc2::math::rotationMatrix(mc2::math::Euler(0, 0.5235987756f, 0));
        CHECK(-pose[1].m[3] == doctest::Approx(stock.x));
        CHECK(-pose[1].m[11] == doctest::Approx(stock.z));
        CHECK(pose[1].m[3] == doctest::Approx(-0.5f));
        CHECK(pose[1].m[7] == doctest::Approx(2));
        CHECK(pose[1].m[11] == doctest::Approx(-std::sqrt(0.75f)));
        for (size_t foot : {2u, 3u})
            for (int k = 0; k < 16; ++k) CHECK(pose[foot].m[k] == rest[foot].m[k]);
    }
    // The named torso works without diagnostic clips.
    for (auto* clip : {fixture.scene.mAnimations[0], fixture.scene.mAnimations[1]}) clip->mName = "other";
    const auto fallback = mc2skel::ResolveTorsoTwist(*mc2skel::BuildPlan(&fixture.scene, fixture.names));
    CHECK(fallback.node == twist.node);
    CHECK(fallback.axis[0] == doctest::Approx(1));
    fixture.scene.mRootNode->mChildren[0]->mName = "j_Pitch";
    CHECK(mc2skel::ResolveTorsoTwist(*mc2skel::BuildPlan(&fixture.scene, fixture.names)).node == -1);
}

TEST_CASE("torso twist: pelvis sway cannot capture named or largest descendant spine yaw") {
    TwistScene f;
    auto* pelvis = f.scene.mRootNode;
    auto* torso = pelvis->mChildren[0];
    auto* lower = new aiNode("j_Spine");
    lower->mParent = pelvis;
    lower->mNumChildren = 1;
    lower->mChildren = new aiNode*[1]{torso};
    torso->mParent = lower;
    pelvis->mChildren[0] = lower;
    for (auto* clip : {f.scene.mAnimations[0], f.scene.mAnimations[1]}) {
        auto* authored = clip->mChannels[0];
        delete[] clip->mChannels;
        clip->mNumChannels = 3;
        clip->mChannels = new aiNodeAnim*[3]{new aiNodeAnim, new aiNodeAnim, authored};
        for (unsigned c = 0; c < 2; ++c) {
            auto* ch = clip->mChannels[c];
            ch->mNodeName = c == 0 ? "pelvis" : "j_Spine";
            ch->mNumRotationKeys = 1;
            ch->mRotationKeys = new aiQuatKey[1]{{0, aiQuaternion(aiVector3D(0, 1, 0), c == 0 ? 1.2f : 1.0f)}};
        }
    }
    auto resolve = [&] { return mc2skel::ResolveTorsoTwist(*mc2skel::BuildPlan(&f.scene, f.names)); };
    CHECK(resolve().node == 2); // named Spine2 wins over pelvis and lower spine
    torso->mName = "j_Spine3";
    for (auto* clip : {f.scene.mAnimations[0], f.scene.mAnimations[1]}) {
        clip->mChannels[2]->mNodeName = "j_Spine3";
        clip->mChannels[1]->mRotationKeys[0].mValue = aiQuaternion(aiVector3D(0, 1, 0), 0.2f);
    }
    CHECK(resolve().node == 2); // 0.8 rad wins over the first spine's 0.2 rad
    pelvis->mName = "unrelated_root";
    CHECK(resolve().node == -1); // no unverified root/subtree fallback
}
