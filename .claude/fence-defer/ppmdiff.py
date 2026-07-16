"""Compare two binary P6 PPMs: per-channel mean-abs-diff + max diff + %pixels differing."""
import sys

def load(p):
    d = open(p, "rb").read()
    # P6\n<w> <h>\n255\n
    parts = d.split(b"\n", 3)
    assert parts[0] == b"P6", parts[0]
    w, h = map(int, parts[1].split())
    assert parts[2] == b"255"
    px = parts[3]
    assert len(px) >= w * h * 3
    return w, h, px[: w * h * 3]

wa, ha, a = load(sys.argv[1])
wb, hb, b = load(sys.argv[2])
assert (wa, ha) == (wb, hb), f"size mismatch {wa}x{ha} vs {wb}x{hb}"
n = len(a)
tot = 0
mx = 0
ndiff = 0
for i in range(n):
    d = a[i] - b[i]
    if d < 0:
        d = -d
    tot += d
    if d > mx:
        mx = d
    if d:
        ndiff += 1
print(f"{wa}x{ha} MAD={tot/n:.4f} max={mx} diff_bytes={ndiff}/{n} ({100.0*ndiff/n:.2f}%)")
