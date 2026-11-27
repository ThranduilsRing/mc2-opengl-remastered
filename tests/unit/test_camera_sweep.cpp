#include "doctest.h"
#include "camera_frustum_math.h"
#include "camera_elevation.h"
#include "terrain_depth_bias.h"
#include "../../GameOS/gameos/shadow_mover_fit.h"
#include "../../RenderCore/VisibilityHistory.h"
#include "math/mc2_xform.h"
#include "../../3rdparty/glm/glm/gtc/matrix_transform.hpp"

namespace {
float frameLength=1.0f/60;
struct Camera {
    float projectionAngle=45, cameraRotation=0, cameraAltitude=1200;
    float cameraAltitudeDesired=1200, altitudeVelocity=123, newScaleFactor=0;
    float goalRotTime=0;
    glm::vec3 goalRotation{};
    static constexpr float AltitudeMinimum=60;
    static float altitudeMaxForAngle(float angle) {
        return camera_frustum_math::altitudeMaxForAngle(angle,10,90,1500,3000);
    }
    void setCameraRotation(float a,float) { cameraRotation=a; }
    #include "camera_script_set.inc"
    void updateGoalRotation();
};
#include "camera_script_goal.inc"
}

TEST_CASE("camera sweep: scripted altitude holds and goal includes altitude") {
    Camera camera;
    camera.setRotation({18,90,900});
    CHECK(camera.altitudeVelocity==0);
    for(int i=0;i<30;++i) {
        camera.updateGoalRotation();
        camera.cameraAltitude=mc2cam::easeElevation(camera.cameraAltitude,camera.cameraAltitudeDesired,
            camera.altitudeVelocity,frameLength,60,3000);
    }
    CHECK(camera.cameraAltitude==900);
    camera.goalRotation={45,180,1800}; camera.goalRotTime=0.5f;
    for(int i=0;i<31;++i) camera.updateGoalRotation();
    CHECK(camera.cameraAltitudeDesired==doctest::Approx(1800));
}

TEST_CASE("camera sweep: diagonal cone and pick rays across poses") {
    using namespace camera_frustum_math;
    for (int yaw=0; yaw<360; yaw+=45)
    for (float pitch : {10.0f,18.0f,45.0f,88.0f,90.0f})
    for (float altitude : {60.0f,1200.0f,3000.0f})
    for (float fov : {40.0f,55.0f,65.0f,90.0f})
    for (float aspect : {4.0f/3,16.0f/9,21.0f/9,32.0f/9}) {
        const float radians=glm::radians(fov), tx=std::tan(radians/2), ty=tx/aspect;
        const float cone=coneCosine(radians,1/aspect);
        glm::mat4 pose(1);
        mc2::math::buildRotation(pose, mc2::math::Euler(glm::radians(-pitch),glm::radians(float(yaw)),0));
        const auto forward=mc2::math::localBackwardInWorld(pose);
        const auto origin=-forward*altitude;
        mc2::math::buildTranslation(pose,origin);
        // Row-vector reverse-Z ZERO_TO_ONE projection, matching the engine.
        const float near=100, far=50000;
        glm::mat4 projection(0);
        projection[0][0]=1/tx; projection[1][1]=1/ty;
        projection[2][2]=near/(far-near); projection[3][2]=-1;
        projection[2][3]=near*far/(far-near);
        const auto matrix=glm::inverse(pose)*projection;
        const auto inverse=glm::inverse(glm::dmat4(matrix));
        for (int c=0;c<4;++c) for(int r=0;r<4;++r) CHECK(std::isfinite(matrix[c][r]));
        for (float x : {-1.0f,0.0f,1.0f}) for(float y : {-1.0f,0.0f,1.0f}) {
            auto h=glm::dvec4(x,y,1,1)*inverse;
            const auto point=glm::vec3(h/h.w);
            const auto ray=glm::normalize(point-origin);
            CHECK(sphereInCone(glm::dot(ray,forward),cone,0,altitude));
            const auto local=ray*glm::mat3(glm::inverse(pose));
            CHECK(std::fabs(local.x/local.z)<=tx+0.0001f);
            CHECK(std::fabs(local.y/local.z)<=ty+0.0001f);
            if (std::fabs(ray.y)>0.0001f) {
                const auto ground=origin+ray*(-origin.y/ray.y);
                const auto clip=glm::vec4(ground,1)*matrix;
                CHECK(std::fabs(clip.x/clip.w-x)*1920/2<1);
                CHECK(std::fabs(clip.y/clip.w-y)*1080/2<1);
            }
        }
    }
    CHECK(altitudeMaxForAngle(45,45,45,100,200)==100);
    CHECK(sphereInCone(0.7f,0.8f,30,100));
}

TEST_CASE("camera sweep: shadow footprint circle covers tilted views at every yaw") {
    using namespace mc2shadow;
    const auto basis=lightBasis(normalized({1,1,1}));
    const std::array bounds{MoverSphere{{-50000,-50000,0},1000},MoverSphere{{50000,50000,800},1000}};
    for(float pitch : {18.0f,45.0f,88.0f}) {
        float expected=0;
        for(int yaw=0;yaw<360;yaw+=45) {
            glm::mat4 pose(1);
            mc2::math::buildRotation(pose,mc2::math::Euler(glm::radians(-pitch),glm::radians(float(yaw)),0));
            mc2::math::buildTranslation(pose,mc2::math::localForwardInWorld(pose)*1200.0f);
            const float tx=std::tan(glm::radians(40.0f)/2), ty=tx*0.75f;
            glm::mat4 projection(0);
            projection[0][0]=1/tx; projection[1][1]=1/ty;
            projection[2][2]=100.0f/99900; projection[3][2]=-1; projection[2][3]=10000000.0f/99900;
            const auto matrix=glm::inverse(pose)*projection;
            // Ground helper takes MC2 Z-up world rows.
            float rows[16];
            for(int c=0;c<4;++c) {
                rows[c*4]=-matrix[c][0]; rows[c*4+1]=matrix[c][2];
                rows[c*4+2]=matrix[c][1]; rows[c*4+3]=matrix[c][3];
            }
            auto region=moverGroundFootprint(rows,0,50000);
            float band=0; region.radius=moverRadiusBand(region.radius,band);
            const auto window=fitMoverWindow(bounds,basis,0,4096,region);
            if(!expected) expected=window.texelWorld;
            CHECK(window.texelWorld==doctest::Approx(expected).epsilon(0.00001)); // continuous zoom: equal up to float rounding across yaw
            const auto inverse=glm::inverse(glm::dmat4(matrix));
            for(float x : {-1.0f,1.0f}) for(float y : {-1.0f,1.0f}) {
                auto h=glm::dvec4(x,y,1,1)*inverse;
                const glm::vec3 origin=mc2::math::getTranslation(pose), ray=glm::vec3(h/h.w)-origin;
                const auto hit=origin+ray*(-origin.y/ray.y);
                CHECK(glm::length(glm::vec2(hit.x,hit.z))<=region.radius+0.1f);
            }
        }
    }
}

TEST_CASE("camera sweep: light sphere and current actor survive an offscreen centre and lag") {
    float planes[6][4]={{1,0,0,1},{-1,0,0,1},{0,1,0,1},{0,-1,0,1},{0,0,1,0},{0,0,-1,10}};
    const float light[3]={2,0,5};
    CHECK_FALSE(camera_frustum_math::sphereInFrustum(planes,light,0));
    CHECK(camera_frustum_math::sphereInFrustum(planes,light,1.1f));
    RenderCore::VisibilityHistory history(1);
    history.consume([](uint32_t, auto& sample) { sample.count=1; sample.ids[0]=1; sample.bits[0]=0; return true; });
    history.buildSnapshot(2,false);
    REQUIRE_FALSE(history.isVisible(1));
    // A yaw of 3 degrees per frame carries this point inside the live right plane.
    for(int lag=1;lag<=2;++lag) {
        const float angle=glm::radians(3.0f*lag);
        planes[1][0]=-std::cos(angle); planes[1][2]=std::sin(angle);
        const float actor[3]={1.05f,0,5};
        CHECK((history.isVisible(1) || camera_frustum_math::sphereInFrustum(planes,actor,0.05f)));
    }
}

TEST_CASE("camera sweep: layer world push is proportional and ordered at every near plane") {
    using namespace mc2depth;
    for(float distance : {200.0f,1000.0f,3000.0f,6000.0f})
    for(float near : {100.0f,250.0f,400.0f}) {
        float previous=0;
        for(float layer : {VEG_DEPTH_BIAS_REL,OVERLAY_DEPTH_BIAS_REL,OBJECT_DEPTH_BIAS_REL,WATER_DEPTH_FUDGE_FAST_REL}) {
            const float push=worldPush(layer,distance,near);
            CHECK(push>previous);
            CHECK(push/distance==doctest::Approx(worldPush(layer,1000,400)/1000).epsilon(0.01));
            previous=push;
        }
    }
}

TEST_CASE("camera sweep: segments clip at reverse Z near before division") {
    for(int yaw : {0,90,180,270}) for(float pitch : {18.0f,45.0f,88.0f})
    for(float zoom : {300.0f,1200.0f,3000.0f}) {
        glm::mat4 pose(1);
        mc2::math::buildRotation(pose,mc2::math::Euler(glm::radians(-pitch),glm::radians(float(yaw)),0));
        mc2::math::buildTranslation(pose,mc2::math::localForwardInWorld(pose)*zoom);
        glm::mat4 projection(0);
        projection[0][0]=2; projection[1][1]=3;
        projection[2][2]=100.0f/49900; projection[3][2]=-1; projection[2][3]=5000000.0f/49900;
        const auto matrix=glm::inverse(pose)*projection;
        auto a=(glm::vec4(20,10,-200,1)*pose)*matrix;
        auto b=(glm::vec4(-10,30,100,1)*pose)*matrix;
        const glm::vec2 start=glm::vec2(a)/a.w, end=glm::vec2(b)/b.w;
        REQUIRE(camera_frustum_math::clipSegmentNear(a,b));
        CHECK(b.w==doctest::Approx(b.z).epsilon(0.0001));
        const auto cut=glm::vec2(b)/b.w, line=end-start, delta=cut-start;
        CHECK(std::fabs(line.x*delta.y-line.y*delta.x)<0.0001f);
        CHECK(std::isfinite(cut.x)); CHECK(std::isfinite(cut.y));
        const auto originalA=a,originalB=b;
        REQUIRE(camera_frustum_math::clipSegmentNear(a,b));
        CHECK(a==originalA); CHECK(b==originalB);
        a=glm::vec4(0,0,200,100); b=glm::vec4(0,0,300,-100);
        CHECK_FALSE(camera_frustum_math::clipSegmentNear(a,b));
    }
}
