import sys, json, os, shutil

d = json.load(sys.stdin)
f = d.get('tool_input', {}).get('file_path', '').replace(chr(92), '/')
ext = f.rsplit('.', 1)[-1] if '.' in f else ''

if 'nifty-mendeleev/shaders/' not in f:
    sys.exit(0)
if ext not in ('frag', 'vert', 'tesc', 'tese', 'geom'):
    sys.exit(0)

dst_base = 'A:/Games/mc2-opengl/mc2-win64-v0.1.1/shaders'
dst_dir = dst_base + '/include' if '/shaders/include/' in f else dst_base
dst = dst_dir + '/' + os.path.basename(f)

shutil.copy2(f, dst)
print('[shader-hook] deployed ' + os.path.basename(f) + ' -> ' + dst_dir)
