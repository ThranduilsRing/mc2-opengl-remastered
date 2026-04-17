import sys, json, subprocess

d = json.load(sys.stdin)
cmd = d.get('tool_input', {}).get('command', '')

needs_check = 'mc2.exe' in cmd and ('mc2-win64' in cmd or 'build64' in cmd)
if not needs_check:
    sys.exit(0)

r = subprocess.run('tasklist', capture_output=True, text=True, shell=True)
if 'mc2.exe' in r.stdout.lower():
    print(json.dumps({
        'decision': 'block',
        'reason': 'mc2.exe is running — close the game first to avoid LNK1201 PDB lock errors'
    }))
    sys.exit(2)
