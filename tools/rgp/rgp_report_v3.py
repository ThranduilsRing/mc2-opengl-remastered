"""rgp_report_v3.py - per-pass GPU attribution for a decoded AMD RGP (gfx11 SQTT) capture. Replaces rgp_report_v2.py.

v3 keeps v2's wave attribution (code-object anchor + launch order; v2's docstring in the archive explains why marker spans are wrong) and
fixes what the 2026-11-19 Fable review found (A:/Games/mc2-ops/studies/fable-rgp-review-1119.md):
  1. piecewise clock: SE0 shader cycles -> wall ms by interpolating between the decoder's REALTIME pairs (the shader clock drifts +-12%
     inside one capture; v2 used one scale from the first and last pair);
  2. carry-over bucket: waves that launch before the first pass marker belong to the previous frame and get their own row;
  3. occupancy from occ.csv (every wave start/end on SE0, not the ~3% instruction-traced sample in waves.csv);
  4. stage columns (VS = NGG/GS/VS shaders, PS, CS) per pass;
  5. atypical-frame flag: hitch capture frames (busy >> the capture's own QueueEvent median) and one-off single-shader bursts are flagged
     and excluded from `typical_busy_ms`;
  6. anchor fix: a code object that never shows a program-address write (the terrain shaders) is anchored to the pass whose CP window
     holds its first launch, and an unanchored run that launches in a gap between markers gets an explicit "(unlabelled: A..B)" row
     instead of a lump spanning several passes (the "9.4 ms water lump" and "LightLists lump" were that).

usage (extract/decode are NOT in this repo: they come from the external v2 research toolchain under RGP_RESEARCH_ROOT on the maintainer
machine, `rgp_report_v2.py extract <capture.rgp> <workdir>` then `decode <workdir>`; this script only reports a decoded workdir, and its
golden test skips unless MC2_RGP_V3_WORKDIRS is set; pass paths as arguments, run with -I):
  py -3 -I rgp_report_v3.py report <workdir> [--csv out.csv] report a decoded workdir
  py -3 -I rgp_report_v3.py summary <workdir>...             one CSV row per capture
"""
import argparse, bisect, collections, csv, os, re, struct, sys

PGM_REGS = ("2c08", "2c48", "2c88", "2cc8", "2e0c")   # SPI_SHADER_PGM_LO_{PS,VS,HS/ES,GS}, COMPUTE_PGM_LO
STAGE = {"cs": "CS", "ps": "PS", "gs": "VS", "vs": "VS", "es": "VS", "hs": "VS", "ls": "VS"}   # NGG gs_main is the vertex-side stage
CARRY = "(carry-over from previous frame)"
HITCH_RATIO = 1.25     # capture frame GPU ms / median frame GPU ms (QueueEvent sums)
BURST_MS, BURST_FRAC = 1.0, 0.20   # unmarked single run: >= 1 ms and >= 20% of frame busy


def read_csv(path):
    with open(path) as f:
        return list(csv.DictReader(f))


class Clock:
    """Piecewise-linear shader-cycle -> wall-ms map from (shader_cycles, rt_ticks@100MHz) pairs."""
    def __init__(self, pairs):
        p = sorted(set(pairs))
        self.c = [a for a, _ in p]
        self.r = [b for _, b in p]
        if len(p) < 2:
            raise ValueError("need >= 2 REALTIME pairs")

    def ms(self, t):
        """wall ms from the first pair; linear extrapolation with the end segment's slope outside the pair range."""
        i = min(max(bisect.bisect_right(self.c, t) - 1, 0), len(self.c) - 2)
        c0, c1, r0, r1 = self.c[i], self.c[i + 1], self.r[i], self.r[i + 1]
        return (r0 + (t - c0) * (r1 - r0) / (c1 - c0) - self.r[0]) / 1e5

    def seg_k(self, n):
        """ms/cycle over n equal groups of pairs (clock drift view)."""
        step = max(1, (len(self.c) - 1) // n)
        return [((self.r[min(i + step, len(self.c) - 1)] - self.r[i]) / 1e5) / (self.c[min(i + step, len(self.c) - 1)] - self.c[i])
                for i in range(0, len(self.c) - 1, step)]

    def global_k(self):
        return ((self.r[-1] - self.r[0]) / 1e5) / (self.c[-1] - self.c[0])


def clock_pairs(wd, se=0):
    out = []
    for l in open(f"{wd}/out0/info.txt"):
        m = re.match(r"se(\d) realtime shader=(\d+) rt=(\d+)", l)
        if m and int(m.group(1)) == se:
            out.append((int(m.group(2)), int(m.group(3))))
    return out


def union_ms(iv, clock):
    """union length in wall ms of [(c0, c1)] cycle intervals."""
    tot, c0, c1 = 0.0, None, None
    for a, b in sorted(iv):
        if c1 is None or a > c1:
            if c1 is not None:
                tot += clock.ms(c1) - clock.ms(c0)
            c0, c1 = a, b
        else:
            c1 = max(c1, b)
    if c1 is not None:
        tot += clock.ms(c1) - clock.ms(c0)
    return tot


def merge(iv):
    out = []
    for a, b in sorted(iv):
        if out and a <= out[-1][1]:
            out[-1][1] = max(out[-1][1], b)
        else:
            out.append([a, b])
    return out


class Occupancy:
    """resident-wave step function N(t) on SE0 from occ.csv start/end events."""
    def __init__(self, rows):
        ev = sorted((int(r["time"]), 1 if r["start"] == "1" else -1) for r in rows)
        self.t, self.n, self.cum = [], [], []
        n, cum, last = 0, 0, ev[0][0] if ev else 0
        for t, d in ev:
            cum += n * (t - last)
            n += d
            last = t
            self.t.append(t); self.n.append(n); self.cum.append(cum)
        self.cap = 16 * max(1, len({(r["cu"], r["simd"]) for r in rows}))   # 16 waves per SIMD

    def integral(self, t):
        i = bisect.bisect_right(self.t, t) - 1
        return 0 if i < 0 else self.cum[i] + self.n[i] * (t - self.t[i])

    def mean(self, merged):
        span = sum(b - a for a, b in merged)
        return sum(self.integral(b) - self.integral(a) for a, b in merged) / span if span else 0.0


def parse_markers(wd, se=0):
    """-> (events [(t, PUSH|POP|CBSTART|CBEND, arg)], regs [(t, reg, value)]) from the USERDATA_2/3 write stream (as v2)."""
    rows, regs = [], []
    for r in read_csv(f"{wd}/out0/tok_se{se}.csv"):
        if r["kind"] != "REG":
            continue
        t, v = int(r["time"]), int(r["b"], 16)
        regs.append((t, r["a"], v))
        if r["a"] in ("c342", "c343"):
            rows.append((t, v))
    cand = collections.Counter(rows[i + 1][1] for i in range(len(rows) - 1)
                               if rows[i][1] & 0xF in (1, 2) and (rows[i][1] >> 4) & 7 == 0 and rows[i + 1][1] > 0xFFFFF)
    dev = cand.most_common(1)[0][0] if cand else None
    ev, i = [], 0
    while i < len(rows):
        t, v = rows[i]
        if v == 0x2005 and i + 2 < len(rows) and 0 < rows[i + 1][1] <= 96:
            L = rows[i + 1][1]; n = (L + 3) // 4
            s = b"".join(struct.pack("<I", w) for _, w in rows[i + 2:i + 2 + n])[:L].decode("latin1")
            ev.append((t, "PUSH", s[6:] if s.startswith("VKIsl.") else s)); i += 2 + n
        elif v == 0x1005:
            ev.append((t, "POP", "")); i += 1
        elif dev is not None and i + 1 < len(rows) and v & 0xF in (1, 2) and rows[i + 1][1] == dev:
            ev.append((t, "CBSTART" if v & 0xF == 1 else "CBEND", (v >> 7) & 0xFFFFF)); i += 2
        else:
            i += 1
    return ev, regs


def build_passes(ev, last):
    stack, passes = [], []
    for t, k, a in ev:
        if k == "PUSH":
            p = dict(name=a, path="/".join([s["name"] for s in stack] + [a]), depth=len(stack), t0=t, t1=None, idx=len(passes))
            passes.append(p); stack.append(p)
        elif k == "POP" and stack:
            stack.pop()["t1"] = t
    for p in stack:
        p["t1"] = last
    return passes


def innermost_at(passes, t):
    best = None
    for p in passes:
        if p["t0"] <= t < p["t1"] and (best is None or p["depth"] >= best["depth"] and p["t0"] >= best["t0"]):
            best = p
    return best


def gap_label(passes, t):
    """the markers a wave launched in a gap between markers sits between."""
    prev = max((p for p in passes if p["t1"] <= t), key=lambda p: p["t1"], default=None)
    nxt = min((p for p in passes if p["t0"] > t), key=lambda p: p["t0"], default=None)
    return "(unlabelled: %s..%s)" % (prev["name"] if prev else "start", nxt["name"] if nxt else "end")


def classify(passes, t, co, co_anchor, major):
    """pass (or synthetic bucket name) for an unbound wave launched at cycle t: -> (pass|name, kind). Only a wave of a major run
    (>= 8 consecutive waves of one shader) seeds the shader's anchor; stray waves launched while another pass is open do not."""
    p = innermost_at(passes, t)
    if p is not None:
        if major:
            co_anchor.setdefault(co, p)
        return p, "inferred"
    if co in co_anchor:
        return co_anchor[co], "tail"          # late launch of a shader already seen inside a pass window (big indirect draws)
    return gap_label(passes, t), "gap"


def load_waves(wd):
    lab = sorted((int(r["addr"]), int(r["co"]), r["name"]) for r in read_csv(f"{wd}/out0/labels.csv"))
    addrs = [a for a, _, _ in lab]
    waves = []
    for r in read_csv(f"{wd}/out0/waves.csv"):
        pc = int(r["first_pc"]); i = bisect.bisect_right(addrs, pc) - 1
        ok = i >= 0 and pc
        st = STAGE.get(lab[i][2].replace("_amdgpu_", "").replace("_main", ""), "?") if ok else "?"
        waves.append((int(r["begin"]), int(r["end"]), lab[i][1] if ok else -1, st))
    waves.sort()
    return lab, waves


def queue_frames(wd):
    """per submitted frame (QueueEvent api id) -> (sum of cmdbuf GPU ms, first start..last end span ms), full frames only."""
    d = f"{wd}/chunks"
    by = collections.defaultdict(list)
    if os.path.isdir(d):
        for f in os.listdir(d):
            if f.startswith("QueueEvent_") and f.endswith(".bin"):
                q = struct.unpack("<9Q", open(f"{d}/{f}", "rb").read())
                if q[3] and q[7] and q[8] > q[7]:
                    by[q[3]].append((q[7], q[8]))
    return [(sum(b - a for a, b in v) / 1e5, (max(b for _, b in v) - min(a for a, _ in v)) / 1e5) for v in by.values() if len(v) >= 10]


def hitch_check(frames, window_ms):
    """-> (median frame ms, capture frame ms, ratio): the capture frame is the full frame whose span is closest to the trace window."""
    if len(frames) < 3:
        return None
    med = sorted(s for s, _ in frames)[len(frames) // 2]
    cap = min(frames, key=lambda f: abs(f[1] - window_ms))[0]
    return med, cap, cap / med


def match_binds(passes, regs, lab, waves, wd):
    """v2's run <-> program-address-write alignment. -> {wave index: pass} for waves of bound runs."""
    ent = {a >> 8: co for a, co, _ in lab}
    binds = []
    for t, reg, v in regs:
        if reg in PGM_REGS and v in ent:
            co = ent[v]
            if binds and binds[-1][1] == co and t - binds[-1][0] < 400:
                continue
            binds.append((t, co, innermost_at(passes, t)))
    sz = {}
    def sig(co):
        if co not in sz:
            try:
                sz[co] = os.path.getsize(f"{wd}/co/co_{co}.elf") if co >= 0 else -1
            except OSError:
                sz[co] = -1
        return sz[co]
    runs = []
    for i, (b, e, co, _) in enumerate(waves):
        if runs and runs[-1][0] == co:
            runs[-1][2].append(i)
        else:
            runs.append([co, b, [i]])
    major = [r for r in runs if len(r[2]) >= 8]
    runs = []
    for r in major:
        if runs and runs[-1][0] == r[0]:
            runs[-1][2] += r[2]
        else:
            runs.append(r)
    last_q, wp = -1, {}
    for co, b, idxs in runs:
        if len(idxs) < 4 or co < 0:
            continue
        for q in range(last_q + 1, min(len(binds), last_q + 13)):
            if binds[q][2] is not None and sig(binds[q][1]) == sig(co) and binds[q][0] <= b:
                last_q = q
                for i in idxs:
                    wp[i] = binds[q][2]
                break
    return wp, runs


def relabel_family(wd, waves, owner, passes):
    """A shader never seen bound to a pass, launched inside pass T's window, whose permutation family (same disassembly length within 3%)
    runs mostly in another top-level pass M, is not evidence of T: book it to "(family of M launched in T)" (v2 booked the 2 ms
    prop-family block that runs inside the Terrain window to Terrain; the terrain shader itself has a unique, much larger body)."""
    top = lambda o: o["path"].split("/")[0] if isinstance(o, dict) else None
    size = {}
    for co in {w[2] for w in waves if w[2] >= 0}:
        try:
            size[co] = open(f"{wd}/dis/co_{co}.txt", errors="replace").read().count("\n")
        except OSError:
            pass
    fam, anchor = {}, None
    for co in sorted(size, key=size.get):
        if anchor is None or size[co] > 1.03 * size[anchor]:
            anchor = co
        fam[co] = anchor
    tops = collections.defaultdict(collections.Counter)       # co -> Counter(top pass) over its bound/inferred/tail waves
    bound_cos = {waves[i][2] for i, (o, k) in enumerate(owner) if k == "bound"}
    for i, (o, k) in enumerate(owner):
        if top(o) and waves[i][2] in fam:
            tops[waves[i][2]][top(o)] += 1
    for i, (o, k) in enumerate(owner):
        co = waves[i][2]
        if k not in ("inferred", "tail") or not top(o) or co in bound_cos or co not in fam:
            continue
        mates = collections.Counter()
        for m in tops:
            if m != co and fam.get(m) == fam[co]:
                mates.update(tops[m])
        if mates:
            mtop, n = mates.most_common(1)[0]
            if mtop != top(o) and n >= 30:
                owner[i] = ("(family of %s launched in %s)" % (mtop, top(o)), "family")


def co_table(wd, waves, owner, lab, clock):
    """per code object: stage, instruction/sample counts from the disassembly (shader family evidence), busy ms, anchor and how it was decided."""
    by = collections.defaultdict(list)
    for i, w in enumerate(waves):
        by[w[2]].append(i)
    out = []
    for co, idx in by.items():
        try:
            dis = open(f"{wd}/dis/co_{co}.txt", errors="replace").read()
            insts, samples = dis.count("\n"), dis.count("image_sample")
        except OSError:
            insts = samples = -1
        how = collections.Counter((owner[i][0]["path"] if isinstance(owner[i][0], dict) else owner[i][0], owner[i][1]) for i in idx).most_common(1)[0][0]
        out.append(dict(co=co, stage="/".join(sorted({waves[i][3] for i in idx})), insts=insts, samples=samples, waves=len(idx),
                        busy_ms=union_ms([(waves[i][0], waves[i][1]) for i in idx], clock), anchor=how[0], how=how[1]))
    return sorted(out, key=lambda r: -r["busy_ms"])


def analyse(wd):
    clock = Clock(clock_pairs(wd))
    ev, regs = parse_markers(wd, 0)
    last = max(t for t, _, _ in regs)
    passes = build_passes(ev, last)
    lab, waves = load_waves(wd)
    first_t0 = min(p["t0"] for p in passes)
    bound, runs = match_binds(passes, regs, lab, waves, wd)
    co_anchor = {}
    for i in sorted(bound):                                   # bound runs seed the co -> pass anchors in launch order
        co_anchor.setdefault(waves[i][2], bound[i])
    synth = {}
    owner = []                                                # per wave: (pass dict | synthetic name, kind)
    in_major = {i for _, _, idxs in runs for i in idxs}
    carrying = True                                          # leading waves: before the first marker, or the same shader's unbroken run
    for i, (b, e, co, st) in enumerate(waves):
        carrying = carrying and (b < first_t0 or co == waves[0][2])
        if carrying:
            owner.append((CARRY, "carry"))
        elif i in bound:
            owner.append((bound[i], "bound"))
        else:
            owner.append(classify(passes, b, co, co_anchor, i in in_major))
    relabel_family(wd, waves, owner, passes)
    rows_by = collections.defaultdict(list)                   # key -> [(wave idx)]
    for i, (o, kind) in enumerate(owner):
        key = o["path"] if isinstance(o, dict) else o
        rows_by[(key, o["idx"] if isinstance(o, dict) else -1)].append(i)
    occ = Occupancy(read_csv(f"{wd}/out0/occ.csv"))
    def ivs(idx):
        return [(waves[i][0], waves[i][1]) for i in idx]
    def child_idx(p):
        out = []
        for (key, pid), idx in rows_by.items():
            if pid >= 0 and passes[pid] is not p and passes[pid]["path"].startswith(p["path"] + "/") \
               and passes[pid]["t0"] >= p["t0"] and passes[pid]["t1"] <= p["t1"]:
                out += idx
        return out
    rows = []
    def add_row(name, depth, idx, p=None, kind=""):
        iv = ivs(idx)
        incl = iv + (ivs(child_idx(p)) if p else [])
        m = merge(iv)
        r = dict(name=name, depth=depth, kind=kind, excl_ms=union_ms(iv, clock) if iv else 0.0, incl_ms=union_ms(incl, clock) if incl else 0.0,
                 nwaves=len(idx), inferred_ms=union_ms([(waves[i][0], waves[i][1]) for i in idx if owner[i][1] != "bound"], clock) if idx else 0.0,
                 cp_ms=(clock.ms(p["t1"]) - clock.ms(p["t0"])) if p else 0.0,
                 occ_waves=occ.mean(m), occ_pct=100.0 * occ.mean(m) / occ.cap)
        for s in ("VS", "PS", "CS"):
            r[s.lower() + "_ms"] = union_ms([(waves[i][0], waves[i][1]) for i in idx if waves[i][3] == s], clock) if idx else 0.0
        rows.append(r)
    for p in passes:
        add_row(p["name"], p["depth"], rows_by.get((p["path"], p["idx"]), []), p)
    for (key, pid), idx in sorted(rows_by.items(), key=lambda kv: kv[1][0]):
        if pid < 0:
            add_row(key, 0, idx, None, "carry" if key == CARRY else "gap")
    allv = ivs(range(len(waves)))
    busy = union_ms(allv, clock)
    busy_v2 = sum(b - a for a, b in merge(allv)) * clock.global_k()     # v2's one-scale figure, for the golden comparison
    cos = co_table(wd, waves, owner, lab, clock)
    # one-off bursts: a single run of one shader that is a large share of the frame
    bursts = []
    for co, b, idxs in runs:
        iv = [(waves[i][0], waves[i][1]) for i in idxs]
        ms = union_ms(iv, clock)
        if ms >= BURST_MS and ms >= BURST_FRAC * busy and sum(owner[i][1] == "gap" for i in idxs) > len(idxs) // 2:   # work outside every marker
            kinds = collections.Counter(owner[i][0] for i in idxs if owner[i][1] == "gap")
            bursts.append(dict(co=co, ms=ms, waves=len(idxs), where=kinds.most_common(1)[0][0], iv=iv))
    hc = hitch_check(queue_frames(wd), clock.ms(clock.c[-1]))
    qmed = hc[0] if hc else None
    flags = []
    if hc and hc[2] > HITCH_RATIO:
        flags.append("HITCH_FRAME(capture frame %.2f ms vs median frame %.2f ms, x%.2f)" % (hc[1], hc[0], hc[2]))
    for b in bursts:
        flags.append("BURST(co%d %.2f ms in %s)" % (b["co"], b["ms"], b["where"]))
    typical = busy - sum(union_ms(b["iv"], clock) for b in bursts)
    return dict(rows=rows, busy_ms=busy, busy_v2_scale_ms=busy_v2, cos=cos, typical_busy_ms=typical, flags=flags, k_global=clock.global_k(), nwaves=len(waves), qmed=qmed,
                window_ms=clock.ms(clock.c[-1]), carry_ms=sum(r["excl_ms"] for r in rows if r["kind"] == "carry"),
                clock_drift_pct=100.0 * (max(clock.seg_k(50)) / clock.global_k() - 1.0))


def report(wd, csv_path=None):
    a = analyse(wd)
    print("== GPU time per pass (v3: piecewise clock; waves from waves.csv; occupancy from occ.csv on SE0) ==")
    print(f"  {'pass':44s} {'incl':>7s} {'excl':>7s} {'VS':>6s} {'PS':>6s} {'CS':>6s} {'waves':>6s} {'infer':>6s} {'occ w':>6s} {'occ %':>5s}")
    for r in a["rows"]:
        if r["excl_ms"] or r["incl_ms"] or r["kind"]:
            print(f"  {'  ' * r['depth'] + r['name']:44s} {r['incl_ms']:7.3f} {r['excl_ms']:7.3f} {r['vs_ms']:6.3f} {r['ps_ms']:6.3f} {r['cs_ms']:6.3f} "
                  f"{r['nwaves']:6d} {r['inferred_ms']:6.3f} {r['occ_waves']:6.1f} {r['occ_pct']:5.1f}")
    print(f"\n  frame busy (union of waves)   {a['busy_ms']:.3f} ms    typical busy (bursts removed) {a['typical_busy_ms']:.3f} ms")
    print(f"  carry-over from prev frame    {a['carry_ms']:.3f} ms    trace window {a['window_ms']:.3f} ms    clock segment max {a['clock_drift_pct']:+.0f}% vs mean")
    print(f"  v2-scale busy (one clock, v2 method) {a['busy_v2_scale_ms']:.3f} ms")
    print("  atypical flags: " + (", ".join(a["flags"]) if a["flags"] else "none"))
    print("\n== code objects (insts/samples = shader family evidence; how = bound by program-address write | inferred by launch window | tail | gap | carry) ==")
    for c in a["cos"][:10]:
        print(f"  co{c['co']:<3d} {c['stage']:8s} insts {c['insts']:6d} samples {c['samples']:4d} waves {c['waves']:5d} busy {c['busy_ms']:6.3f} ms  {c['how']:8s} {c['anchor']}")
    if csv_path:
        with open(csv_path, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(a["rows"][0].keys()))
            w.writeheader(); w.writerows(a["rows"])
    return a


def summary_row(wd):
    name = os.path.basename(wd.rstrip("/\\"))
    m = re.match(r"(.*?)_(\d{3,4})$", name)
    a = analyse(wd)
    tops = sorted((r for r in a["rows"] if r["excl_ms"] > 0), key=lambda r: -r["excl_ms"])[:8]
    return [m.group(1) if m else name, m.group(2) if m else "", f"{a['busy_ms']:.3f}", f"{a['typical_busy_ms']:.3f}", ";".join(a["flags"]) or "ok"] + \
           [f"{r['name']}={r['excl_ms']:.3f}" for r in tops]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["report", "summary"])
    ap.add_argument("a", nargs="+")
    ap.add_argument("--csv")
    args = ap.parse_args()
    if args.cmd == "summary":
        w = csv.writer(sys.stdout, lineterminator="\n")
        for wd in args.a:
            w.writerow(summary_row(wd))
        return
    report(args.a[0], args.csv)


if __name__ == "__main__":
    main()
