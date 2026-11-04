#include "mc2_czstring.h"
#include "utilities.h"
#include "gameos_resource.hpp"
#include "utils/mc2log.h"

#include <cstring>

HSTRRES gosResourceHandle = nullptr;

namespace {

void report(mc2::czstring event, unsigned int id, size_t length, int capacity, size_t copied)
{
    mc2::log::event<"RESOURCE_STRING", 1>(mc2::log::Stream::Err,
        "event={} id={} source_length={} capacity={} copied={}",
        event, id, length, capacity, copied);
}

const char* resource(unsigned int id, HSTRRES handle, int capacity)
{
    if (!handle) {
        report("null_handle", id, 0, capacity, 0);
        return nullptr;
    }
    if (!gos_ResourceStringLength(handle, id)) {
        report("missing_id", id, 0, capacity, 0);
        return nullptr;
    }
    const char* text = gos_GetResourceString(handle, id);
    if (!text)
        report("null_provider_result", id, 0, capacity, 0);
    return text;
}

} // namespace

std::string loadResourceString(unsigned int id, HSTRRES handle)
{
    const char* text = resource(id, handle, 0);
    return text ? std::string(text) : std::string();
}

int cLoadString(unsigned int id, char* destination, int capacity, HSTRRES handle)
{
    if (!destination || capacity <= 0) {
        report("invalid_destination", id, 0, capacity, 0);
        return 0;
    }
    std::memset(destination, 0, static_cast<size_t>(capacity));
    const char* text = resource(id, handle, capacity);
    if (!text)
        return 0;

    const size_t length = std::strlen(text);
    // Reject incomplete values so callers cannot mistake a prefix for valid text.
    if (length >= static_cast<size_t>(capacity)) {
        report("too_long", id, length, capacity, 0);
        return 0;
    }
    std::memcpy(destination, text, length);
    return static_cast<int>(length);
}

std::string cLoadString(unsigned int id)
{
    return loadResourceString(id);
}

std::string cLoadString(unsigned int id, int capacity)
{
    std::string value(capacity > 0 ? static_cast<size_t>(capacity) : 0, '\0');
    const int length = cLoadString(id, value.data(), capacity);
    value.resize(static_cast<size_t>(length));
    return value;
}
