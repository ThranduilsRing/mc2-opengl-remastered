# Syntax-only compile of one TU with MC2_VULKAN_ISLAND undefined (the island is on in every shipped build; this
# proves the guarded phase helpers do not leave the non-island build referring to missing names).
# usage: py -3 tools/phase/ph_nonvk.py <ninja obj target> <source file name>
import re
import subprocess
import sys
sys.path.insert(0, 'scripts')
from configure_ninja import NINJA, under_vcvars, DEFAULT_TREE
target, src = sys.argv[1], sys.argv[2]
out = subprocess.check_output([NINJA, '-C', DEFAULT_TREE, '-t', 'commands', target], text=True)
cmd = [l for l in out.splitlines() if src in l][-1]
cmd = cmd.replace('-DMC2_VULKAN_ISLAND ', '')
cmd = re.sub(r' -c ', ' /Zs -c ', cmd, 1) if ' -c ' in cmd else cmd + ' /Zs'
cmd = re.sub(r'/Fo\S+|-Fo\S+', '', cmd)
cmd = re.sub(r'/Fd\S+|-Fd\S+', '', cmd)
sys.exit(under_vcvars(cmd))
