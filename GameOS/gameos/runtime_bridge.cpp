#include "runtime_bridge.h"
#include "runtime_bridge_protocol.h"
#include "../include/build_identity.h"
#include "../../Gates/gate_cold.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

namespace mc2::bridge {
namespace {
using Clock = std::chrono::steady_clock;
struct Transport {
    std::atomic<bool> stop{false};
    std::atomic<double> hz{0};
    std::atomic<uint64_t> dropped{0};
    std::mutex mutex;
    std::string state, movers;
    std::thread thread;
    Clock::time_point next{}; // main thread only
    ~Transport() { close(); }
    void close() {
        stop = true;
        if (thread.joinable()) thread.join();
    }
    bool wait(SOCKET socket, bool writing) {
        fd_set ready; FD_ZERO(&ready); FD_SET(socket, &ready);
        timeval timeout{0, 100000};
        return select(0, writing ? nullptr : &ready, writing ? &ready : nullptr, nullptr, &timeout) > 0;
    }
    bool transfer(SOCKET socket, char* data, size_t bytes, bool writing) {
        const auto deadline = Clock::now() + std::chrono::seconds(2);
        size_t offset = 0;
        while (offset < bytes && !stop && Clock::now() < deadline) {
            if (!wait(socket, writing)) continue;
            int count = writing ? send(socket, data + offset, int(bytes - offset), 0)
                                : recv(socket, data + offset, int(bytes - offset), 0);
            if (count == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) continue;
            if (count <= 0) return false;
            offset += size_t(count);
        }
        return offset == bytes;
    }
    bool sendJson(SOCKET socket, const Json& message) {
        auto body = message.dump(-1, ' ', false, Json::error_handler_t::replace);
        if (body.empty() || body.size() > maxFrameBytes) return false;
        auto header = frameHeader(uint32_t(body.size()));
        return transfer(socket, reinterpret_cast<char*>(header.data()), header.size(), true) &&
               transfer(socket, body.data(), body.size(), true);
    }
    bool receive(SOCKET socket, Json& message) {
        unsigned char header[4];
        if (!transfer(socket, reinterpret_cast<char*>(header), 4, false)) return false;
        auto size = frameSize(header);
        if (size == 0 || size > maxFrameBytes) return false;
        std::string body(size, '\0');
        if (!transfer(socket, body.data(), body.size(), false)) return false;
        message = Json::parse(body, nullptr, false);
        return !message.is_discarded() && message.is_object();
    }
    void run(unsigned short port, std::string session) {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return;
        // Initial connection plus exactly one reconnect; no network retry loop on the sim thread.
        for (int attempt = 0; attempt < 2 && !stop; ++attempt) {
            SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (socket == INVALID_SOCKET) break;
            u_long nonblocking = 1;
            if (ioctlsocket(socket, FIONBIO, &nonblocking) != 0) { closesocket(socket); break; }
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = htons(port);
            int connected = connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address));
            bool live = connected == 0;
            if (!live && WSAGetLastError() == WSAEWOULDBLOCK) {
                const auto deadline = Clock::now() + std::chrono::seconds(2);
                while (!stop && Clock::now() < deadline && !live) {
                    if (!wait(socket, true)) continue;
                    int error = 0, length = sizeof(error);
                    live = getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length) == 0 && !error;
                    break;
                }
            }
            if (live) live = sendJson(socket, {{"version", 1}, {"type", "hello"}, {"session", session},
                                             {"read_only", true}, {"build_id", std::string(MC2_BUILD_GIT_SHA)}});
            Subscription subscription;
            if (live) std::printf("[BRIDGE v0] ev=session.hello protocol=1\n");
            while (live && !stop) {
                fd_set read; FD_ZERO(&read); FD_SET(socket, &read);
                timeval timeout{0, 20000};
                int ready = select(0, &read, nullptr, nullptr, &timeout);
                if (ready < 0) break;
                if (ready > 0) {
                    Json message;
                    if (!receive(socket, message)) break;
                    // Wrong session/version is fatal. No control dispatcher exists in P3.
                    if (!message.contains("version") || message["version"] != 1 ||
                        !message.contains("session") || message["session"] != session) break;
                    const auto next = subscribe(message, session);
                    const bool accepted = next.has_value();
                    if (accepted) {
                        subscription = *next;
                        hz = (subscription.state || subscription.movers) ? subscription.hz : 0;
                    }
                    live = sendJson(socket, {{"version", 1}, {"session", session},
                        {"type", accepted ? "ack" : "nack"}, {"seq", message.value("seq", Json(nullptr))}});
                }
                std::string snapshot, transforms;
                {
                    std::lock_guard lock(mutex);
                    snapshot.swap(state); transforms.swap(movers);
                }
                auto emit = [&](std::string_view stream, std::string_view body) {
                    auto payload = Json::parse(body.begin(), body.end(), nullptr, false);
                    if (payload.is_discarded()) return false;
                    return sendJson(socket, {{"version", 1}, {"type", "telemetry"}, {"session", session},
                        {"stream", stream}, {"dropped", dropped.load()}, {"data", std::move(payload)}});
                };
                if (live && subscription.state && !snapshot.empty()) live = emit("runtime.observation", snapshot);
                if (live && subscription.movers && !transforms.empty()) live = emit("mover.transform", transforms);
            }
            hz = 0;
            closesocket(socket);
            std::printf("[BRIDGE v0] ev=session.degraded protocol=1\n");
        }
        WSACleanup();
    }
};
Transport& transport() { static Transport value; return value; }
} // namespace

bool configured() {
    static const bool value = [] {
        const auto port = mc2::gate::get<"MC2_BRIDGE_PORT">();
        return mc2::gate::get<"MC2_BRIDGE">() && port > 0 && port <= 65535 &&
               validSession(mc2::gate::str<"MC2_BRIDGE_SESSION">());
    }();
    return value;
}
bool sampleDue() {
    if (!configured()) return false;
    auto& value = transport();
    if (!value.thread.joinable() && !value.stop) {
        const auto port = static_cast<unsigned short>(mc2::gate::get<"MC2_BRIDGE_PORT">());
        auto session = mc2::gate::str<"MC2_BRIDGE_SESSION">();
        try {
            value.thread = std::thread([&value, port, session] {
                try { value.run(port, session); }
                catch (...) { value.hz = 0; } // Diagnostics must never terminate the game.
            });
        } catch (...) { value.stop = true; }
    }
    const auto now = Clock::now();
    const double hz = value.hz.load();
    if (hz <= 0 || now < value.next) return false;
    value.next = now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / hz));
    return true;
}
void publish(std::string snapshot, std::string movers) {
    auto& value = transport();
    std::unique_lock lock(value.mutex, std::try_to_lock);
    if (!lock || snapshot.size() > maxFrameBytes || movers.size() > maxFrameBytes) { ++value.dropped; return; }
    if (!value.state.empty()) ++value.dropped;
    value.state = std::move(snapshot);
    value.movers = std::move(movers);
}
void shutdown() { if (configured()) transport().close(); }
} // namespace mc2::bridge
