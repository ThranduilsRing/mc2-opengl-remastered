// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once
#include "dstd.h"
#include "dheap.h"
#include "gameos.hpp"
#include <cstdlib>
#ifndef NO_ERR
#define NO_ERR 0
#endif
// Compatibility for callers whose storage has not yet moved to containers.
// The former gos heaps never owned allocations or enforced their size.
class UserHeap {
    bool ready_ = false;
public:
    long init(unsigned long, const char* = nullptr, bool = true) { ready_ = true; return NO_ERR; }
    void destroy() { ready_ = false; }
    bool heapReady() const { return ready_; }
    void* Malloc(size_t bytes) { return ready_ ? std::malloc(bytes) : nullptr; }
    long Free(void* p) { std::free(p); return NO_ERR; }
    void* calloc(size_t bytes) { return std::calloc(1, bytes); }
    unsigned long coreLeft() const { return 0; }
    unsigned long totalCoreLeft() const { return 0; }
    unsigned long size() const { return 0; }
    long getLastError() const { return NO_ERR; }
    void setMallocFatals(bool) {}
    void walkHeap(bool = false, bool = false) {}
    void startHeapMallocLog() {}
    void stopHeapMallocLog() {}
    void dumpRecordLog() {}
};
extern UserHeapPtr systemHeap;
