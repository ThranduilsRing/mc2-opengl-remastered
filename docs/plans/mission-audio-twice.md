# Mission audio twice

Assignment: slot11, claude/mission-audio-twice; requested base f269b312d;
assigned checkout already includes Linux fixes through 8fc0c6c89.

Found: GameSoundSystem::update starts speech without advancing currentFragment
when the opening noise packet is absent. The next completed-channel update
replays fragment zero. Radio::playMessage only requests opening noise after a
successful noise packet seek, so that branch depends on packet availability.
Fix: use playNextDataClip and advance exactly as the subsequent-fragment path;
set wholeMsgDone before starting so an empty message remains complete.

Trace: briefing-screen play/stop/pause handlers are empty; logistics tutorial
uses the ABL sound bindings. Mission mc2_01 ABL guards MissionStartTune1 with
playMissionStartTune and each tutorial voice/video with its stage. playwave ->
playSupportSample -> playDigitalStream -> STREAM3CHANNEL; music uses two crossfade
slots; gosAudio PlayOnce maps to Mix_PlayChannel loops=0. Startup preload excludes
sound/music paths. Predecode caches resources without playing. Tick deferral
executes collected sound commands once at join.

Regression: compile production radio playback method slices using the existing
unit source-slice seam; count callsign/message plays with/without opening static
and 90 busy device updates. This isolates a real duplication bug; matching the
owner's specific heard cue and all mc2_01 startup cues remains unproven.

First proof at e1c2a4d27: box engine build, smoke-lite and lint passed. Unit
compile found two seam mistakes (handle must be a pointer; CAPTURE takes one
argument), corrected in the next commit. Covering sim_include_firewall row
passed (114.51 s, board wall 194.78 s: tool slowness). Boundaries is nightly and
was not selected by the initial non-nightly command.

Next: box build/unit/smoke/lint at corrected tip; Windows compile completion
and final identity; targeted doctests and boundaries row; final receipts go to
E:/mc2-archive-2026-10/proofs/mission-audio-twice/STATE.md.
