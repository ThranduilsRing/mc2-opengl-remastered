# World limits (E6, base 091a7b943)

| Limit | Enforcement / representation | Audit result |
|---|---|---|
| MOVE side 3072; terrain authored in whole blocks (3060 usable) | `terrain.h`, `globalmap_limits.h`, `move_internal.h`; signed 16-bit path cells | Fits; assertions pin coordinate and squared-index products to their widths. |
| Areas 30000; doors 32766 + 2 scratch; 255 doors/area | `globalmap_limits.h`, global-map reader/builder; signed 16-bit identities, 32-bit counts/products | Fits. Mapgen allowed 32767 areas: writer now refuses at 30000 and rejects sides above 3072. |
| Movers 1024 | `dmovemgr.h` (`MAX_MOVERS`), object-manager mover arrays; mover identities are no longer bytes | Raised from 255. Fixed `MAX_MOVERS` arrays grow 4x (tens of KB at most). The sensor pool stays 512 and its exhaustion is handled (`bldng.cpp`). |
| Warriors / team roster | `warrior.h`, `dteam.h`, `team.cpp` | 120 was narrower than 255 movers. Runtime storage now derives from mover cap (256 warrior slots, including reserved slot zero). Legacy warrior layout and 120-entry team prefix stay frozen; larger team rosters have bounded portable suffixes. |
| Sensor pool 512; contacts/sensor | `dcontact.h`, `contact.h`, `contact.cpp` | Contacts derive from the mover cap; sensor indices use 16 bits with 65535 absent. Counts use 16 bits, spotter identities use 32 bits, and packed contacts use 31 handle bits plus the visual flag. All are runtime only. |
| Team contacts / scratch | `contact.h`, `contact.cpp`, `dteam.h` | Team contact arrays and scored scratch derive from `MAX_MOVERS`; per-team sensor storage now covers the full 512-sensor pool; team contact indices use 16 bits, absent 65535. |
| Player queue 16/warrior | `warrior.h`, `warrior_queue.cpp`, `warrior_state.cpp` | Shared pool 2000 could not cover all warriors; now cap product (4096). Count remains signed byte; cursor is 32-bit and checked on load. Marker pointers remain null in files. |
| Save packet directory | `saveload.cpp`, `MOVE_saveData(nullptr, 0)` | Fixed 20000 slots could underallocate for large door graphs and object counts. Reservation now uses the existing count-only MOVE writer plus bounded pilot/team/commander overhead and actual object capacity, retaining 20000 as the stock minimum. |
| Saved global route / cursor | `move.h`, `warrior.h`, `warrior_state.cpp` | Runtime 256 steps / 16-bit counters exceeded the frozen 50-step / byte-counter save record. V2 suffix now stores all 256 steps, the full cursor and goal-path cache; local progress remains in the native prefix. Legacy long routes still replan. |
| Typed queue 4096 pending, 8192 receipts, recipients | `tactical_order_queue.h/.cpp` | Recipients 128 increased to the mover cap (1024). Bounded 32-bit sequence counts; full-capacity codec test. Other queue caps unchanged. |
| Terrain chunk target 20 bits; point coordinates 10 bits | `terrain_part_chunk.h`, `tacordr.cpp` | Process-local terrain indices are unsafe across loads. New WQT2 queue suffix stores 32-bit watch IDs, restored independently of the registry. Old big-map compact targets remain conservatively dropped. Point queues retain full float coordinates; attack-point queues now use that same field instead of the 10-bit chunk coordinates. Legacy network chunks remain bounded; multiplayer expansion is outside this change. |
| Picking | `camera_project.cpp`, `missiongui_update.cpp`, `objmgr_pick.cpp` | Terrain ray picking uses map bounds; object picking walks actual object counts. No fixed 720-cell picking scratch remains in these paths. |
| Editor / mission writer | `EditorData_save.cpp`, `MissionValidation.cpp`, `tools/mapgen/mission_writer.py` | Editor refuses MOVE sides above the shared cap before allocation. Mission writer intentionally supports only square 180..720, multiples of 60; all fit the reader. MOVE writer separately rejects excess dimensions/topology. |

WQT2 is a bounded, little-endian v2 target supplement to each unchanged native
warrior packet. New files carry `MC2SAVE.WQT2` so older engines reject them safely;
this reader also accepts the legacy release stamp. The mission-stable watch table is restored by the object manager.
Queue count, cursor, flags, markers, order parameters and active orders retain their
legacy fields. Save copies now preserve the active/last order ID, last move goal
and status, which ordinary tactical-order assignment resets. Target identities no longer depend on network roster unpacking or
the terrain registry. This is not a conversion of the entire legacy mission save
to a portable schema. Legacy packets without the suffix still load.

Sim architecture packet 1 adds runtime entity IDs (legacy slot plus session and
generation) and ordered spatial candidates. Neither changes the serialized slot
table. `MAX_MOVERS` is 1024 while the sensor pool stays 512 (`MAX_SENSORS`); pool
exhaustion is handled. Saves write the first 256 pilot packets plus any used higher
slot, so stock missions are byte-identical; a save with a used pilot above slot 256
cannot be read by an older build.
