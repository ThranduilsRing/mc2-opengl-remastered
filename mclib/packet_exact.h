// WHAT: a .pak read that accepts a packet only when it is exactly the size the reader's field or grid holds.
// WHY: pak files are mod content; a packet of another size used to be copied whole into a fixed-size field or grid.
#pragma once

#include <cstddef>
#include <string_view>

class PacketFile;

namespace mc2::pak
{
// True when packet `index` exists, is `bytes` long and was copied into `dst`. Otherwise logs one line naming `what`
// and the packet; `dst` is untouched when the packet is missing or of another size, and may be partly written when
// a compressed packet fails to decode, so a caller treats `dst` as invalid on false.
bool readExact (PacketFile& file, long index, void* dst, std::size_t bytes, std::string_view what);
}
