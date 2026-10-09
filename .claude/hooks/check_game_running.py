import sys, json, subprocess
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
# OWNER RULE: no window for any child process (hidden_kwargs: CREATE_NO_WINDOW + SW_HIDE).
from smoke_lib import nofocus  # noqa: E402

d = json.load(sys.stdin)
cmd = d.get('tool_input', {}).get('command', '')

needs_check = 'mc2.exe' in cmd and ('mc2-win64' in cmd or 'build64' in cmd)
if not needs_check:
    sys.exit(0)

r = subprocess.run('tasklist', capture_output=True, text=True, shell=True, **nofocus.hidden_kwargs())
if 'mc2.exe' in r.stdout.lower():
    print(json.dumps({
        'decision': 'block',
        'reason': 'mc2.exe is running — close the game first to avoid LNK1201 PDB lock errors'
    }))
    sys.exit(2)
