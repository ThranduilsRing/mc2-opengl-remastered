#ifndef TACTICALOVERVIEW_H
#define TACTICALOVERVIEW_H
// Engine glue for the Tactical Overview camera. Owns the pure blend-state,
// reads env flags, drives the Camera via its public API, draws the 2D overlay.
#include "tacticaloverview_state.h"
#include <span>
#include <stuff/stuff.hpp>

class Camera;
class Mover;

class TacticalOverview {
public:
    // True iff MC2_TACTICAL_OVERVIEW is set (master gate). Cached on first call.
    static bool enabled();
    // True iff the friendly-coverage tint is enabled (default ON under master,
    // killable via MC2_TACTICAL_OVERVIEW_TINT=0).
    static bool tintEnabled();

    // Called from the camera-input site every frame.
    void onWheel(long delta, bool atCeiling, float dtSeconds, bool worldOwnsWheel);
    void onHotkey();                 // hotkey pressed (snap-toggle)
    void advance(float dtSeconds);   // per-frame state advance + debug log

    // Drive the camera each frame from the current blend t. Pass the live
    // mission camera (the global `eye`). Captures a return snapshot once on
    // entry; restores it on full exit unless the user panned in overview.
    void driveCamera(Camera* eye);

    // Call from the camera-input site when the user manually pans/rotates while
    // overview is active — suppresses the auto-return so we don't yank them back.
    void notifyUserPan() { if (state_.active()) userPannedInOverview_ = true; }

    float blend() const { return state_.t(); }
    float iconAlpha() const { return state_.iconAlpha(); }  // overlay fade
    float setpoint() const { return state_.setpoint(); }   // diagnostic
    int   hotkeyFires() const { return hotkeyFires_; }      // diagnostic
    bool  active() const { return state_.active(); }

    // Squad-card click targets (screen space). Set each frame by the overview
    // renderer; queried by the mission click handler to map a click to a force
    // group. forceGroupAtScreen returns the group, or kNoForceGroup if none.
    static const int kNoForceGroup = -9999;
    struct CardHit { int forceGroup; void* unit; float l, t, r, b; };
    void setCardHits( std::span<const CardHit> hits );
    // Returns the card under (x,y) in screen space, or null if none.
    const CardHit* cardHitAt( float x, float y ) const;

    // A card click is consumed on the press frame, but the world MOVE order fires
    // on the later mouse-RELEASE frame. Arm suppression on the card press; the
    // release handler consumes it so the move under the card doesn't also fire.
    void armReleaseSuppression() { suppressRelease_ = true; }
    bool consumeReleaseSuppression() { bool s = suppressRelease_; suppressRelease_ = false; return s; }

    // --- Formation line (MC2_TACMAP_FORMATION_LINE, default OFF) ---
    // Draw a line in F6 overview; selected squad gets one move order per
    // evenly spaced slot. State machine: IDLE -> ARMED (L) -> DRAGGING (LMB)
    // -> issue on release. Esc / RMB / exiting F6 cancels.
    enum class FormationLineState { FL_IDLE = 0, FL_ARMED, FL_DRAGGING };
    using enum FormationLineState;
    static bool formationLineEnabled();        // env gate, cached
    void flOnHotkeyL();                        // L pressed while F6 active
    void flOnCancel();                         // Esc / right-click / F6 exit
    void flOnDragStart( const glm::vec3& worldStart );
    void flOnDragMove( const glm::vec3& worldEnd );
    void flOnRelease();                        // resets state + count
    FormationLineState flState() const { return flState_; }
    const glm::vec3& flStart() const { return flStart_; }
    const glm::vec3& flEnd()   const { return flEnd_; }
    // Evenly spaced slots start->end inclusive, scaled by the spacing factor
    // about the line midpoint; N==1 -> midpoint. Returns count.
    int flComputeSlots( glm::vec3* outSlots, int maxSlots ) const;
    // Spacing wheel: stretch/compress the squad spread about the line
    // midpoint. Persists across lines (the player's preferred spread).
    float flSpacing() const { return flSpacing_; }
    void  flAdjustSpacing( long wheelDelta );  // wheel up = wider
    // World health bars are replaced by icons/cards while the overview is up.
    // Game code masks DRAW_BARS through this at the game->appearance handoff
    // (mech/vehicle setObjectParameters, turret render).
    static unsigned long maskWorldBars( unsigned long drawFlags );

    void flSetMovers( Mover* const* movers, int n );
    int  flMoverCount() const { return flMoverCount_; }
    Mover* flMover( int i ) const { return ( i >= 0 && i < flMoverCount_ ) ? flMovers_[i] : nullptr; }

private:
    // Engine-free snapshot of the gameplay camera, captured once on entry.
    // Only altitude + tilt are captured/restored: those are the ONLY fields
    // driveCamera mutates while active. Position and rotation are never touched
    // by the overview, so they are naturally preserved (and stay wherever the
    // player panned to) — no restore needed, no yank-back on exit.
    struct CamSnapshot {
        bool  valid    = false;
        float altitude = 0.0f;   // cameraAltitude (set via zoomValue)
        float tilt     = 0.0f;   // projectionAngle (set via tiltValue) — lerp FROM this
        float maxAltLo = 0.0f;   // Camera::AltitudeMaximumLo — the zoom-out ceiling
        float maxAltHi = 0.0f;   // Camera::AltitudeMaximumHi — raised so we can pull back
    };

    TacticalOverviewState state_;
    CamSnapshot returnSnap_;
    bool        userPannedInOverview_ = false;
    int         hotkeyFires_ = 0;   // diagnostic: count of onHotkey() calls

    static const int kMaxCardHits = 32;
    CardHit cardHit_[kMaxCardHits];
    int     cardHitCount_ = 0;
    bool    suppressRelease_ = false;

    FormationLineState flState_ = FL_IDLE;
    glm::vec3    flStart_;
    glm::vec3    flEnd_;
    static const int   kFlMaxMovers = 32;
    Mover*             flMovers_[kFlMaxMovers] = {};   // snapshot at drag start
    int                flMoverCount_ = 0;
    float              flSpacing_ = 1.0f;   // persists across lines (NOT reset)
};

extern TacticalOverview g_tacticalOverview;

#endif // TACTICALOVERVIEW_H
