// GameOS/gameos/vk_caps_json.h -- the tiny JSON writer the per-GPU capability report (vk_device_profile.cpp: writeCapsReport) is built with.
// Header-only and Vulkan-free so tests/unit checks the escaping and the comma/brace discipline offline.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace mc2vk::profile {

class JsonWriter {
public:
    JsonWriter& beginObject(std::string_view key = {}) { open(key, '{'); return *this; }
    JsonWriter& endObject() { out_ += '}'; first_ = false; return *this; }
    JsonWriter& beginArray(std::string_view key = {}) { open(key, '['); return *this; }
    JsonWriter& endArray() { out_ += ']'; first_ = false; return *this; }
    JsonWriter& str(std::string_view key, std::string_view v) { sep(key); quote(v); return *this; }
    JsonWriter& num(std::string_view key, uint64_t v) { sep(key); out_ += std::to_string(v); return *this; }
    JsonWriter& boolean(std::string_view key, bool v) { sep(key); out_ += v ? "true" : "false"; return *this; }
    const std::string& text() const { return out_; }

private:
    void sep(std::string_view key) {
        if (!first_) out_ += ',';
        first_ = false;
        if (!key.empty()) { quote(key); out_ += ':'; }
    }
    void open(std::string_view key, char c) { sep(key); out_ += c; first_ = true; }
    void quote(std::string_view s) {
        out_ += '"';
        for (const char ch : s) {
            const unsigned char u = static_cast<unsigned char>(ch);
            if (ch == '"' || ch == '\\') { out_ += '\\'; out_ += ch; }
            else if (u < 0x20) { static const char* hex = "0123456789abcdef"; out_ += "\\u00"; out_ += hex[u >> 4]; out_ += hex[u & 15]; }
            else out_ += ch;
        }
        out_ += '"';
    }
    std::string out_;
    bool first_ = true;
};

}  // namespace mc2vk::profile
