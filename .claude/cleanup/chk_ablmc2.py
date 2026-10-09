import re,glob
defs={}
for f in glob.glob("code/ablmc2*.cpp"):
    for m in re.finditer(r"^void (exec\w+)\s*\(void\)\s*\{",open(f,"rb").read().decode("utf-8","replace"),re.M):
        defs.setdefault(m.group(1),[]).append(f)
dup={k:v for k,v in defs.items() if len(v)>1}
reg=set(re.findall(r"(exec\w+)\s*\)\s*;",open("code/ablmc2.cpp","rb").read().decode("utf-8","replace")))
src_before=__import__("subprocess").run(["git","show","HEAD:code/ablmc2.cpp"],capture_output=True).stdout.decode("utf-8","replace")
print("defs",len(defs),"dups",dup,"registered-but-undefined",sorted(r for r in reg if r not in defs)[:5])
before_defs=set(re.findall(r"^void (exec\w+)\s*\(void\)\s*\{",src_before,re.M))
print("before defs",len(before_defs),"missing now:",sorted(before_defs-set(defs))[:5])
