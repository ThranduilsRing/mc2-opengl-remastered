// The field searchlight is an operator-controlled visual, independent of the sim pose.
#include "bdactor_internal.h"
#include "objmgr.h"
#include "mech.h"
#include "team.h"
#include "searchlight_aim.h"

void BldgAppearance::updateSearchlight()
{
    if (!appearType || !bldgShape || S_stricmp(appearType->elevationNodeId, "NONE") == 0)
        return;
    const long yawNode = bldgShape->GetNodeNameId(appearType->rotationalNodeId);
    const long pitchNode = bldgShape->GetNodeNameId(appearType->elevationNodeId);
    if (yawNode < 0 || pitchNode < 0) return;

    // Only the presentation branch calls this. Destroyed/disabled lamps retain their last pose.
    if (status == OBJECT_STATUS_NORMAL && !forceLightsOut) {
        const float dt = std::clamp(frameLength, 0.0f, 0.1f);
        searchlightTime_ += dt;
        const float phase = position.x * 0.013f + position.y * 0.007f;
        float yaw = 65.0f * std::sin(searchlightTime_ * 0.12f + phase);
        float pitch = 14.0f + 6.0f * std::sin(searchlightTime_ * 0.09f + phase);
        glm::vec3 lamp = position + glm::vec3(0.0f, 0.0f, 85.0f);
        mc2_cone_glow::NodeBeam beam;
        if (mc2_cone_glow::GetSearchlightBeam(bldgShape, pitchNode, beam)) lamp = beam.apex;
        float best = 400.0f * 400.0f;
        if (ObjectManager && Team::home) {
            for (long i = 0; i < ObjectManager->getNumMechs(); ++i) {
                auto* mech = ObjectManager->getMech(i);
                if (!mech || !mech->getExists() || mech->isDestroyed() || mech->isDisabled()) continue;
                // A hidden sensor contact must never be disclosed by a beam.
                const bool friendly = mech->isFriendly(Team::home);
                if (!friendly && (!mech->getContactInfo() ||
                    mech->getContactInfo()->getContactStatus(Team::home->getId(), true) != CONTACT_VISUAL)) continue;
                const glm::vec3 delta = mech->getPosition() + glm::vec3(0, 0, 12) - lamp;
                const float distance2 = glm::dot(delta, delta);
                if (distance2 > 400.0f * 400.0f) continue;
                const float score = distance2 * (friendly ? 1.0f : 0.65f);
                if (score >= best) continue;
                best = score;
                yaw = std::atan2(-delta.x, delta.y) * RADS_TO_DEGREES - rotation;
                pitch = std::clamp(-std::atan2(delta.z, std::hypot(delta.x, delta.y)) * RADS_TO_DEGREES,
                    -10.0f, 55.0f);
            }
        }
        searchlightYaw_ = mc2::searchlight::approachAngle(searchlightYaw_, yaw, 18.0f, dt);
        searchlightPitch_ = mc2::searchlight::approachAngle(searchlightPitch_, pitch, 10.0f, dt);
    }
    auto yaw = mc2::math::quatFromEuler(mc2::math::Euler(0, searchlightYaw_ * DEGREES_TO_RADS, 0));
    auto pitch = mc2::math::quatFromEuler(mc2::math::Euler(searchlightPitch_ * DEGREES_TO_RADS, 0, 0));
    bldgShape->SetNodeRotation(yawNode, &yaw);
    bldgShape->SetNodeRotation(pitchNode, &pitch);
    auto root = mc2::math::quatFromEuler(mc2::math::Euler(0, rotation * DEGREES_TO_RADS, 0));
    glm::vec3 translation; mc2ToRender(translation, position);
    // Lights and glow below must sample THIS frame's lamp hierarchy.
    bldgShape->TransformMultiShape_HierarchyOnly(&translation, &root);
    if (mc2::gate::get<"MC2_LIGHT_CONE_DIAG">()) {
        static unsigned count = 0;
        mc2_cone_glow::NodeBeam beam;
        if ((count++ % 120) < 2 && mc2_cone_glow::GetSearchlightBeam(bldgShape, pitchNode, beam))
            mc2::fmt::print_err("[SEARCHLIGHT] x={:.1f} y={:.1f} yaw={:.2f} pitch={:.2f} apex=({:.1f},{:.1f},{:.1f}) aim=({:.3f},{:.3f},{:.3f})\n",
                position.x, position.y, searchlightYaw_, searchlightPitch_, beam.apex.x, beam.apex.y, beam.apex.z,
                beam.aim.x, beam.aim.y, beam.aim.z);
    }
}
