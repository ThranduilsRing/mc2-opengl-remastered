Stock VFX playback oracle fixture: `mc2.fx` (920,265 bytes), copied unchanged
from `mc2-win64-v0.4/data/effects/mc2.fx`.

SHA-256: `0fc3f12eec5245a7544429c93fc891f0f770a4131cd4a8ca95616bf4b5361b3d`.

The playback test compares all 904 definitions with the legacy oracle in four
quarters and three scenarios per definition. It resolves this fixture through
`test_repo_root.h`, independent of the install location or working directory.
Missing fixtures are reported as a doctest skip; malformed data and mismatches
still fail. The generated controlled catalog test always runs.
