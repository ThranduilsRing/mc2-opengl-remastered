#!/bin/sh
# scripts/check-sim-no-audio-reads.sh
#
# Simulation code never reads audio-device state. The only readers of gosAudio_Get* (channel play mode, sliders) are the
# sound system itself (mclib/soundsys.cpp) and the radio-message queue (code/gamesound.cpp): both are audio-side. Anything
# the simulation needs to know about music or sound (what a script asked for, when a tune ends) is the scenario clock plus
# file data, never the device (mclib/script_music.h).
#
# WHY: the audio device's playback position is wall-clock state. getCurrentMusicId once reported it, every stock mission
# script branches on that builtin and then draws from the simulation RNG, and two runs of mc2_16 forked (2026-10-12).
#
# Exit 0 = clean; exit 1 = a read outside the audio-side files (file:line printed). Comment-only lines are ignored.

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

SCOPE_DIRS="code mclib gui editor"
AUDIO_SIDE='^(mclib/soundsys\.cpp|code/gamesound\.cpp):'
PATTERN='\bgosAudio_Get[A-Za-z]*[[:space:]]*\('

hits="$(git ls-files $SCOPE_DIRS | grep -E '\.(c|cc|cpp|cxx|h|hh|hpp|hxx|inl)$' | xargs grep -nE "$PATTERN" 2>/dev/null \
        | grep -vE "$AUDIO_SIDE" | grep -vE '^[^:]*:[0-9]+:[[:space:]]*(//|/\*|\*)' || true)"

if [ -n "$hits" ]; then
    printf '%s\n' "$hits" | sed 's/^/VIOLATION: /'
    echo "scripts/check-sim-no-audio-reads.sh: audio-state read(s) outside the audio side (see the header)"
    exit 1
fi
echo "scripts/check-sim-no-audio-reads.sh: clean (gosAudio_Get* only in mclib/soundsys.cpp and code/gamesound.cpp)"
exit 0
