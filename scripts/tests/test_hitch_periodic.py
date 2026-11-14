"""Source contracts for diagnostic cadence and stable hitch attribution.

These pin policy at the frame/load boundaries without requiring a GPU or a
live Tracy connection. Runtime duration and hitch acceptance belong to Tracy A/B.
"""
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def source(name):
    return (ROOT / "GameOS" / "gameos" / name).read_text(encoding="utf-8")


def between(text, begin, end):
    return text.split(begin, 1)[1].split(end, 1)[0]


def test_present_keeps_outer_zones_and_disjoint_tail_children():
    src = source("engine_present_vk.cpp")
    tail = between(src, '// The Render ticket covers every frame allocation', 'const VkSemaphore renderSem')
    assert tail.index('Blit.ProfileEnd') < tail.index('vkEndCommandBuffer(s.curCbuf())') < tail.index('afterRenderTail(')
    assert tail.index('afterRenderTail(') < tail.index('VKIsl.Present.Tail.Blit"') < tail.index('Blit.BarriersAndBlit')
    assert tail.index('Blit.BarriersAndBlit') < tail.index('vkCmdBlitImage(') < tail.index('vkEndCommandBuffer(blitCb)')
    assert 'gpuprof::recordFrameEnd(s.curCbuf()); }' in tail
    hud = between(src, 'uint32_t recordPendingHudColorProof(', '// Build (or rebuild) the shared present image')
    for name in ('ObjectsAndMirrorRetirement', 'PrepareAndTextureUpload', 'VertexUpload', 'DrawRecord'):
        assert f'ZoneScopedN("VKIsl.Present.Tail.Hud.{name}")' in hud
    prepare = hud.index('Hud.PrepareAndTextureUpload')
    assert prepare < hud.index('for (const VkHudDraw& d : s.pendingHudDraws)')
    assert hud.index('Hud.DrawRecord') < hud.index('for (const HudNativeCmd& td : commands)')


def test_profiler_separates_queries_output_and_connected_tracy():
    src = source("vk_gpu_profile.cpp")
    for name in ('QueryCollection', 'WindowOutput', 'TracyCollection'):
        assert f'VKIsl.Present.Tail.Blit.Profile.{name}' in src
    assert src.count('if (tracyLive) { ZoneScopedN(') == 2
    assert 'const bool tsOptOut = ' in src
    assert 'if (tsOk && !tsOptOut)' in src
    assert 'g.tracy && tracy::GetProfiler().IsConnected()' in src


def test_gpu_query_ring_reads_retired_frames_before_reset_and_reuse():
    # Trigger: GameOS/gameos/vk_gpu_profile.cpp and vk_frame_ring.h.
    import re
    src = source("vk_gpu_profile.cpp")
    lag = int(re.search(r'kMaxFramesInFlight = (\d+)', source("vk_frame_ring.h"))[1])
    assert 'constexpr uint32_t kReadLag = kMaxFramesInFlight;' in src
    assert 'constexpr uint32_t kRing = kReadLag + 1;' in src
    for begin, end in (('bool recordFrameEnd(', 'void frameEndSubmitted('),
                       ('void frameEnd() {', 'Scope::Scope(')):
        body = between(src, begin, end)
        assert '(f + kRing - kReadLag) % kRing' in body
        assert 'if (g.timers && f >= kReadLag)' in body
        assert body.index('readSlot(rslot, used)') < body.index('if (used) vkCmdResetQueryPool(')
    assert 'VK_QUERY_RESULT_WAIT_BIT' not in src
    for inflight in range(1, lag + 1):
        for frame in range(lag, 32):
            read_frame = frame - lag
            assert read_frame <= frame - inflight
            assert (frame + lag + 1 - lag) % (lag + 1) == read_frame % (lag + 1)
            assert read_frame + lag + 1 > frame  # reset precedes the next recording


def test_decal_build_is_measured_outside_unchanged_parent_and_wait_is_separate():
    src = between(source("vulkan_impact_decals.cpp"), 'extern "C" int mc2_vk_impact_decals_run', 'return 1;\n}')
    assert src.index('VK-impact-decals.LazyBuild') < src.index('ZoneScopedN("VK-impact-decals")')
    assert 'if (!s->inited) {' in src
    assert 'if (!build(s, w, h)) {' in src
    for name in ('SlotWait', 'PrepareAndUpload', 'Record', 'Submit'):
        assert f'ZoneScopedN("VK-impact-decals.{name}")' in src
    assert '5ull * 1000 * 1000 * 1000, "impact_decals_slot"' in src
    assert src.index('VK-impact-decals.Record') < src.index('vkEndCommandBuffer(cb)') < src.index('VK-impact-decals.Submit')


def test_audio_distinguishes_open_from_wav_without_logging_asset_paths():
    src = between(source("gameos_sound.cpp"), 'ZoneScopedN("Audio.Stream.LoadFile")', 'gosASSERT(res_type == gosAudio_UserMemory)')
    assert 'Audio.Stream.LoadFile.MusicOpen' in src
    assert src.count('Audio.Stream.LoadFile.WavLoad') == 2  # SDL and Mix build paths
    assert 'ZoneText(file_name, std::min<size_t>(std::strlen(file_name), 192))' in src


def test_periodic_work_has_attribution_outside_element_loops():
    mem = source("gpu_memory_report.cpp")
    for name in ('VulkanQuery', 'DxgiQuery', 'BattlebenchSnapshot', 'BattlebenchOutput', 'TracyPoll'):
        assert f'GpuMemory.{name}' in mem
    infra = source("vk_render_infra.cpp")
    persist = between(infra, 'void saveLocked(State& s)', '\n}\n}')
    assert persist.index('if (!s.cache || !s.dirty) return;') < persist.index('VKInfra.Cache.Persist') < persist.index('vkGetPipelineCacheData')
    assert 'VKInfra.Ledger.Sample' in infra
    assert 'VKInfra.Reload.Poll' in infra


def test_pass_window_keeps_default_emission_and_one_unflushed_write():
    src = source("vk_gpu_profile.cpp")
    window = between(src, 'void flushWindow(uint64_t frame)', '// Reads one slot')
    assert window.count('mc2::fmt::print_err(') == 1
    assert 'std::fprintf' not in window and 'std::fflush' not in window
    assert '[VK_GPU_PASSES v1] frame={} n={} span_ms={:.3f} sum_ms={:.3f} unattributed_ms={:.3f} scopes={} dropped={} overflow={} |{}' in window
    assert '"=%.3f(x%.1f)"' in window and "output += r.name" in window
    assert 'constexpr uint32_t kLogEvery = 120;' in src
    assert src.count('if (g.windowFrames >= kLogEvery) flushWindow(f);') == 2
    assert 'tagRequested' not in window.split('output += \'\\n\';', 1)[0]
    facade = (ROOT / 'mclib' / 'mc2format.h').read_text(encoding='utf-8')
    writer = between(facade, 'int print(std::FILE* f,', 'template <class... A> int print_err(')
    assert writer.count('std::fwrite(') == 1 and 'fflush' not in writer


def test_quiet_ledger_retains_crash_refresh_but_only_diagnostics_query_driver():
    src = source("vk_render_infra.cpp")
    frame = between(src, 'void frame() {', 'void prewarm(')
    periodic = frame.split('if(f%120==0)', 1)[1]
    diagnostic, quiet = periodic.split('} else {', 1)
    assert 'get<"MC2_DIAG_VERBOSE">() || mc2_diag::tagRequested("VK_RESIDENCY")' in diagnostic
    assert 'ledgerJson()' in diagnostic
    assert 'refreshLedgerLocked(s,f,' in quiet
    assert 'ledgerJson()' not in quiet and 'deviceLocalMemory' not in quiet
    snapshot = between(src, 'void refreshLedgerLocked(', '\nstd::string ledgerJson()')
    assert 'std::snprintf(s.ledger,sizeof(s.ledger)' in snapshot
    assert 'ostringstream' not in src and 'deviceLocalMemory' not in snapshot
    assert 'snapshot_frame' in snapshot and 'vma_sample_frame' in snapshot
    prewarm = between(src, 'void prewarm(', 'void deviceLost()')
    assert '(void)ledgerJson()' in prewarm  # load-boundary driver snapshot, even when quiet


def test_cache_persistence_stays_outside_gameplay_and_after_compile_drain():
    src = source("vk_render_infra.cpp")
    frame = between(src, 'void frame() {', 'void prewarm(')
    assert 'saveCache(' not in frame and 'saveLocked(' not in frame
    assert 'seconds(30)' not in frame
    prewarm = between(src, 'void prewarm(', 'void deviceLost()')
    assert 'saveCache();' in prewarm
    shutdown = between(src, 'void shutdown() {', 'static void recordAllocation(')
    assert shutdown.index('s.compileIdle.wait(') < shutdown.index('saveLocked(s)') < shutdown.index('vkDestroyPipelineCache(')
    persist = between(src, 'void saveLocked(State& s)', '\n}\n}')
    for operation in ('!s.cache || !s.dirty', 'vkGetPipelineCacheData', 'h.hash=hashBytes', 'out.flush()', 'std::filesystem::rename', 's.dirty=false'):
        assert operation in persist
    assert persist.index('std::filesystem::rename') < persist.index('s.dirty=false')


def test_memory_tick_keeps_bench_and_connected_plots_without_duplicate_poll():
    tick = between(source("gpu_memory_report.cpp"), 'void tick(unsigned long frame)', '}  // namespace mc2::gpumem')
    assert tick.index('frame % kPlotEveryFrames') < tick.index('if (mc2::battlebench::output)')
    assert tick.index('snapshot = take()') < tick.index('if (!TracyIsConnected) return;')
    plot = tick.split('if (!TracyIsConnected) return;', 1)[1]
    assert 'if (!mc2::battlebench::output)' in plot
    assert plot.count('QueryVideoMemoryInfo') == 1
    assert 'if (snapshot.dxgiProcessUsageBytes >= 0)' in plot
    assert 'TracyPlot("VRAM.Process.MB"' in plot


def test_crash_json_formats_fit_max_counters_and_preserve_all_categories():
    # Expand the production formats at their maximum counter widths. This is a
    # static capacity/schema check; the production build checks the C++ call types.
    import ast
    import json
    import re

    src = between(source("vk_render_infra.cpp"), 'void refreshLedgerLocked(', '\nstd::string ledgerJson()')
    header = between(src, 'int len=std::snprintf(s.ledger,sizeof(s.ledger),', 'static_cast<unsigned long long>(s.budget)')
    literals = re.findall(r'"(?:\\.|[^"\\])*"', header)
    header_format = ''.join(ast.literal_eval(s) for s in literals)
    category = between(src, 'len+=std::snprintf(s.ledger+len,sizeof(s.ledger)-len,', ',i?')
    category_format = ast.literal_eval(category)
    text = re.sub(r'%llu|%zu|%s', lambda m: 'false' if m[0] == '%s' else str(2**64-1), header_format)
    for i, name in enumerate(('textures', 'shadows', 'terrain', 'vegetation', 'geometry', 'staging', 'targets')):
        assert 0 < len(text) < 2048-3
        values = iter((',' if i else '', name, str(2**64-1), str(2**64-1)))
        text += re.sub(r'%llu|%s', lambda _: next(values), category_format)
    assert 0 < len(text) < 2048-2
    data = json.loads(text + '}}')
    assert len(data['categories']) == 7
    assert data['categories']['targets']['peak'] == 2**64-1
    assert data['snapshot_frame'] == data['vma_sample_frame'] == 2**64-1
