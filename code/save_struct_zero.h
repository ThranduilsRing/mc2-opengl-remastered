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

#include <new>

#define MC2_SAVE_STRUCT(Type, var) \
	alignas(Type) unsigned char var##_zeroedBytes[sizeof(Type)] = {}; \
	Type& var = *new (var##_zeroedBytes) Type{}

#endif // CODE_SAVE_STRUCT_ZERO_H
