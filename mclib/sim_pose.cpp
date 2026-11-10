// Simulation pose capture and deterministic serial publication. Kept out
// of msl_transform.cpp, which the game-free importer also compiles.
#include "msl_internal.h"
#include "sim_pose_kernel.h"

void TG_MultiShape::CaptureSimPose(glm::vec3* pos, glm::quat* rot, mc2::sim::PoseJob& job)
{
    // Capture is the revision/lifetime boundary: the job owns copies of every input, so no mover, animation, shape
    // generation or turn change after this point can reach the evaluation; publication writes back in node order.
    job.revision = turn;
    job.done = false;
    const long frame = float2long(frameNum);
    mc2::math::buildRotation(job.origin, *rot);
    mc2::math::buildTranslation(job.origin, *pos);
    BindMaterials(); // material registry writes must stay on the sim thread

    job.inputs.resize(numTG_Shapes);
    job.outputs.resize(numTG_Shapes);
    job.evaluated.resize(numTG_Shapes);
    for (int i = 0; i < numTG_Shapes; ++i) {
        const auto& rec = listOfShapes[i];
        auto& in = job.inputs[i];
        in.parent = rec.parentNode ? static_cast<int>(rec.parentNode - listOfShapes) : -1;
        in.current = rec.calcedThisFrame == job.revision;
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
}

void TG_MultiShape::PublishSimPose(const mc2::sim::PoseJob& job)
{
    // Canonical node order, independent of worker count.
    for (int i = 0; i < numTG_Shapes; ++i) {
        auto& rec = listOfShapes[i];
        if (!job.inputs[i].current) {
            rec.localShapeToWorld = job.outputs[i].local;
            rec.shapeToWorld = job.outputs[i].world;
            if (job.inputs[i].inverseRequired) rec.worldToShape = job.outputs[i].inverse;
            rec.calcedThisFrame = job.revision;
        }
        if (rec.node) rec.node->lastTurnTransformed = job.revision;
    }
}

long TG_MultiShape::TransformSimPose(glm::vec3* pos, glm::quat* rot)
{
    ZoneScopedN("GameLogic.SimPose.Kernel");
    // Reuse capture storage without adding per-instance state or save bytes.
    static thread_local mc2::sim::PoseJob job;
    CaptureSimPose(pos, rot, job);
    job.evaluate();
    PublishSimPose(job);
    return 0;
}
