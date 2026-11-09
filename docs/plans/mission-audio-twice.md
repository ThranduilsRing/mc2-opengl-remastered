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

Next: box build/unit/smoke,
Windows Ninja compile, covering contracts and final lint; record receipts here.
