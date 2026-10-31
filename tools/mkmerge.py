"""Write merge briefs; --continue N derives a brief from the gate journals."""
import json
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import console_child

OPS = Path('A:/Games/mc2-ops/briefs')
ROOT = Path('A:/Games/mc2-main')


def continuation(n, root):
    build = root / 'build64-ninja'
    window = json.loads((build / 'merge-window.json').read_text(encoding='utf-8'))
    state = json.loads((build / 'gate-state.json').read_text(encoding='utf-8'))
    if window['head'] != state['head']:
        raise ValueError('gate-state and merge-window HEAD differ; reconcile before continuing')
    tips = window.get('tips')
    if tips is None:  # Older windows did not journal tips; recover second parents.
        result = subprocess.run(['git', '-C', str(root), 'log', '--first-parent', '--merges',
                                 '--reverse', '--format=%P %s', f"{window['base']}..{window['head']}"],
                                check=True, capture_output=True, text=True,
                                **console_child.child_kwargs(redirected=True))
        tips = [line.split(' ', 1)[1] for line in result.stdout.splitlines()]
    rows = state.get('stages', {})
    def passed(name):
        row = rows.get(name, {})
        status = row.get('status', '')
        return (row.get('head') == state['head'] and row.get('exe_sha') == state.get('exe_sha')
                and state.get('exe_sha') is not None
                and (status == 'PASS' or status.startswith(('REUSED:', 'SKIP:')))
                and 'operator stage filter' not in status)
    next_stage = next((name for name in state['order'] if not passed(name)), 'none (gate stages complete)')
    refusals = list(window.get('refusals', [])) + list(state.get('refusals', []))
    refusals += [f"{name}: {row['status']}" for name, row in rows.items()
                 if row.get('status', '').startswith(('FAIL', 'REFUSED'))]
    lines = [f'MERGE WINDOW {n} CONTINUATION', f"HEAD: {state['head']}; base: {window['base']}",
             'Tips landed:']
    lines += [f'- {tip}' for tip in tips] or ['- none recorded']
    lines += [f'Next stage: {next_stage}', 'Open refusals:']
    lines += [f'- {reason}' for reason in refusals] or ['- none']
    lines += [f"Proofs: {state['artefact_dir']}",
              f'First run `py -3 scripts/merge_gate.py --preflight --window {n}`, then continue in mc2-main with `py -3 scripts/merge_gate.py --resume --window {n}`; retain the original window clock.',
              'Do not remerge landed tips or start a second gate. The gate owns its stage leases.']
    return '\n'.join(lines) + '\n'


def main(argv=None, ops=OPS, root=ROOT):
    args = sys.argv[1:] if argv is None else argv
    if len(args) == 2 and args[0] == '--continue':
        n = int(args[1])
        text = continuation(n, root)
        output = ops / f'sol-merge{n}-continue.md'
    else:
        if len(args) not in (3, 4):
            raise ValueError('usage: mkmerge.py N BASE TIPS [NOTES] | --continue N')
        n, base = int(args[0]), args[1]
        tips = Path(args[2]).read_text(encoding='utf-8').strip().splitlines()
        extra = Path(args[3]).read_text(encoding='utf-8').strip() if len(args) == 4 else ''
        text = (ops / f'sol-merge{n - 1}.md').read_text(encoding='utf-8')
        a, b = text.index('Tips, in order'), text.index('Run every ratchet')
        body = 'Tips, in order:\n' + ''.join(f'{i}. {t}\n' for i, t in enumerate(tips, 1))
        body += 'Merge with `py -3 tools/lane_merge.py --batch ...`; generated-file conflicts are union/regenerate; real code conflicts get minimal resolution, and say how.\n'
        body += f'Before any gate run: `py -3 scripts/merge_gate.py --preflight --window {n}`. Run the gate with `--window {n}`; rebuild every shipped executable after the last commit if preflight requests it.\n'
        if extra:
            body += extra + '\n'
        body += ('DISK CLEANUP IS PART OF THIS WINDOW (owner 2026-10-26, hard rule): after the gate, for EVERY tip you merged, '
                 'delete its lane install E:/mc2-installs/<branch with / -> -> (plus any <name>-* / .partial siblings) and the build dirs '
                 '(build64-ninja, build64, build64-tests) of its worktree if that worktree is not running another lane (check '
                 'A:/Games/mc2-ops/LANES), and every temporary stage/snapshot you created. Report the GB freed per item and the free space '
                 'on A:, C: and E: before/after. If a delete is refused by policy, list the exact paths for the orchestrator.\n')
        body += ('HOST LEASE: merge_gate.py takes its own per-stage exclusive holds (captures only). Do NOT take any exclusive hold yourself and never wrap the gate in host_lease hold; run single-stage reruns in normal slots. Report the gate\'s "exclusive host hold total" line.\n')
        text = text[:a] + body + '\n' + text[b:]
        text = text.replace(f'MERGE WINDOW {n - 1}', f'MERGE WINDOW {n}').replace(f'merge{n - 1}', f'merge{n}')
        text = re.sub(r'for [0-9a-f]{7,40}\.\.HEAD', f'for {base}..HEAD', text)
        output = ops / f'sol-merge{n}.md'
    output.write_text(text, encoding='utf-8', newline='\n')
    print(output)
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        sys.exit(str(exc))
