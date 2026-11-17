#include "doctest.h"
#include "camera_elevation.h"
#include <cstdio>

// Trigger paths: camera_elevation.h, camera_input.cpp, missiongui_orders.cpp.
TEST_CASE("CameraElevation/wheel notches retain monotonic intermediate altitudes") {
    for (float bound : {60.0f, 6000.0f}) for (float speed : {1.0f, 2.0f}) {
        float reference = 0;
        for (int fps : {30, 60, 144}) {
            float altitude = 3000, target = altitude, velocity = 0;
            for (int notch = 0; notch < 12; ++notch) {
                const float previous = altitude;
                const float scale = 1.0f - (altitude - 60) / 6000;
                target = mc2cam::zoomElevationTarget(target, bound,
                    mc2cam::wheelZoomAmount(10 * speed, 15, scale), scale, 1);
                for (int frame = 0; frame < fps; ++frame)
                    altitude = mc2cam::easeElevation(altitude, target, velocity, 1.0f / fps, 60, 6000);
                CHECK((altitude - previous) * (bound - previous) > 0);
                CHECK(altitude > 60);
                CHECK(altitude < 6000);
            }
            if (fps == 30) reference = altitude;
            CHECK(altitude == doctest::Approx(reference).epsilon(0.00001));
        }
    }
    CHECK(mc2cam::zoomElevationTarget(3000, 60, 1e6f, 0, 5) == 2265);
}

TEST_CASE("CameraElevation/edge and keyboard pan stay bounded at far zoom and stalls") {
    for (float scale : {0.0f, 0.01f, 0.1f, 0.5f, 1.0f}) {
        CHECK(mc2cam::panStep(10000, scale, 1, 15) <= doctest::Approx(1000.0f / 3));
        CHECK(mc2cam::panStep(10000, scale, -1, 15) == 0);
        const float step = mc2cam::panStep(10000, scale, 1.0f / 60, 15);
        CHECK(step * 2 == doctest::Approx(mc2cam::panStep(10000, scale, 1.0f / 30, 15)));
    }
}

TEST_CASE("CameraElevation/exact damped step agrees at 30 60 and 144 fps") {
    for (float target : {10.0f, 88.0f, 60.0f}) {
        float reference = 45, refVelocity = 0;
        reference = mc2cam::easeElevation(reference, target, refVelocity, 0.5f, -20, 88);
        for (int fps : {30, 60, 144}) {
            float value = 45, velocity = 0;
            for (int frame = 0; frame < fps / 2; ++frame) {
                const float prev = value;
                value = mc2cam::easeElevation(value, target, velocity, 1.0f / fps, -20, 88);
                CHECK(value >= std::min(45.0f, target));
                CHECK(value <= std::max(45.0f, target));
                CHECK(std::abs(value - target) <= std::abs(prev - target));
            }
            CHECK(value == doctest::Approx(reference).epsilon(0.00001));
            for (int frame = 0; frame < fps * 2; ++frame)
                value = mc2cam::easeElevation(value, target, velocity, 1.0f / fps, -20, 88);
            CHECK(value == doctest::Approx(target).epsilon(0.00001));
        }
    }
}

TEST_CASE("CameraElevation/clamps targets and moving bounds without overshoot") {
    float value = 40, velocity = 0;
    for (float target : {200.0f, -200.0f, 55.0f}) {
        for (int i = 0; i < 180; ++i) {
            value = mc2cam::easeElevation(value, target, velocity, 1.0f / 60, -20, 88);
            CHECK(value >= -20);
            CHECK(value <= 88);
        }
        CHECK(value == doctest::Approx(std::clamp(target, -20.0f, 88.0f)).epsilon(0.00001));
    }
    CHECK(mc2cam::easeElevation(500, 900, velocity, 0, 60, 400) == 400);
}

TEST_CASE("CameraElevation/per-frame before after pitch and altitude trace") {
    float pitch = 45, pv = 0, altitude = 1200, av = 0, beforeAlt = altitude;
    std::printf("elevation_trace,frame,dt,before_pitch,after_pitch,before_alt,after_alt\n");
    for (int frame = 0; frame < 120; ++frame) {
        const float prev = pitch;
        pitch = mc2cam::easeElevation(pitch, 46.125f, pv, 1.0f / 60, -20, 88);
        altitude = mc2cam::easeElevation(altitude, 1100, av, 1.0f / 60, 60, 6000);
        beforeAlt += (1100 - beforeAlt) * (1.0f - std::exp(-1.0f / 6));
        CHECK(pitch - prev < 0.15f);
        std::printf("elevation_trace,%d,0.016666667,46.125,%.6f,%.6f,%.6f\n", frame, pitch, beforeAlt, altitude);
    }
}
