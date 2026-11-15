#pragma once
#include "save_archive.h"
#include <memory>

namespace mc2::net {
inline constexpr std::uint32_t protocolVersion = 1;
inline constexpr std::size_t maxPacketBytes = 256 * 1024;
struct Identity {
    save::Digest executable{}, content{};
    std::string mission;
    std::uint32_t seed = 0;
    bool operator==(const Identity&) const = default;
};
save::Bytes hello(const Identity& identity);
// Returns the specific incompatible field, or empty on an exact match.
std::string checkHello(std::span<const std::uint8_t> packet, const Identity& local);

// Separate from the retail MultiPlayer object: no client/server sim forks.
class Session {
public:
    Session();
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    // An empty host endpoint binds loopback:23460. Only numeric IPv4 is accepted.
    void open(bool host, std::string_view endpoint, Identity identity);
    std::vector<save::Bytes> pump();
    void send(std::span<const std::uint8_t> packet);
    bool connected() const;
    const std::string& error() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
