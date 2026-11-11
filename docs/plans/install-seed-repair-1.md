# Install seed repair

The canonical seed's content was stale relative to its deployed manifest at
f269b312d8328aaafccec0b53a15f23ce5775efb. All 34 mismatched files were restored
from that commit, selecting Git LF or CRLF bytes only when their SHA256 and
length exactly matched the original manifest. Seventeen editor files had
changed line endings; seventeen legacy campaign pages also had changed UV
dimensions. The manifest was retained unchanged. Original files and the
manifest are archived under
`E:/mc2-archive-2026-10/proofs/install-seed-repair-1/seed-before/` and
`seed-manifest-before.csv`; `seed-diagnosis.json` records each mismatch.
No verification was relaxed and no seed files were deleted.

Parity sync now reports mutation counts before writing, clears read-only
attributes on replacements, and deletes unowned extras under data/, assets/
and root FST archives. Payload-owned files, cook stamps, excluded backups,
and files outside those trees remain protected. Deploy's payload copying also
clears read-only attributes after its existing hardlink isolation check.

Tree enumeration first reuses published seed/reference KTX2 files when they
match the checked-in SHA256 pins, then verifies the copied bytes. A corrupt
candidate is ignored; the existing cook remains the fallback. This avoids
the redundant long cook that exhausted the 300-second enumeration deadline.

Offline coverage is in `scripts/tests/test_asset_parity.py`: seed rejection
and exact-byte repair, read-only atomic replacement without changing peer
bytes, read-only payload copying, eight extra ferns deleted after a dry run,
and pinned-cache reuse/rejection. Focused tests also include
`test_isolated_deploy.py` and `test_deploy_build_layout.py`.

Final receipts live in the proof directory above. The single real install is
`E:/mc2-installs/install-seed-repair-1`; no protected install is a write target.
The short continuation note is in slot02 scratch, per lane rules.
