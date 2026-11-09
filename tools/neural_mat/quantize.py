"""Post-training INT8 quantization of the trunk (study 1.4 / 1.7 playbook).

INT8 symmetric per-output-channel weights; first and last layers stay FP16; per-tensor activation scale from the
99.9th percentile of calibration inputs; layers whose quantized output has cosine < 0.99 vs float stay FP16.
"""
from __future__ import annotations
import numpy as np
import torch

from . import reference_decode as rd

COSINE_GATE = 0.99
PERCENTILE = 99.9


def _fp16_layer(lin):
    return dict(kind="fp16", W=lin.weight.detach().numpy().astype(np.float16), b=lin.bias.detach().numpy().astype(np.float32))


def quantize(model, calib_u, calib_wi, calib_wo):
    model = model.eval()
    q = dict(spec=model.spec(),
             frame_w=model.frame.weight.detach().numpy().astype(np.float16),
             frame_b=model.frame.bias.detach().numpy().astype(np.float16),
             proxy_w0=model.proxy[0].weight.detach().numpy().astype(np.float16),
             proxy_b0=model.proxy[0].bias.detach().numpy().astype(np.float16),
             proxy_w1=model.proxy[2].weight.detach().numpy().astype(np.float16),
             proxy_b1=model.proxy[2].bias.detach().numpy().astype(np.float16),
             layers=[], report=[])
    u = torch.from_numpy(calib_u).float()
    with torch.no_grad():
        x = model.trunk_input(u, torch.from_numpy(calib_wi).float(), torch.from_numpy(calib_wo).float())
    # float reference activations layer by layer (inputs to each linear)
    x = x.numpy()
    n = len(model.layers)
    for i, lin in enumerate(model.layers):
        W, b = lin.weight.detach().numpy(), lin.bias.detach().numpy()
        yf = x @ W.T + b
        if i in (0, n - 1):
            q["layers"].append(_fp16_layer(lin))
            q["report"].append(dict(layer=i, kind="fp16", why="first/last", cosine=1.0))
        else:
            sw = np.maximum(np.abs(W).max(1), 1e-8) / 127.0
            Wq = np.clip(np.rint(W / sw[:, None]), -127, 127).astype(np.int8)
            sx = max(float(np.percentile(np.abs(x), PERCENTILE)), 1e-8) / 127.0
            L = dict(kind="int8", W=Wq, b=b.astype(np.float32), sw=sw.astype(np.float32), sx=np.float32(sx))
            yq = rd.run_layer(L, x.astype(np.float32))
            cos = float((yq * yf).sum() / (np.linalg.norm(yq) * np.linalg.norm(yf) + 1e-12))
            if cos < COSINE_GATE:
                L = _fp16_layer(lin)
                q["report"].append(dict(layer=i, kind="fp16", why="cosine", cosine=cos))
            else:
                q["report"].append(dict(layer=i, kind="int8", why="", cosine=cos))
            q["layers"].append(L)
        # next layer's input follows the quantized path so calibration sees what the decoder sees
        y = rd.run_layer(q["layers"][-1], x.astype(np.float32))
        x = np.maximum(y, 0) if i < n - 1 else y
    return q


def psnr(a, b, peak=None):
    peak = float(np.max(np.abs(b))) if peak is None else peak
    mse = float(np.mean((a - b) ** 2))
    return 99.0 if mse == 0 else 10 * np.log10(peak * peak / mse)
