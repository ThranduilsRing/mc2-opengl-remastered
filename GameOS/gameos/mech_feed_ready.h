// Engine-free readiness flags for the mech island frame feed and the shadow feed.
#pragma once

struct MechFeedReady {
    bool frame  = false;
    bool shadow = false;
    void beginFlush() { shadow = false; }                              // an empty frame must not replay last frame's casters
    void endEmpty()   { frame = false; shadow = false; }               // nothing submitted: the retained feed must not draw
    void publish(bool populated) { frame = populated; shadow = populated; }
    void reset()      { frame = false; shadow = false; }
};
