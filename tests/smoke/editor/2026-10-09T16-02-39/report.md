# Editor smoke 2026-10-09T16-02-39  result=FAIL (14/15)

| Case | Result | Bucket | rc | sec | foliage_count |
|------|--------|--------|----|----|---------------|
| gen_map_basic | PASS | - | 0 | 37.6 | -1 |
| foliage_present | PASS | - | 0 | 36.2 | 73 |
| foliage_missing | PASS | - | 0 | 25.0 | 0 |
| foliage_garbage | PASS | - | 0 | 37.9 | 0 |
| foliage_menu_commands | FAIL | menu_reload_bad | 0 | 107.6 | 73 |
|  | | count after reload = 0 (want >0) | | | |
| outliner | PASS | - | 0 | 22.8 | -1 |
| inspector | PASS | - | 0 | 23.2 | -1 |
| validate | PASS | - | 0 | 23.2 | -1 |
| inspector_edit | PASS | - | 0 | 23.2 | -1 |
| asset_browser | PASS | - | 0 | 23.8 | -1 |
| gameplay_debugger | PASS | - | 0 | 23.7 | -1 |
| undo_history | PASS | - | 0 | 43.6 | -1 |
| place_oob | PASS | - | 0 | 23.6 | -1 |
| gen_save_load:gen+save | PASS | - | 0 | 30.3 | -1 |
| gen_save_load:load | PASS | - | 0 | 17.6 | -1 |
