#!/usr/bin/env python3
"""RenderDoc replay worker -- runs INSIDE the RenderDoc python interpreter.

This file is never launched with the system python. It is executed by
``qrenderdoc.exe --python rd_worker.py`` so that ``import renderdoc`` (the
replay module) resolves. The stock Windows RenderDoc install does not ship a
standalone ``renderdoc.pyd`` for an external interpreter, so ``qrenderdoc
--python`` is the supported headless entry point (verified on v1.44).

The outer CLIs (attribute.py / states.py) do not import this module. They
write a *job* JSON to a temp file, point ``MC2_RD_JOB`` at it, and spawn
qrenderdoc. This worker reads the job, opens the capture headlessly, does the
requested analysis, writes ``job["result_json"]`` and ``job["result_md"]``,
then hard-exits with ``os._exit`` (the Qt UI never opens).

Job schema (all paths absolute)::

    {
      "mode": "attribute" | "states",
      "rdc": "<path to .rdc>",
      "result_json": "<path>",
      "result_md": "<path>",

      # attribute mode
      "region": [x, y, w, h] | null,   # null => whole frame
      "grid": 11,                       # sample grid NxN across region
      "target": null,                   # output target index (default backbuffer)

      # states mode
      "draw": <eventId int>
    }

Every replay call is wrapped defensively: a missing accessor on some
RenderDoc version degrades to a note rather than aborting the report.
"""
import os
import sys
import json
import traceback

# ---------------------------------------------------------------------------
# Bootstrap: read the job, set up a crash sink so failures are never silent.
# ---------------------------------------------------------------------------
JOB_PATH = os.environ.get("MC2_RD_JOB")


def _die(msg, job=None):
    """Write an error result (if we know where) and hard-exit."""
    try:
        if job and job.get("result_json"):
            with open(job["result_json"], "w") as fh:
                json.dump({"ok": False, "error": msg}, fh, indent=2)
        if job and job.get("result_md"):
            with open(job["result_md"], "w") as fh:
                fh.write("# RenderDoc worker error\n\n```\n%s\n```\n" % msg)
    except Exception:
        pass
    # Last-resort breadcrumb next to the job file.
    try:
        if JOB_PATH:
            with open(JOB_PATH + ".err", "w") as fh:
                fh.write(msg)
    except Exception:
        pass
    os._exit(3)


if not JOB_PATH or not os.path.isfile(JOB_PATH):
    _die("MC2_RD_JOB not set or missing: %r" % JOB_PATH)

with open(JOB_PATH) as _fh:
    JOB = json.load(_fh)

try:
    import renderdoc as rd
except Exception as e:  # pragma: no cover - only in a broken install
    _die("import renderdoc failed: %r\n%s" % (e, traceback.format_exc()), JOB)


# ---------------------------------------------------------------------------
# Capture open / close helpers
# ---------------------------------------------------------------------------
def open_capture(path):
    if not os.path.isfile(path):
        _die("capture not found: %s" % path, JOB)
    cap = rd.OpenCaptureFile()
    res = cap.OpenFile(path, "", None)
    if res != rd.ResultCode.Succeeded:
        _die("OpenFile failed (%s) for %s" % (res, path), JOB)
    if cap.LocalReplaySupport() != rd.ReplaySupport.Supported:
        _die("local replay unsupported for this capture", JOB)
    status, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if status != rd.ResultCode.Succeeded:
        _die("OpenCapture failed: %s" % status, JOB)
    return cap, controller


def flatten(actions, acc=None):
    if acc is None:
        acc = []
    for a in actions:
        acc.append(a)
        if a.children:
            flatten(a.children, acc)
    return acc


def build_marker_paths(actions, stack=None, out=None):
    """Map eventId -> 'Marker A / Marker B' pass context from the tree."""
    if out is None:
        out = {}
    if stack is None:
        stack = []
    for a in actions:
        is_marker = bool(a.flags & rd.ActionFlags.PushMarker) or (
            a.children and not (a.flags & rd.ActionFlags.Drawcall)
        )
        name = None
        if is_marker:
            try:
                name = a.customName or a.GetName(_STRUCT_FILE)
            except Exception:
                name = getattr(a, "customName", "") or "<marker>"
        new_stack = stack + [name] if name else stack
        out[a.eventId] = " / ".join(s for s in new_stack if s)
        if a.children:
            build_marker_paths(a.children, new_stack, out)
    return out


def resource_names(controller):
    names = {}
    try:
        for r in controller.GetResources():
            names[int(r.resourceId)] = r.name
    except Exception:
        pass
    return names


def action_name(a):
    try:
        return a.GetName(_STRUCT_FILE)
    except Exception:
        return "<eid %d>" % a.eventId


def blend_to_str(cb):
    """One-line human-readable blend description for a ColorBlend."""
    try:
        if not cb.enabled:
            return "OFF (writeMask=%s)" % _mask_str(cb.writeMask)
        c = cb.colorBlend
        a = cb.alphaBlend
        return "ON  color:%s->%s %s  alpha:%s->%s %s  writeMask=%s" % (
            _enum(c.source), _enum(c.destination), _enum(c.operation),
            _enum(a.source), _enum(a.destination), _enum(a.operation),
            _mask_str(cb.writeMask),
        )
    except Exception as e:
        return "<blend read error %r>" % e


def _enum(v):
    s = str(v)
    return s.split(".")[-1] if "." in s else s


def _mask_str(m):
    try:
        m = int(m)
    except Exception:
        return str(m)
    return "".join(ch for ch, bit in zip("RGBA", (1, 2, 4, 8)) if m & bit) or "none"


def pick_target(controller, pipe, names, explicit_index):
    """Return (resourceId, TextureDescription) for the output to sample."""
    outs = []
    try:
        outs = pipe.GetOutputTargets()
    except Exception:
        pass
    chosen = None
    if explicit_index is not None and 0 <= explicit_index < len(outs):
        rid = outs[explicit_index].resource
        if int(rid) != 0:
            chosen = rid
    if chosen is None:
        for o in outs:
            if int(o.resource) != 0:
                chosen = o.resource
                break
    if chosen is None:
        # Fall back to a swap-buffer-flagged texture.
        for t in controller.GetTextures():
            try:
                if t.creationFlags & rd.TextureCategory.SwapBuffer:
                    chosen = t.resourceId
                    break
            except Exception:
                pass
    tex = None
    if chosen is not None:
        for t in controller.GetTextures():
            if t.resourceId == chosen:
                tex = t
                break
    return chosen, tex


# ---------------------------------------------------------------------------
# Mode: attribute -- which draws touched a screen region
# ---------------------------------------------------------------------------
def run_textures(controller, job):
    """Enumerate color textures (render targets) so a caller can pick a
    pre-composite FBO to PixelHistory against by resid."""
    global _STRUCT_FILE
    _STRUCT_FILE = controller.GetStructuredFile()
    names = resource_names(controller)
    rows = []
    for t in controller.GetTextures():
        try:
            flags = int(t.creationFlags)
        except Exception:
            flags = 0
        cats = []
        for label, bit in (("ColorTarget", rd.TextureCategory.ColorTarget),
                           ("DepthTarget", rd.TextureCategory.DepthTarget),
                           ("SwapBuffer", rd.TextureCategory.SwapBuffer),
                           ("ShaderRead", rd.TextureCategory.ShaderRead)):
            try:
                if flags & bit:
                    cats.append(label)
            except Exception:
                pass
        rows.append({
            "resid": int(t.resourceId),
            "name": names.get(int(t.resourceId), "?"),
            "width": t.width, "height": t.height,
            "format": _fmt(t),
            "cats": cats,
        })
    rows.sort(key=lambda r: (-("ColorTarget" in r["cats"]), r["resid"]))
    result = {"ok": True, "mode": "textures", "rdc": job["rdc"],
              "textures": rows}
    _write_json(job, result)
    with open(job["result_md"], "w") as fh:
        fh.write("# RenderDoc textures\n\n| resid | name | dims | format | cats |\n")
        fh.write("|---|---|---|---|---|\n")
        for r in rows:
            fh.write("| %d | %s | %dx%d | %s | %s |\n"
                     % (r["resid"], _md_cell(r["name"]), r["width"],
                        r["height"], _md_cell(r["format"]),
                        ",".join(r["cats"])))


def run_disasm(controller, job):
    """Disassemble/dump the source of a shader resid (identify a program)."""
    global _STRUCT_FILE
    _STRUCT_FILE = controller.GetStructuredFile()
    names = resource_names(controller)
    want = int(job["shader_resid"])
    refl = None
    src = ""
    for s in controller.GetShaders() if hasattr(controller, "GetShaders") else []:
        pass
    # Find via pipeline at the given eid.
    eid = int(job.get("eid", 0))
    if eid:
        controller.SetFrameEvent(eid, True)
    pipe = controller.GetPipelineState()
    for st in (rd.ShaderStage.Vertex, rd.ShaderStage.Pixel,
               rd.ShaderStage.Geometry, rd.ShaderStage.Compute):
        try:
            sid = pipe.GetShader(st)
        except Exception:
            continue
        if int(sid) != want:
            continue
        try:
            refl = pipe.GetShaderReflection(st)
        except Exception:
            refl = None
        if refl is not None:
            targets = controller.GetDisassemblyTargets(True)
            tgt = targets[0] if targets else ""
            try:
                src = controller.DisassembleShader(pipe.GetGraphicsPipelineObject()
                                                   if hasattr(pipe, "GetGraphicsPipelineObject") else rd.ResourceId.Null(),
                                                   refl, tgt)
            except Exception as e:
                src = "<disasm failed: %r>" % e
        break
    result = {"ok": True, "mode": "disasm", "rdc": job["rdc"],
              "shader_resid": want, "eid": eid,
              "source": src or "<no reflection>"}
    _write_json(job, result)
    with open(job["result_md"], "w") as fh:
        fh.write("# Shader %d disasm (eid %d)\n\n```\n%s\n```\n"
                 % (want, eid, src[:20000]))


def run_pixel(controller, job):
    """Dump PixelHistory pre/post colors at one pixel of a target texture,
    so a caller can see exactly which eid turns a pixel white/black."""
    global _STRUCT_FILE
    _STRUCT_FILE = controller.GetStructuredFile()
    names = resource_names(controller)
    flat = flatten(controller.GetRootActions())
    by_eid = {a.eventId: a for a in flat}
    want_resid = int(job["target_resid"])
    tgt = None
    for t in controller.GetTextures():
        if int(t.resourceId) == want_resid:
            tgt = t.resourceId
            break
    if tgt is None:
        _die("target_resid %r not found" % want_resid, job)
    px, py = int(job["px"]), int(job["py"])
    sub = rd.Subresource(0, 0, 0)
    mods = controller.PixelHistory(tgt, px, py, sub, rd.CompType.Typeless)
    rows = []
    for m in mods:
        def col(c):
            try:
                return [round(float(v), 4) for v in list(c.floatValue)[:4]]
            except Exception:
                return None
        a = by_eid.get(m.eventId)
        rows.append({
            "eid": m.eventId,
            "name": action_name(a) if a else "<eid %d>" % m.eventId,
            "pre": col(m.preMod.col),
            "post": col(m.postMod.col),
            "passed": bool(m.Passed()) if hasattr(m, "Passed") else None,
        })
    result = {"ok": True, "mode": "pixel", "rdc": job["rdc"],
              "target_resid": want_resid, "px": px, "py": py, "mods": rows}
    _write_json(job, result)
    with open(job["result_md"], "w") as fh:
        fh.write("# Pixel history resid %d (%d,%d)\n\n" % (want_resid, px, py))
        fh.write("| eid | name | pre RGBA | post RGBA | passed |\n|---|---|---|---|---|\n")
        for r in rows:
            fh.write("| %d | %s | %s | %s | %s |\n"
                     % (r["eid"], _md_cell(r["name"])[:40], r["pre"],
                        r["post"], r["passed"]))


def run_attribute(controller, job):
    global _STRUCT_FILE
    _STRUCT_FILE = controller.GetStructuredFile()
    names = resource_names(controller)
    roots = controller.GetRootActions()
    flat = flatten(roots)
    markers = build_marker_paths(roots)
    draws = [a for a in flat if a.flags & rd.ActionFlags.Drawcall]
    by_eid = {a.eventId: a for a in flat}

    # Establish the output target from the final drawcall's bound outputs.
    if not draws:
        _die("capture has no drawcalls", job)
    controller.SetFrameEvent(draws[-1].eventId, True)
    pipe = controller.GetPipelineState()
    want_resid = job.get("target_resid")
    tgt = tex = None
    if want_resid is not None:
        # Sample an explicit texture (e.g. the pre-composite HDR scene FBO).
        for t in controller.GetTextures():
            if int(t.resourceId) == int(want_resid):
                tgt, tex = t.resourceId, t
                break
        if tgt is None:
            _die("target_resid %r not found among capture textures"
                 % want_resid, job)
    else:
        tgt, tex = pick_target(controller, pipe, names, job.get("target"))
    if tgt is None or tex is None:
        _die("could not resolve an output color target to sample", job)

    W, H = tex.width, tex.height
    region = job.get("region")
    if region:
        rx, ry, rw, rh = [int(v) for v in region]
    else:
        rx, ry, rw, rh = 0, 0, W, H
    rx = max(0, min(rx, W - 1))
    ry = max(0, min(ry, H - 1))
    rw = max(1, min(rw, W - rx))
    rh = max(1, min(rh, H - ry))

    grid = int(job.get("grid", 11))
    grid = max(2, min(grid, 64))
    sub = rd.Subresource(0, 0, 0)

    # Sample a grid of pixels; PixelHistory gives exact per-pixel draw list.
    hits = {}           # eid -> dict(count, wrote, samples[])
    samples = []
    xs = _spread(rx, rw, grid)
    ys = _spread(ry, rh, grid)
    total = 0
    for py in ys:
        for px in xs:
            total += 1
            try:
                mods = controller.PixelHistory(tgt, px, py, sub,
                                               rd.CompType.Typeless)
            except Exception as e:
                samples.append((px, py, "ERR %r" % e))
                continue
            top_writer = None
            for m in mods:
                eid = m.eventId
                wrote = _mod_wrote(m)
                rec = hits.setdefault(eid, {"count": 0, "wrote": 0,
                                            "px": []})
                rec["count"] += 1
                if wrote:
                    rec["wrote"] += 1
                    top_writer = eid
                if len(rec["px"]) < 8:
                    rec["px"].append([px, py])
            samples.append((px, py, top_writer))

    # Assemble per-draw attribution records, richest first.
    records = []
    for eid, rec in hits.items():
        a = by_eid.get(eid)
        controller.SetFrameEvent(eid, True)
        p = controller.GetPipelineState()
        rec_out = {
            "eid": eid,
            "name": action_name(a) if a else "<eid %d>" % eid,
            "marker": markers.get(eid, ""),
            "sample_hits": rec["count"],
            "write_hits": rec["wrote"],
            "num_indices": getattr(a, "numIndices", 0) if a else 0,
            "num_instances": getattr(a, "numInstances", 0) if a else 0,
            "example_pixels": rec["px"],
        }
        rec_out.update(_draw_state(controller, p, names))
        records.append(rec_out)
    # Order: pixels this draw actually *wrote* (visible) first, then coverage.
    records.sort(key=lambda r: (r["write_hits"], r["sample_hits"]),
                 reverse=True)

    result = {
        "ok": True,
        "mode": "attribute",
        "rdc": job["rdc"],
        "target": {"resid": int(tgt), "name": names.get(int(tgt), "?"),
                   "width": W, "height": H,
                   "format": _fmt(tex)},
        "region": {"x": rx, "y": ry, "w": rw, "h": rh},
        "grid": grid,
        "samples_taken": total,
        "draws": records,
    }
    _write_json(job, result)
    _write_attribute_md(job, result)


def _spread(origin, length, n):
    if n <= 1 or length <= 1:
        return [origin + length // 2]
    step = (length - 1) / (n - 1)
    return sorted({origin + int(round(i * step)) for i in range(n)})


def _mod_wrote(m):
    """True if this event actually modified the pixel (passed all tests)."""
    for attr in ("passed", "shaderDiscarded"):
        pass
    try:
        # RenderDoc PixelModification: .Passed() convenience if present.
        if hasattr(m, "Passed"):
            return bool(m.Passed())
    except Exception:
        pass
    # Fallback: pre != post colour.
    try:
        a = list(m.preMod.col.floatValue)[:4]
        b = list(m.postMod.col.floatValue)[:4]
        return a != b
    except Exception:
        return True


def _draw_state(controller, pipe, names):
    """Compact per-draw state used in attribution rows."""
    out = {"vs": None, "ps": None, "blend": None,
           "depth_test": None, "depth_write": None, "cull": None}
    try:
        vs = pipe.GetShader(rd.ShaderStage.Vertex)
        out["vs"] = {"resid": int(vs), "name": names.get(int(vs), "?")}
    except Exception:
        pass
    try:
        ps = pipe.GetShader(rd.ShaderStage.Pixel)
        out["ps"] = {"resid": int(ps), "name": names.get(int(ps), "?")}
    except Exception:
        pass
    try:
        blends = pipe.GetColorBlends()
        if blends:
            out["blend"] = blend_to_str(blends[0])
    except Exception:
        pass
    _add_gl_depth_cull(controller, out)
    return out


def _add_gl_depth_cull(controller, out):
    try:
        api = controller.GetAPIProperties().pipelineType
    except Exception:
        return
    try:
        if api == rd.GraphicsAPI.OpenGL:
            gl = controller.GetGLPipelineState()
            out["depth_test"] = bool(gl.depthState.depthEnable)
            out["depth_write"] = bool(gl.depthState.depthWrites)
            out["cull"] = _enum(gl.rasterizer.state.cullMode)
        elif api == rd.GraphicsAPI.Vulkan:
            vk = controller.GetVulkanPipelineState()
            out["depth_test"] = bool(vk.depthStencil.depthTestEnable)
            out["depth_write"] = bool(vk.depthStencil.depthWriteEnable)
            out["cull"] = _enum(vk.rasterizer.cullMode)
    except Exception:
        pass


def _fmt(tex):
    try:
        return tex.format.Name()
    except Exception:
        return "?"


# ---------------------------------------------------------------------------
# Mode: states -- full pipeline dump for one draw
# ---------------------------------------------------------------------------
def run_states(controller, job):
    global _STRUCT_FILE
    _STRUCT_FILE = controller.GetStructuredFile()
    names = resource_names(controller)
    flat = flatten(controller.GetRootActions())
    markers = build_marker_paths(controller.GetRootActions())
    by_eid = {a.eventId: a for a in flat}
    eid = int(job["draw"])
    if eid not in by_eid:
        # Nearest drawcall at/after requested eid.
        draws = [a.eventId for a in flat
                 if a.flags & rd.ActionFlags.Drawcall]
        after = [d for d in draws if d >= eid]
        if after:
            eid = after[0]
        elif draws:
            eid = draws[-1]
        else:
            _die("no drawcall found near eid %d" % eid, job)
    a = by_eid[eid]
    controller.SetFrameEvent(eid, True)
    pipe = controller.GetPipelineState()

    result = {
        "ok": True,
        "mode": "states",
        "rdc": job["rdc"],
        "eid": eid,
        "name": action_name(a),
        "marker": markers.get(eid, ""),
        "num_indices": getattr(a, "numIndices", 0),
        "num_instances": getattr(a, "numInstances", 0),
        "base_vertex": getattr(a, "baseVertex", 0),
        "topology": _enum(_safe(lambda: pipe.GetPrimitiveTopology())),
        "shaders": _shader_dump(pipe, names),
        "blends": _blend_dump(pipe),
        "outputs": _output_dump(pipe, names),
        "viewport": _safe(lambda: _viewport(pipe)),
        "scissor": _safe(lambda: _scissor(pipe)),
    }
    _add_gl_depth_cull(controller, result)
    _write_json(job, result)
    _write_states_md(job, result)


def _safe(fn, default=None):
    try:
        return fn()
    except Exception:
        return default


def _viewport(pipe):
    v = pipe.GetViewport(0)
    return {"x": v.x, "y": v.y, "w": v.width, "h": v.height,
            "minDepth": v.minDepth, "maxDepth": v.maxDepth}


def _scissor(pipe):
    s = pipe.GetScissor(0)
    return {"x": s.x, "y": s.y, "w": s.width, "h": s.height,
            "enabled": bool(getattr(s, "enabled", True))}


def _shader_dump(pipe, names):
    stages = [("vertex", rd.ShaderStage.Vertex),
              ("tess_control", rd.ShaderStage.Tess_Control),
              ("tess_eval", rd.ShaderStage.Tess_Eval),
              ("geometry", rd.ShaderStage.Geometry),
              ("pixel", rd.ShaderStage.Pixel),
              ("compute", rd.ShaderStage.Compute)]
    out = []
    for label, st in stages:
        try:
            sid = pipe.GetShader(st)
        except Exception:
            continue
        if int(sid) == 0:
            continue
        entry = _safe(lambda: pipe.GetShaderEntryPoint(st), "?")
        ro = _safe(lambda: pipe.GetReadOnlyResources(st), []) or []
        bound_tex = []
        for r in ro:
            try:
                rid = r.descriptor.resource if hasattr(r, "descriptor") else r.resources[0].resource
            except Exception:
                rid = None
            if rid is not None and int(rid) != 0:
                bound_tex.append(names.get(int(rid), "resid %d" % int(rid)))
        out.append({
            "stage": label,
            "resid": int(sid),
            "name": names.get(int(sid), "?"),
            "entry": entry,
            "num_readonly": len(ro),
            "bound_textures": bound_tex[:16],
        })
    return out


def _blend_dump(pipe):
    out = []
    try:
        blends = pipe.GetColorBlends()
    except Exception:
        return out
    for i, cb in enumerate(blends):
        out.append({"target": i, "desc": blend_to_str(cb)})
    return out


def _output_dump(pipe, names):
    out = {"color": [], "depth": None}
    try:
        for o in pipe.GetOutputTargets():
            if int(o.resource) != 0:
                out["color"].append(names.get(int(o.resource),
                                              "resid %d" % int(o.resource)))
    except Exception:
        pass
    try:
        d = pipe.GetDepthTarget()
        if int(d.resource) != 0:
            out["depth"] = names.get(int(d.resource),
                                     "resid %d" % int(d.resource))
    except Exception:
        pass
    return out


# ---------------------------------------------------------------------------
# Report writers
# ---------------------------------------------------------------------------
def _write_json(job, result):
    with open(job["result_json"], "w") as fh:
        json.dump(result, fh, indent=2)


def _md_state_line(r):
    bits = []
    if r.get("blend") is not None:
        bits.append("blend=%s" % r["blend"])
    if r.get("depth_test") is not None:
        bits.append("depthTest=%s" % r["depth_test"])
    if r.get("depth_write") is not None:
        bits.append("depthWrite=%s" % r["depth_write"])
    if r.get("cull") is not None:
        bits.append("cull=%s" % r["cull"])
    return "  ".join(bits)


def _write_attribute_md(job, result):
    t = result["target"]
    reg = result["region"]
    lines = []
    lines.append("# RenderDoc draw attribution")
    lines.append("")
    lines.append("- **Capture:** `%s`" % result["rdc"])
    lines.append("- **Target:** %s (resid %d) %dx%d %s"
                 % (t["name"], t["resid"], t["width"], t["height"],
                    t["format"]))
    lines.append("- **Region:** x=%d y=%d w=%d h=%d  (grid %dx%d = %d samples)"
                 % (reg["x"], reg["y"], reg["w"], reg["h"],
                    result["grid"], result["grid"], result["samples_taken"]))
    lines.append("")
    lines.append("Draws are ordered by how many sampled pixels each draw "
                 "actually **wrote** (topmost visible contributor first), "
                 "then by total coverage.")
    lines.append("")
    lines.append("| # | eid | draw | wrote/hit | verts | inst | VS | PS | state |")
    lines.append("|---|-----|------|-----------|-------|------|----|----|-------|")
    for i, r in enumerate(result["draws"]):
        vs = (r.get("vs") or {}).get("name", "?")
        ps = (r.get("ps") or {}).get("name", "?")
        lines.append("| %d | %d | %s | %d/%d | %d | %d | %s | %s | %s |"
                     % (i + 1, r["eid"], _md_cell(r["name"]),
                        r["write_hits"], r["sample_hits"],
                        r["num_indices"], r["num_instances"],
                        _md_cell(vs), _md_cell(ps),
                        _md_cell(_md_state_line(r))))
    lines.append("")
    lines.append("## Per-draw detail")
    for i, r in enumerate(result["draws"]):
        lines.append("")
        lines.append("### %d. eid %d -- %s" % (i + 1, r["eid"], r["name"]))
        if r.get("marker"):
            lines.append("- **Pass / marker:** %s" % r["marker"])
        lines.append("- **Sampled pixels:** wrote %d, touched %d"
                     % (r["write_hits"], r["sample_hits"]))
        lines.append("- **Geometry:** %d indices x %d instances"
                     % (r["num_indices"], r["num_instances"]))
        if r.get("vs"):
            lines.append("- **Vertex shader:** %s (resid %d)"
                         % (r["vs"]["name"], r["vs"]["resid"]))
        if r.get("ps"):
            lines.append("- **Pixel shader:** %s (resid %d)"
                         % (r["ps"]["name"], r["ps"]["resid"]))
        st = _md_state_line(r)
        if st:
            lines.append("- **State:** %s" % st)
        if r.get("example_pixels"):
            lines.append("- **Example pixels:** %s"
                         % ", ".join("(%d,%d)" % (p[0], p[1])
                                     for p in r["example_pixels"]))
    lines.append("")
    with open(job["result_md"], "w") as fh:
        fh.write("\n".join(lines))


def _write_states_md(job, result):
    lines = []
    lines.append("# RenderDoc pipeline state -- eid %d" % result["eid"])
    lines.append("")
    lines.append("- **Capture:** `%s`" % result["rdc"])
    lines.append("- **Draw:** %s" % result["name"])
    if result.get("marker"):
        lines.append("- **Pass / marker:** %s" % result["marker"])
    lines.append("- **Geometry:** %d indices x %d instances (baseVertex %d)"
                 % (result["num_indices"], result["num_instances"],
                    result["base_vertex"]))
    lines.append("- **Topology:** %s" % result["topology"])
    st = _md_state_line(result)
    if st:
        lines.append("- **Depth / cull:** %s" % st)
    if result.get("viewport"):
        v = result["viewport"]
        lines.append("- **Viewport:** x=%s y=%s w=%s h=%s depth=[%s,%s]"
                     % (v["x"], v["y"], v["w"], v["h"],
                        v["minDepth"], v["maxDepth"]))
    if result.get("scissor"):
        s = result["scissor"]
        lines.append("- **Scissor:** x=%s y=%s w=%s h=%s enabled=%s"
                     % (s["x"], s["y"], s["w"], s["h"], s["enabled"]))
    lines.append("")
    lines.append("## Shaders")
    lines.append("")
    lines.append("| stage | resid | name | entry | RO res | bound textures |")
    lines.append("|-------|-------|------|-------|--------|----------------|")
    for s in result["shaders"]:
        lines.append("| %s | %d | %s | %s | %d | %s |"
                     % (s["stage"], s["resid"], _md_cell(s["name"]),
                        _md_cell(str(s["entry"])), s["num_readonly"],
                        _md_cell(", ".join(s["bound_textures"]))))
    lines.append("")
    lines.append("## Color blends (per target)")
    lines.append("")
    for b in result["blends"]:
        lines.append("- **RT%d:** %s" % (b["target"], b["desc"]))
    lines.append("")
    lines.append("## Output targets")
    o = result["outputs"]
    lines.append("- **Color:** %s" % (", ".join(o["color"]) or "none"))
    lines.append("- **Depth:** %s" % (o["depth"] or "none"))
    lines.append("")
    with open(job["result_md"], "w") as fh:
        fh.write("\n".join(lines))


def _md_cell(s):
    return str(s).replace("|", "\\|").replace("\n", " ")


# ---------------------------------------------------------------------------
# Entry
# ---------------------------------------------------------------------------
_STRUCT_FILE = None


def main():
    cap = controller = None
    try:
        cap, controller = open_capture(JOB["rdc"])
        mode = JOB.get("mode")
        if mode == "attribute":
            run_attribute(controller, JOB)
        elif mode == "states":
            run_states(controller, JOB)
        elif mode == "textures":
            run_textures(controller, JOB)
        elif mode == "pixel":
            run_pixel(controller, JOB)
        elif mode == "disasm":
            run_disasm(controller, JOB)
        else:
            _die("unknown mode: %r" % mode, JOB)
    except SystemExit:
        raise
    except Exception as e:
        _die("worker crash: %r\n%s" % (e, traceback.format_exc()), JOB)
    finally:
        try:
            if controller:
                controller.Shutdown()
            if cap:
                cap.Shutdown()
        except Exception:
            pass


main()
# The Qt UI has not opened yet (script runs before it). Hard-exit so the
# process never spawns a window and never hangs a headless run.
os._exit(0)
