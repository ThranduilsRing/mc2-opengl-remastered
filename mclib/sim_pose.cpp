// Simulation pose capture and deterministic fork/join publication. Kept out
// of msl_transform.cpp, which the game-free importer also compiles.
#include "msl_internal.h"
#include "sim_pose_kernel.h"
#include "../code/job_system.h"

long TG_MultiShape::TransformSimPose(glm::vec3* pos, glm::quat* rot)
{
    ZoneScopedN("GameLogic.SimPose.Kernel");
    // The synchronous join is the revision/lifetime boundary: no mover,
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
    static thread_local std::vector<int> dirty;
    inputs.resize(numTG_Shapes);
    outputs.resize(numTG_Shapes);
    dirty.clear();
    for (int i = 0; i < numTG_Shapes; ++i) {
        const auto& rec = listOfShapes[i];
        auto& in = inputs[i];
        in.parent = rec.parentNode ? static_cast<int>(rec.parentNode - listOfShapes) : -1;
        in.current = rec.calcedThisFrame == revision;
        if (in.current) {
            in.cached = {rec.localShapeToWorld, rec.shapeToWorld, rec.worldToShape};
            continue;
        }
        dirty.push_back(i);
        in.base = rec.baseRotation;
        in.animated = rec.currentAnimation != nullptr;
        in.animation = in.animated && rec.currentAnimation->quat
            ? rec.currentAnimation->quat[frame] : mc2::math::quatXYZW(0.0f, 0.0f, 0.0f, 1.0f);
        in.translation = in.animated && rec.currentAnimation->pos
            ? rec.currentAnimation->pos[frame]
            : (in.parent < 0 ? rec.node->myType->GetNodeCenter() : rec.node->myType->GetRelativeNodeCenter());
    }
    const std::span<const mc2::sim::PoseNodeInput> captured(inputs);
    const std::span<const int> order(dirty);
    const std::span<mc2::sim::PoseMatrices> results(outputs);
    mc2::jobs::parallel_for(mc2::jobs::Domain::Sim, static_cast<int>(order.size()), 8, 1000,
        [captured, order, results, origin](int begin, int end) {
            for (int n = begin; n < end; ++n) {
                const int i = order[n];
                results[i] = mc2::sim::poseNode(captured, origin, i);
            }
        });
    // Canonical node order, independent of worker count. Workers never touch TG.
    for (int i = 0; i < numTG_Shapes; ++i) {
        auto& rec = listOfShapes[i];
        if (!inputs[i].current) {
            rec.localShapeToWorld = outputs[i].local;
            rec.shapeToWorld = outputs[i].world;
            rec.worldToShape = outputs[i].inverse;
            rec.calcedThisFrame = revision;
        }
        if (rec.node) rec.node->lastTurnTransformed = revision;
    }
    return 0;
}
