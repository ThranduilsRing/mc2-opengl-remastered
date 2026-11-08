// Simulation pose capture and deterministic serial publication. Kept out
// of msl_transform.cpp, which the game-free importer also compiles.
#include "msl_internal.h"
#include "sim_pose_kernel.h"

long TG_MultiShape::TransformSimPose(glm::vec3* pos, glm::quat* rot)
{
    ZoneScopedN("GameLogic.SimPose.Kernel");
    // Synchronous evaluation is the revision/lifetime boundary: no mover,
    // animation, shape generation or turn can change before publication.
    const int32_t revision = turn;
    const long frame = float2long(frameNum);
    glm::mat4 origin;
    mc2::math::buildRotation(origin, *rot);
    mc2::math::buildTranslation(origin, *pos);
    BindMaterials(); // material registry writes must stay on the sim thread

    // Reuse capture storage without adding per-instance state or save bytes.
    static thread_local std::vector<mc2::sim::PoseNodeInput> inputs;
    static thread_local std::vector<mc2::sim::PoseMatrices> outputs;
    static thread_local std::vector<unsigned char> evaluated;
    inputs.resize(numTG_Shapes);
    outputs.resize(numTG_Shapes);
    evaluated.resize(numTG_Shapes);
    for (int i = 0; i < numTG_Shapes; ++i) {
        const auto& rec = listOfShapes[i];
        auto& in = inputs[i];
        in.parent = rec.parentNode ? static_cast<int>(rec.parentNode - listOfShapes) : -1;
        in.current = rec.calcedThisFrame == revision;
        // Only private mech/vehicle simulation shapes call this entry point.
        // Their anchor queries read shapeToWorld; CpuDraw's inverse consumer
        // uses the separate presentation shapes and TransformMultiShape.
        in.inverseRequired = false;
        if (in.current) {
            in.cached = {rec.localShapeToWorld, rec.shapeToWorld, rec.worldToShape};
            continue;
        }
        in.base = rec.baseRotation;
        in.animated = rec.currentAnimation != nullptr;
        in.animation = in.animated && rec.currentAnimation->quat
            ? rec.currentAnimation->quat[frame] : mc2::math::quatXYZW(0.0f, 0.0f, 0.0f, 1.0f);
        in.translation = in.animated && rec.currentAnimation->pos
            ? rec.currentAnimation->pos[frame]
            : (in.parent < 0 ? rec.node->myType->GetNodeCenter() : rec.node->myType->GetRelativeNodeCenter());
    }
    mc2::sim::poseNodes(inputs, origin, outputs, evaluated);
    // Canonical node order, independent of worker count.
    for (int i = 0; i < numTG_Shapes; ++i) {
        auto& rec = listOfShapes[i];
        if (!inputs[i].current) {
            rec.localShapeToWorld = outputs[i].local;
            rec.shapeToWorld = outputs[i].world;
            if (inputs[i].inverseRequired) rec.worldToShape = outputs[i].inverse;
            rec.calcedThisFrame = revision;
        }
        if (rec.node) rec.node->lastTurnTransformed = revision;
    }
    return 0;
}
