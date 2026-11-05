#!/bin/bash
# Build bc7enc_rdo, cook one albedo and one normal map from random pixels, read both KTX2 files back.
# Usage: W=<scratch dir> bash tools/texcook/cook_selftest.sh   (run from the repo root; box: taskset -c 32-63)
set -euo pipefail
W=${W:?set W to a scratch dir}
mkdir -p "$W"
(cd tools/texcook/bc7enc_rdo && ${CXX:-clang++} -O2 -fopenmp -fno-strict-aliasing bc7enc.cpp bc7decomp.cpp bc7decomp_ref.cpp \
    lodepng.cpp test.cpp rgbcx.cpp utils.cpp ert.cpp rdo_bc_encoder.cpp -o "$W/bc7enc")
python3 -c "
from PIL import Image; import numpy as np
a = (np.random.RandomState(1).rand(256, 128, 4) * 255).astype('uint8')
Image.fromarray(a, 'RGBA').save('$W/t_albedo.png'); Image.fromarray(a, 'RGBA').save('$W/t_normal.png')"
python3 tools/texcook/cook_bc7enc.py --bc7enc "$W/bc7enc" --jobs 4 "$W/t_albedo.png"
python3 tools/texcook/cook_bc7enc.py --bc7enc "$W/bc7enc" --role normal "$W/t_normal.png"
python3 -c "
import sys; sys.path.insert(0, 'tools')
from content_pipeline import texcook
for n in ('t_albedo', 't_normal'):
    vk, w, h, lv = texcook.read_ktx2(open('$W/%s.ktx2' % n, 'rb').read()); print(n, vk, w, h, len(lv))"
