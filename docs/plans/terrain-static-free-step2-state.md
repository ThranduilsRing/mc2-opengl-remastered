# terrain-static-free step 2

Assignment: slot04, claude/terrain-static-free, starting tip aaaf895a042.
Read prior slot08 receipt and integration plan sections terrain-static-free / 5.
Inspection: registration arms the tree bake latch; full update clears it;
prewarm shares the valid LOD0 slot across LODs; registered LOD selection does not
rearm it. Render recovery can invalidate on the shape cached-slot sentinel or
re-register after failed replay, arming the latch again. Prior mc2_24 census
has 48.6 tree bake updates/frame, no STATIC_REG_INVALID messages in full log.
Next: reproduce recovery with a production-source doctest before the fix;
then dirty-ID queue (dedup/drain/height-block cases), remove static shadow walk.
Proof pending: compile, covering unit/contracts, census 10/24, box headless
SIMID 01/03/10/24, box smoke-lite, final lint. No runtime changes yet.
