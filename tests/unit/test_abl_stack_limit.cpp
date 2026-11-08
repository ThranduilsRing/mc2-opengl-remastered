// tests/unit/test_abl_stack_limit.cpp
//
// AblExecContext::resetStack on the root stack: the push guard limit is the end of the backing storage, not a fixed item count.

#include "doctest.h"

#include "abl_exec_shim.h"
#include "ablmc2_internal.h"

#include <vector>

TEST_CASE("ABL stack limit equals the storage end after resetStack") {
	std::vector<StackItem> storage(8191);  // fewer items than MAXSIZE_STACK, as the environment allocates
	StackItem* const savedStack = stack;
	const size_t savedCount = stackCount;
	const long savedEternal = eternalOffset;
	stack = storage.data();
	stackCount = storage.size();
	eternalOffset = 3;

	AblExecContext ctx;
	ctx.resetStack();
	CHECK(ctx.stackLimit == storage.data() + storage.size());
	CHECK(ctx.stackLimit < storage.data() + MAXSIZE_STACK);
	CHECK(ctx.tos == storage.data() + 3);
	CHECK(ctx.stackFrameBasePtr == ctx.tos);

	stack = savedStack;
	stackCount = savedCount;
	eternalOffset = savedEternal;
}
