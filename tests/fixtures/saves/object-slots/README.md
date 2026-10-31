Real ObjectSlot records from the preserved packet-F5 first saves:
`E:/mc2-archive-2026-10/proofs/lane9/cxx23-f/f5-continuation-mc2_{10,24}/save1.ims`.
Each file holds repeated little-endian u32 slot index, u32 packet length,
then the exact decompressed tagged packet. No fields were synthesized.
Selection covers tag, length, object type and object class variants.
mc2_10 has no gate records.

The CARN fixtures include the self-load failures: mc2_10 slot 3035 and
mc2_24 slot 3105, type 42, objectClass GAMEOBJECT (9), 395 bytes. The codec
consumes all 395 bytes and reproduces them exactly; the former identity rule
incorrectly required type 0 for GAMEOBJECT. Carnage::init(bool,ObjectTypePtr)
calls GameObject::init and retains GAMEOBJECT for active effects too.
All nine class tests decode/re-encode real records and validate slot identity.

`cve-metropolitan-BLDG.bin` holds the 77 type-125 building records from the
tick-100 CVE-G metropolitan save in packet-F10. The original failure named
the object type, not the slot: slot 1690 has 198 cells (396 coordinate shorts),
which cannot fit the legacy array of 81 shorts. The fixture also retains
75 zero-count records and one two-cell record from the same state.
Every record is decoded, re-encoded byte-for-byte and validated by the BLDG
test alongside the other eight class tests. No coordinates were synthesized.

Source: `E:/mc2-archive-2026-10/proofs/lane9/cxx23-f/f10-candidate-metropolitan.ims`,
SHA256 `5875f394bc2a4112fbea24b99f6dc220df2493fa35c0af5707124517d31b9a89`.
All 100 pre-save SIMID ticks exactly match the failing F9 trace. Fixture
SHA256: `8d32c86894c3080efb47e17db5b2aca9802d8008d67b041ce5b1b3f3b4b60e87`.
