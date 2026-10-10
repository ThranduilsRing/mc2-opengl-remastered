# Phase split spec for GpuStaticPropRegistry::flush (GameOS/gameos/gos_static_prop_registry.cpp).
# Line numbers are 1-based lines of the file at BASE.
BASE = '799449697'
SRC = 'GameOS/gameos/gos_static_prop_registry.cpp'
FUNC_SIG = r'^void flush\(\) \{'
INDENT = '    '
DEDENT = ''
STRUCTS = {}
FRAME_GLOBAL = True
MEMBERS = []
EDITS = {}
ORCH_BEFORE = {}
ORCH_EXTRA = []
FILE_NAMESPACE = 'GpuStaticPropRegistry'
INTERNAL_INCLUDES = ['#include "gos_static_prop_registry_internal.h"']

# The orchestrator keeps the latch and early-out, the per-call diagnostic statics and clock, the persistent-store
# generation locals, the range loop skeleton with the locals each iteration hands on (light row, actor centre,
# cached-blob choice) and the cached / legacy branch; each helper takes what it names.
PARAMS = [
    ('currentFrame', 'uint32_t'),
    ('batcher', 'GpuStaticPropBatcher&'),
    ('batcher_pb', 'GpuStaticPropBatcher&'),
    ('s_currentGen', 'uint64_t'),
    ('s_storeDirty', 'bool'),
    ('s_pbExpectStaticCount', 'uint64_t&'),
    ('s_diag_flush_calls', 'uint64_t'),
    ('s_diag_ranges_total', 'uint64_t&'),
    ('s_diag_ranges_tombstone', 'uint64_t&'),
    ('s_diag_ranges_stale_frame', 'uint64_t&'),
    ('s_diag_ranges_stale_after_drawn', 'uint64_t&'),
    ('s_diag_ranges_drawn', 'uint64_t&'),
    ('s_diag_leaves_appended', 'uint64_t&'),
    ('s_diag_persistent_vanish', 'uint64_t&'),
    ('s_diag_total_ns', 'uint64_t'),
    ('s_guard2', 'bool'),
    ('kVanishFrames', 'uint32_t'),
    ('s_staleByType', 'std::unordered_map<int, uint64_t>&'),
    ('s_staleStreak', 'std::unordered_map<uint32_t, uint32_t>&'),
    ('regIdx', 'uint32_t'),
    ('rng', 'RecipeRange&'),
    ('freshLightIdx', 'uint32_t'),
    ('actorWorldCenter', 'const float (&actorWorldCenter)[3]'),
    ('useCachedBlob', 'bool'),
]

ORCH_DOC = ('// Builds this frame\'s static-prop instance and cull-record submissions from the visible ranges. The latch, the\n'
            '// diagnostic counters and clock, the range loop with the per-range light row, actor centre and cached-blob choice,\n'
            '// and the cached / legacy branch stay here; each phase body is a helper declared in\n'
            '// gos_static_prop_registry_internal.h.')

RNG = 'GameOS/gameos/gos_static_prop_registry_flush_range.cpp'
TAIL = 'GameOS/gameos/gos_static_prop_registry_flush_tail.cpp'
FILE_HEADS = {
    RNG: ['// GpuStaticPropRegistry: the per-range phases of flush() -- tombstone and currentness guards, the light-row and',
          '// seam diagnostics and the legacy per-leaf and cached submission paths. The orchestrator is in',
          '// gos_static_prop_registry.cpp.'],
    TAIL: ['// GpuStaticPropRegistry: the persistent-store rebuild and the post-loop phases of flush() -- store reinject, the',
           '// diagnostic summaries and the cull-record version bump. The orchestrator is in gos_static_prop_registry.cpp.'],
}

PHASES = [
    ('flushFrozenStoreRebuild', 861, 901, 'Rebuild the persistent static store and golden list from every registered recipe.', {'file': TAIL}),
    ('flushRangeTombstoneGuard', 905, 931, 'Count the range and skip a tombstoned one.', {'file': RNG, 'exits': 'continue', 'dedent': '    '}),
    ('flushRangeCurrentness', 933, 1039, 'Skip a range whose multi-shape was not refreshed this frame; count and report the stale drops.', {'file': RNG, 'exits': 'continue', 'dedent': '    '}),
    ('flushLightRowDiag', 1071, 1183, 'Count light rows that would reach the GPU empty and trace the first light slots.', {'file': RNG, 'dedent': '    '}),
    ('flushSeamProbe', 1202, 1222, 'Trace the override-type recipes that reach the substrate emit.', {'file': RNG, 'dedent': '    '}),
    ('flushLeafDiag', 1247, 1271, 'Trace which branch a range takes and whether its first leaves are zero.', {'file': RNG, 'dedent': '    '}),
    ('flushLegacyRange', 1273, 1456, 'Submit each leaf of the range with the fresh light index and append its cull record.', {'file': RNG, 'dedent': '    '}),
    ('flushCachedRange', 1458, 1577, 'Bulk-submit the range from the cached records and the persistent store.', {'file': RNG, 'dedent': '    '}),
    ('flushReinjectPersistentStore', 1581, 1617, 'Finalise the persistent static store and run its consistency oracle.', {'file': TAIL}),
    ('flushZeroLightProbe', 1624, 1689, 'Count the registered props whose light slot was never baked and dump their rows once.', {'file': TAIL}),
    ('flushDiagSummary', 1691, 1725, 'Print the per-call flush counters and the stale-drop discriminator.', {'file': TAIL}),
    ('flushSpflushWindow', 1726, 1824, 'Emit the per-10-frame cost-split summary and reset its window.', {'file': TAIL}),
    ('flushCullRecordVersion', 1825, 1839, 'Bump the cull-record version when the submitted prop set changed.', {'file': TAIL}),
]
