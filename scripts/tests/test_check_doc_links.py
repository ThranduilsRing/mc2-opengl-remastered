"""The documentation link contract covers tracked docs, including new staged files."""
import importlib.util
from pathlib import Path
import subprocess


def test_tracked_docs_enforced_local_history_excluded(tmp_path, monkeypatch):
    source = Path(__file__).resolve().parents[1] / 'check-doc-links.py'
    spec = importlib.util.spec_from_file_location('check_doc_links', source)
    checker = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(checker)
    import console_child
    monkeypatch.setattr(checker, 'REPO_ROOT', str(tmp_path))
    (tmp_path / 'docs').mkdir()
    (tmp_path / 'tools').mkdir()
    for name in ('README.md', 'docs/new.md', 'tools/other.md', '.aider.chat.history.md', 'docs/scratch.md'):
        (tmp_path / name).write_text('[broken](missing-file.md)\n', encoding='utf-8')
    for args in (['init'], ['add', 'README.md', 'docs/new.md', 'tools/other.md']):
        subprocess.run(['git', '-C', str(tmp_path), *args], check=True, capture_output=True,
                       **console_child.child_kwargs(redirected=True))
    assert checker.collect_md_files() == ['README.md', 'docs/new.md']
    assert checker.check_file('docs/new.md')[0] == [(1, 'missing-file.md')]
    monkeypatch.setattr('sys.argv', ['check-doc-links.py', '--strict'])
    assert checker.main() == 1
    for name in checker.collect_md_files():
        (tmp_path / name).write_text('# Valid\n', encoding='utf-8')
    assert checker.main() == 0
