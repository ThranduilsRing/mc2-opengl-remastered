"""Golden vectors in N1's nm_golden_v1.json format (format_version 1, tools/neural_kernel_harness on claude/neural-n1).

Float tier (N1 compatible): every trained net is zero-padded to a square width x width x layers MLP, weights [k][n]
FP16-exact, inputs FP16-exact, expected = float64 reference of the padded net (raw trunk outputs, out_exp 0). The
int8 tier of N1 (power-of-two scales, int8 requantised activations) is NOT emitted: this decoder uses per-channel
FP32 dequantise and FP16 first/last layers, so the exact arithmetic lives in the `nmat_ext` block (quantised
weights, scales, inputs and reference_decode outputs) for the kernel to match bit-for-bit where integer, within
tolerance_rel where float.
"""
from __future__ import annotations
import json
from pathlib import Path
import numpy as np
import torch

from . import reference_decode as rd

FORMAT_VERSION = 1
FORMAT_DOC = {
    "float_weights": "layers[l].w is [k][n] row-major (width*width), layers[l].b is [n]; every value exactly representable in FP16; nets are zero-padded to square",
    "float_inputs": "x is [row][k]; the first latents+6 columns are the trunk input (u, half vector, difference direction), the rest are zero",
    "nmat_ext": "per-net `quant`: layers[l] = {kind fp16|int8, w [n][k] (int8 values or fp16-exact floats), sw per-output fp32 scale (int8 only), sx fp32 input scale (int8 only), b fp32 [n]}; expected_f/T/R from reference_decode",
    "tolerance": "|got - expected| <= tolerance_rel * max(1, |expected|) for FP outputs",
    "rows": "rows must be a multiple of 16 (the cooperative-matrix tile)",
}


def _fp16(a):
    return np.asarray(a, np.float32).astype(np.float16).astype(np.float32)


def _pad_net(model, width):
    ws, bs = [], []
    for lin in model.layers:
        W, b = _fp16(lin.weight.detach().numpy()), _fp16(lin.bias.detach().numpy())
        P = np.zeros((width, width), np.float32)
        P[:W.shape[0], :W.shape[1]] = W
        pb = np.zeros(width, np.float32)
        pb[:len(b)] = b
        ws.append(P.T.copy())            # [k][n]
        bs.append(pb)
    return ws, bs


def build(model, q, u, wi, wo, name, rows=48, width=64):
    """u [rows,L], wi/wo [rows,3] float32 (rows % 16 == 0)."""
    assert rows % 16 == 0 and len(u) == rows
    model = model.eval()
    ws, bs = _pad_net(model, width)
    with torch.no_grad():
        x14 = model.trunk_input(torch.from_numpy(u).float(), torch.from_numpy(wi).float(), torch.from_numpy(wo).float()).numpy()
    x = np.zeros((rows, width), np.float32)
    x[:, :x14.shape[1]] = _fp16(x14)
    a = x.astype(np.float64)
    for l in range(len(ws)):
        a = a @ ws[l].astype(np.float64) + bs[l].astype(np.float64)
        if l < len(ws) - 1:
            a = np.maximum(a, 0.0)
    fq, Tq, Rq = rd.decode(q, u, wi, wo)
    layers = []
    for L in q["layers"]:
        d = dict(kind=L["kind"], w=[float(v) for v in L["W"].astype(np.float32).ravel()], b=[float(v) for v in L["b"]])
        if L["kind"] == "int8":
            d["sw"], d["sx"] = [float(v) for v in L["sw"]], float(L["sx"])
        layers.append(d)
    return {
        "name": name, "width": width, "layers": len(ws), "out_exp": 0, "rows": rows,
        "x": [float(v) for v in x.ravel()],
        "layer_params": [{"w": [float(v) for v in ws[l].ravel()], "b": [float(v) for v in bs[l]]} for l in range(len(ws))],
        "expected": [float(v) for v in a.ravel()],
        "nmat_ext": {"spec": q["spec"], "u": [float(v) for v in u.ravel()], "wi": [float(v) for v in wi.ravel()],
                     "wo": [float(v) for v in wo.ravel()], "quant": {"layers": layers},
                     "expected_f": [float(v) for v in fq.ravel()], "expected_T": [float(v) for v in Tq],
                     "expected_R": [float(v) for v in Rq]},
    }


def write(path, nets):
    doc = {"format_version": FORMAT_VERSION, "format": FORMAT_DOC, "tolerance_rel": 2.0 ** -8, "nets": nets}
    Path(path).write_text(json.dumps(doc, separators=(",", ":")) + "\n", encoding="utf-8", newline="\n")


def read(path):
    doc = json.loads(Path(path).read_text(encoding="utf-8"))
    if doc.get("format_version") != FORMAT_VERSION:
        raise ValueError("unsupported golden format_version")
    return doc


def run_float_tier(net):
    """Re-evaluate a net's float tier from the file contents (what the kernel harness does); returns [rows,width]."""
    w = net["width"]
    a = np.asarray(net["x"], np.float64).reshape(net["rows"], w)
    for l, lp in enumerate(net["layer_params"]):
        a = a @ np.asarray(lp["w"], np.float64).reshape(w, w) + np.asarray(lp["b"], np.float64)
        if l < net["layers"] - 1:
            a = np.maximum(a, 0.0)
    return a
