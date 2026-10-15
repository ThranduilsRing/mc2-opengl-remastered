//============================================================================
// save_struct_zero.h -- a local save struct whose every byte, padding included, starts as zero.
//
// WHAT: MC2_SAVE_STRUCT(Type, var) declares `Type& var`, a Type constructed in zeroed storage of its own size and alignment.
// WHY: a Save function writes sizeof(Type) bytes of a stack object into the save file after setting only the named fields. `Type var{};` is
// not enough: the compiler value-initialises the members one by one and leaves the padding between them as it found it, so the padding carried
// stack leftovers into the file (two saves of one game state differed in the pilot records' alignment bytes). Constructing in zeroed storage
// leaves the constructor-set defaults as they were and makes the padding zero.
//============================================================================
#ifndef CODE_SAVE_STRUCT_ZERO_H
#define CODE_SAVE_STRUCT_ZERO_H

#include <cstring>
#include <new>
#include <type_traits>

#define MC2_SAVE_STRUCT(Type, var) \
	alignas(Type) unsigned char var##_zeroedBytes[sizeof(Type)] = {}; \
	Type& var = *new (var##_zeroedBytes) Type{}

// WHAT: clears the vftable pointer of a polymorphic sub-object that lives inside a save staging struct.
// WHY: a record is written as a raw memory image, so the embedded object's vftable pointer reached the file. It is an address inside the exe image,
// which Windows relocates on every launch (the whole pointer, not only its low bytes), so two saves of one game state differed at those bytes.
// The loader never reads it (it copy-assigns the fields), and the staging struct is never destroyed, so zeroing it changes nothing but the file.
// The MSVC ABI keeps the vftable pointer in the first pointer-sized slot of a class with no virtual base.
template <class T>
inline void mc2SaveClearVptr(T& polymorphic)
{
	static_assert(std::is_polymorphic_v<T>, "only a class with a vftable has a pointer to clear");
	std::memset(static_cast<void*>(&polymorphic), 0, sizeof(void*));
}

#endif // CODE_SAVE_STRUCT_ZERO_H
