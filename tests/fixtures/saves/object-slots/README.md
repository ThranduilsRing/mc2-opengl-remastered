Real ObjectSlot records from the preserved packet-F5 first saves:
`E:/mc2-archive-2026-10/proofs/lane9/cxx23-f/f5-continuation-mc2_{10,24}/save1.ims`.
Each file holds repeated little-endian u32 slot index, u32 packet length,
then the exact decompressed tagged packet. No fields were synthesized.
Selection retains the first record for each tag, length, object type and
object class (plus Carnage discriminant). mc2_10 has no gate records.

The CARN fixtures include the self-load failures: mc2_10 slot 3035 and
mc2_24 slot 3105, type 42, objectClass GAMEOBJECT (9), 395 bytes. The codec
consumes all 395 bytes and reproduces them exactly; the former identity rule
incorrectly required type 0 for GAMEOBJECT. Carnage::init(bool,ObjectTypePtr)
calls GameObject::init and retains GAMEOBJECT for active effects too.
All nine class tests decode/re-encode real records and validate slot identity.
