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
    tail = between(src, '// Tail.Blit:', '// Submit the composite and tail')
    assert 'ZoneScopedN("VKIsl.Present.Tail.Blit")' in tail
    assert tail.index('Blit.BarriersAndBlit') < tail.index('Blit.ProfileEnd') < tail.index('Blit.CommandBufferEnd')
    assert 'blitScope.end();\n    }' in tail
    assert '{ ZoneScopedN("VKIsl.Present.Tail.Blit.ProfileEnd"); gpuprof::recordFrameEnd(s.curCbuf()); }' in tail
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
