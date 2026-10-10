<!-- doc-status: current | reviewed:2026-10-11 -->
# comment-tidy

"clang-tidy for comments": finds stale, wrong or bloated comments in C++/GLSL/Python/shell/CMake.
Report-only by default; stdlib-only Python 3; ~10 s for the whole repo.

```
py -3 tools/comment_tidy/comment_tidy.py                         # text summary
py -3 tools/comment_tidy/comment_tidy.py --json out.json         # also write JSON (no FILE = stdout)
py -3 tools/comment_tidy/comment_tidy.py --checks CT001,CT002 --paths GameOS mclib/terrain.cpp
py -3 tools/comment_tidy/comment_tidy.py --fix                   # dry-run diff (CT003 only)
py -3 tools/comment_tidy/comment_tidy.py --fix --apply           # write it
py -3 tools/comment_tidy/test_comment_tidy.py                    # self-tests
```
Options: `--max-block N` (CT007, default 12), `--todo-days N` (CT008, default 365), `--max-print N`, `--jobs N`.
Scope: GameOS mclib code RenderCore GuiRuntime shaders scripts tools (not 3rdparty, data, build*, graphify-out).
"Exists" corpus (env gates, symbols) = every non-3rdparty tracked/untracked-unignored code file in the repo.

| ID | Name | Sev | Fix | Notes |
|---|---|---|---|---|
| CT001 | dead-env | warn | - | `MC2_*` token in a comment, absent from all code (incl. RendererFeatureRegistry.h). Tagged `explains_retirement` when the comment itself says retired/removed |
| CT002 | dead-path | warn | - | `dir/file.ext` that resolves nowhere (repo-relative, file-relative, or path-suffix, case-insensitive) |
| CT003 | stale-line-ref | info | **yes** | `file.ext:NNN[-MMM/...]` -> `file.ext` (comment text only) |
| CT004 | dead-commit | warn | - | hex sha (context-gated) that `git cat-file --batch-check` reports missing |
| CT005 | dead-symbol | info | - | `Func()` / `Class::member` not an identifier anywhere in code (heuristic) |
| CT006 | commented-out-code | warn | - | >=3 consecutive code-looking comment lines |
| CT007 | provenance-essay | info | - | block > N lines, >=40% history/ticket/lane/date lines |
| CT008 | stale-todo | info | - | TODO/FIXME/HACK/XXX whose `git blame` line is older than X days |
| CT009 | lane-era-ref | info | - | worktree / lane / lease / nifty-mendeleev / .claude/worktrees (SIMD-lane style uses filtered) |
| CT010 | tombstone | info | - | "removed/deleted/formerly/was here/used to be" style comments |

JSON findings: `id,name,severity,file,line,end_line,message,snippet,fixable` plus check-specific `token`, `age_days`, `category`, `explains_retirement`.
Comment merging: consecutive standalone `//` or `#` lines form one block; `/* */` is one block; trailing comments stand alone.
