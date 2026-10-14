# ABL debug-string content fixes

Data pass only: no script sources changed by Task F. Sizes include the terminating NUL.
The engine clamps these strings, so undersized declarations cut the debug label.

| Script | Chain starts at line | Variable | Current | Needed size |
|---|---:|---|---:|---:|
| corebrain.abx (coreEscort) | 538 | debugS | char[50] | char[80] |
| corebrain.abx (patrol counters) | 302 | debugS | char[50] | char[59] |
| warriors/hal.abl | 181 | debugS | char[25] | char[59] |
| warriors/mc2_07_enfield1.abl | 183 | debugS | char[25] | char[59] |
| warriors/mc2_07_hunchback1.abl | 183 | debugS | char[25] | char[59] |
| warriors/mc2_07_lrm1.abl | 183 | debugS | char[25] | char[59] |
| warriors/mc2_07_vedette1.abl | 183 | debugS | char[25] | char[59] |
| warriors/mc2_07_vedette2.abl | 183 | debugS | char[25] | char[59] |

The patrol chains need 58 characters plus NUL at full signed-integer width;
char[50] would still truncate that worst case. char[64] is a convenient declaration
for the data pass. coreEscort needs 60 bytes for the audit's sane worst case;
char[80] is the requested headroom for position formatting (the corrupt positions
in the mc2_15 incident produced a 66-byte label).

Source: .claude/cleanup/abl-string-audit.md, section 3.
