#include <algorithm>
#include "tacticaloverview.h"
#include <cstdlib>
#include <cstdio>
#include <cmath>
// DEBUGWINS_print is defined in code/ablmc2.cpp (not exported via a header).
// Use OutputDebugStringA as the debug log sink for this TU.
#include <windows.h>
#include "../mclib/camera.h"
#include "../mclib/appear.h"   // DRAW_BARS (maskWorldBars)
#include "Gates/gate.h"
#include "mc2fmt.h"

TacticalOverview g_tacticalOverview;

unsigned long TacticalOverview::maskWorldBars( unsigned long drawFlags )
{
    if ( g_tacticalOverview.active() )
        drawFlags &= ~static_cast<unsigned long>(DRAW_BARS);
    return drawFlags;
}

// Overview camera envelope. The engine clamps cameraAltitude to testMax (derived
// from AltitudeMaximumLo/Hi) every frame in GameCamera::update(), so to pull back
// past the normal zoom-out stop we must RAISE that ceiling while active and drive
// the altitude well above the gameplay cap (~6400). projectionAngle: 10=shallow ..
// 35=normal .. 88=max top-down; we hold a steep near-top-down read.
static const float kOverviewAltitude = 28000.0f;  // target pull-back altitude
static const float kOverviewCeiling  = 32000.0f;  // raised AltitudeMaximum* while active (> altitude)
static const float kOverviewTiltAngle = 85.0f;    // steep, just under singular 88

static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

bool TacticalOverview::enabled() {
    // Default ON; opt out with MC2_TACTICAL_OVERVIEW=0.
    static int cached = -1;
    if (cached < 0) {
        cached = mc2::gate::get<"MC2_TACTICAL_OVERVIEW">();
    }
    return cached != 0;
}

bool TacticalOverview::tintEnabled() {
    if (!enabled()) return false;
    static int cached = -1;
    if (cached < 0) {
        cached = mc2::gate::get<"MC2_TACTICAL_OVERVIEW_TINT">();   // default ON; only "=0" disables
    }
    return cached != 0;
}

void TacticalOverview::onWheel(long delta, bool atCeiling, float dt, bool worldOwnsWheel) {
    if (!enabled()) return;
    state_.applyWheel(delta, atCeiling, dt, worldOwnsWheel);
}

void TacticalOverview::onHotkey() {
    if (!enabled()) return;
    ++hotkeyFires_;
    state_.toggleHotkey();
    // Formation line never survives an overview toggle.
    flOnCancel();
}

void TacticalOverview::setCardHits(const CardHit* hits, int n) {
    n = std::clamp(n, 0, kMaxCardHits);
    for (int i = 0; i < n; ++i) cardHit_[i] = hits[i];
    cardHitCount_ = n;
}

const TacticalOverview::CardHit* TacticalOverview::cardHitAt(float x, float y) const {
    for (int i = 0; i < cardHitCount_; ++i)
        if (x >= cardHit_[i].l && x <= cardHit_[i].r && y >= cardHit_[i].t && y <= cardHit_[i].b)
            return &cardHit_[i];
    return 0;
}

void TacticalOverview::advance(float dt) {
    if (!enabled()) return;
    float before = state_.t();
    state_.update(dt);
    if (before != state_.t() && mc2::gate::get<"MC2_TACTICAL_OVERVIEW_DEBUG">()) {
        char buf[96];
        sprintf_s(buf, sizeof(buf), "[TacOverview] t=%.3f iconA=%.3f", state_.t(), state_.iconAlpha());
        mc2::fmt::debug_out(buf);
    }
}

void TacticalOverview::driveCamera(Camera* eye) {
    if (!enabled() || !eye) return;
    const float t = state_.t();

    // Capture the gameplay altitude + tilt + zoom-out ceiling exactly once, on
    // first activation.
    if (t > 0.0f && !returnSnap_.valid) {
        returnSnap_.valid    = true;
        returnSnap_.altitude = eye->getCameraAltitude();
        returnSnap_.tilt     = eye->getProjectionAngle();
        returnSnap_.maxAltLo = Camera::AltitudeMaximumLo;
        returnSnap_.maxAltHi = Camera::AltitudeMaximumHi;
        userPannedInOverview_ = false;
    }

    if (t > 0.0f) {
        // Raise the altitude ceiling so GameCamera::update() won't clamp our
        // pulled-back altitude back down to the gameplay testMax. Setting both
        // Lo and Hi to the overview ceiling makes testMax == ceiling at any tilt.
        Camera::AltitudeMaximumLo = kOverviewCeiling;
        Camera::AltitudeMaximumHi = kOverviewCeiling;
        // EXPONENTIAL altitude blend: alt = a0 * (target/a0)^t. Linear t steps
        // (uniform wheel notches) multiply altitude by a constant factor, so the
        // zoom feels the same rate at 6k and at 28k — no sensitivity jump.
        float a0 = returnSnap_.altitude < 60.0f ? 60.0f : returnSnap_.altitude;
        float alt = a0 * powf(kOverviewAltitude / a0, t);
        eye->zoomValue(alt);
        eye->tiltValue(lerpf(returnSnap_.tilt,     kOverviewTiltAngle, t));
    } else if (returnSnap_.valid) {
        // Fully exited (t == 0): restore the gameplay ceiling always, and restore
        // altitude + tilt unless the user manually re-framed in overview.
        // Position/rotation were never touched.
        Camera::AltitudeMaximumLo = returnSnap_.maxAltLo;
        Camera::AltitudeMaximumHi = returnSnap_.maxAltHi;
        if (!userPannedInOverview_) {
            eye->zoomValue(returnSnap_.altitude);
            eye->tiltValue(returnSnap_.tilt);
        }
        returnSnap_.valid = false;
        userPannedInOverview_ = false;
    }
}

// Formation line env gate (MC2_TACMAP_FORMATION_LINE=1), cached on first call.
bool TacticalOverview::formationLineEnabled()
{
    static int cached = -1;
    if ( cached < 0 )
    {
        cached = mc2::gate::get<"MC2_TACMAP_FORMATION_LINE">();
    }
    return cached == 1;
}

void TacticalOverview::flOnHotkeyL()
{
    if ( !formationLineEnabled() || !state_.active() )
        return;
    if ( flState_ == FL_IDLE )
        flState_ = FL_ARMED;
    else
        flOnCancel();           // L again disarms (covers ARMED and mid-drag)
}

void TacticalOverview::flOnCancel()
{
    flState_ = FL_IDLE;
    flMoverCount_ = 0;
}

void TacticalOverview::flOnDragStart( const glm::vec3& worldStart )
{
    if ( flState_ != FL_ARMED )
        return;
    flStart_ = worldStart;
    flEnd_   = worldStart;
    flState_ = FL_DRAGGING;
}

void TacticalOverview::flOnDragMove( const glm::vec3& worldEnd )
{
    if ( flState_ == FL_DRAGGING )
        flEnd_ = worldEnd;
}

void TacticalOverview::flOnRelease()
{
    flState_ = FL_IDLE;
    flMoverCount_ = 0;
}

void TacticalOverview::flSetMovers( Mover* const* movers, int n )
{
    n = std::clamp(n, 0, kFlMaxMovers);
    for ( int i = 0; i < n; i++ )
        flMovers_[i] = movers[i];
    flMoverCount_ = n;
}

void TacticalOverview::flAdjustSpacing( long wheelDelta )
{
    if ( wheelDelta == 0 )
        return;
    const float kStep = 0.15f, kMin = 0.30f, kMax = 2.50f;
    flSpacing_ += ( wheelDelta > 0 ) ? kStep : -kStep;
    flSpacing_ = std::clamp(flSpacing_, kMin, kMax);
}

int TacticalOverview::flComputeSlots( glm::vec3* outSlots, int maxSlots ) const
{
    int n = flMoverCount_;
    if ( n <= 0 || maxSlots <= 0 )
        return 0;
    if ( n > maxSlots ) n = maxSlots;
    // Midpoint of the drawn line; slots scale about it by flSpacing_.
    const float midX = ( flStart_.x + flEnd_.x ) * 0.5f;
    const float midY = ( flStart_.y + flEnd_.y ) * 0.5f;
    const float midZ = ( flStart_.z + flEnd_.z ) * 0.5f;
    if ( n == 1 )
    {
        outSlots[0].x = midX;
        outSlots[0].y = midY;
        outSlots[0].z = midZ;
        return 1;
    }
    for ( int i = 0; i < n; i++ )
    {
        float t = static_cast<float>(i) / static_cast<float>(( n - 1 ));
        // Raw evenly spaced point, then stretch about the midpoint.
        float rx = flStart_.x + ( flEnd_.x - flStart_.x ) * t;
        float ry = flStart_.y + ( flEnd_.y - flStart_.y ) * t;
        float rz = flStart_.z + ( flEnd_.z - flStart_.z ) * t;
        outSlots[i].x = midX + ( rx - midX ) * flSpacing_;
        outSlots[i].y = midY + ( ry - midY ) * flSpacing_;
        outSlots[i].z = midZ + ( rz - midZ ) * flSpacing_;
    }
    return n;
}
