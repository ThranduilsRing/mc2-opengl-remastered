import subprocess,sys,os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', 'scripts'))
import console_child
VC='C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Auxiliary/Build/vcvars64.bat'
def run(cmd): return subprocess.run(f'call "{VC}" >nul && {cmd}',shell=True, **console_child.child_kwargs())
run('cl /nologo /std:c++23preview /EHsc /MD /O2 /DNDEBUG hardening_probe.cpp /Fe:probe_plain.exe')
run('cl /nologo /std:c++23preview /EHsc /MD /O2 /DNDEBUG /D_MSVC_STL_HARDENING=1 hardening_probe.cpp /Fe:probe_hard.exe')
