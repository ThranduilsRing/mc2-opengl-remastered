#pragma once
#include <string>
#include <string_view>

namespace mc2::bridge {
// Dev transport only: parent supplies a loopback port and a fresh session GUID.
// The main thread only offers owned JSON; socket I/O belongs to the worker.
bool configured();
bool sampleDue();
void publish(std::string snapshot, std::string movers);
void shutdown();
} // namespace mc2::bridge
