// TacticalOrder operator new/delete stubs.
// The real TacticalOrder uses a custom heap allocator; the harness uses standard new/delete.
#include "tacordr.h"
#include <cstdlib>

void* TacticalOrder::operator new(size_t sz) { return ::operator new(sz); }
void  TacticalOrder::operator delete(void* p) { ::operator delete(p); }
