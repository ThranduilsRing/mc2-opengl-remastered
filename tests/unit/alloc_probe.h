#pragma once
// ScopedLeakCheck: did the code under test give back every allocation it took?
//
// Reads the allocation probe (mclib/alloc_probe.h) that mc2_tests routes its global operator new/delete through
// (alloc_probe_new.cpp). A test builds one, runs the create/copy/move/destroy sequence of the type under test, and checks that
// delta() is zero: the ownership step's per-type proof that a migrated class owns what it allocates.
#include "mclib/alloc_probe.h"

struct ScopedLeakCheck {
	mc2_alloc_probe::Snapshot start = mc2_alloc_probe::snapshot();

	// Live blocks now minus live blocks at construction; negative when the code released something it did not allocate here.
	long long delta() const
	{
		return (long long)mc2_alloc_probe::snapshot().live - (long long)start.live;
	}

	// Live bytes now minus live bytes at construction.
	long long bytesDelta() const
	{
		return (long long)mc2_alloc_probe::snapshot().bytes - (long long)start.bytes;
	}

	// Whether anything allocated at all since construction (a test that proves "no allocation" uses it).
	bool allocated() const
	{
		return mc2_alloc_probe::snapshot().total != start.total;
	}
};
