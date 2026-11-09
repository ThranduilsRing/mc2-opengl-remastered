<!-- doc-status: current | reviewed:2026-11-09 -->
# Asset container plan, rev 4 (delta over rev 3; M1 landed here)

Rev 3 is the plan of record (`A:/Games/mc2-ops/studies/asset-cook-cache-plan-rev3-2026-11-07.md`: stock-asset container, KTX2 repack, ZIP64 paks, stock FSTs kept as fallback). Sol's review p28 (`A:/Games/mc2-ops/studies/sol-review-p28-2026-11-07.md`) gave it NO-GO for one fixable authority rule. Rev 4 changes only that rule and the M1 definition; every other rev 3 line stands, and p28's colour/alpha/cache/milestone-order findings stay open for M3-M5 and M7.

## What changed from rev 3

1. **Authority is the VFS chain rank, never a provenance class (rev 3 s1, s10 M1).** Rev 3 ordered sources `mod > loose > loose(strip) > user-modified FST > stock`, which promotes a later FST classified UserModified above an earlier stock FST holding the same key. The engine does the opposite: `FastFileFind` returns the first registered archive and the VFS chain keeps registration order. Rev 4: **FST candidates keep registration order.** `SourceRecord.authorityRank` is the candidate's index in `vfs::candidates()` (mods in priority order, loose, size-strip alias, FSTs in registration order). Resolution is unchanged by M1.
2. **Provenance governs eligibility, not rank.** `ArchiveProvenance{Stock, UserModified, Absent}` (M2+) only decides whether a COOKED representation of that source may be served: a derived entry is eligible iff its `SourceRecord` (authority, `sourceDigest`, `depsDigest`) matches the winner's. A UserModified archive's pak entries are ineligible; its own bytes still serve at its registration rank.
3. **Required fixture:** a duplicate key in two archives, earlier stock and later modified. The winner, its rank 0 and its digest must be the earlier archive's. Landed as `tests/unit/test_vfs_content_digest.cpp` (registration-order case).
4. **Content identity (rev 3 s2) is implemented as: stat stamp then content hash.** The memo is keyed by (physical path, size, mtime) and cleared on every VFS rebuild; any stamp change forces a re-hash of the file. A path+size memo is gone (it was a same-size-edit stale hazard, formerly `content_ir.cpp` digestMemo and `sourceBytesDigest`). Rev 3's wider stamp (change-time, file id) and xxh3-128 stay for M2 when the pak digest field lands; M1 uses the existing `fastHash` (64-bit) and keeps the 64-bit width.

## M1 as implemented

- `mc2::vfs::SourceRecord` on every `Entry` (`authorityRank`). Later fields (`sourceDigest`, `depsDigest`, `representation`, `generation`) arrive with the milestone that produces them.
- `mc2::vfs::contentDigest(path)`: digest of the bytes the winning candidate serves, so the mod override order (mod priority, loose, strip, FST registration) decides which file is hashed. Winner is the first candidate that opens, same as `File::open`.
- `content_ir.cpp` takes its source digest from `contentDigest`; `sourceBytesDigest` hashes the bytes it is handed. The cook-cache source key no longer includes the file size separately (the digest covers it), so existing `cache/content-model` entries are orphaned once and re-cooked; the cache's size cap evicts them.
- `fastHash` moved to header-only `mclib/fast_hash.h` (same function, no value change).
- **Cost.** Per query: one `fopen` probe plus one stat. Per content change (or first sight after a rebuild): one sequential read and hash of that file at memory speed (about 1 GB/s class); an FST winner is read and hashed once per VFS generation, because the archive is immutable between rebuilds. Not per frame.
- **Bound (stated, tested).** A same-size edit with a restored mtime is invisible to a stat stamp until the next VFS rebuild (mod reload, `FastFileInit`/`Fini`). Rev 3's hard rule stands: the count/max-mtime `ComputeModDataSignature` is a log hint only and never skips the memo check.

No repack, pak provider, provenance table or `preferCookedSource` change is in M1; those are M2+.
