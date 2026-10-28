#include "doctest.h"
#include "objtype.h"
#include "paths.h"
#include "../../GameOS/gameos/utils/mc2log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <fcntl.h>
#include <io.h>
#include <share.h>

TEST_CASE("ObjectTypeManager refuses a failing pak and remains safely empty")
{
    struct State {
        std::string oldPath = objectPath;
        std::filesystem::path scratch = [] {
            const char* path = std::getenv("MC2_SCRATCH");
            std::filesystem::path dir = path && *path ? path : "G:/mc2-tmp";
            std::filesystem::create_directories(dir);
            return dir;
        }();
        std::string stem = (scratch / "mc2_objtype_init_failure").string();
        std::FILE* capture = std::fopen((stem + ".log").c_str(), "w+b");
        ~State() {
            std::strcpy(objectPath, oldPath.c_str());
            mc2::log::setStreamOverrideForTest(mc2::log::Stream::Err, nullptr);
            if (capture) std::fclose(capture);
            std::remove((stem + ".log").c_str());
            std::remove((stem + ".pak").c_str());
        }
    } state;
    objectPath[0] = '\0';
    REQUIRE(state.capture != nullptr);
    mc2::log::setSpec("*:info");
    mc2::log::setStreamOverrideForTest(mc2::log::Stream::Err, state.capture);

    ObjectTypeManager manager;
    auto empty = [&] {
        CHECK(manager.numObjectTypes == 0);
        CHECK(manager.table == nullptr);
        CHECK(manager.tableStore.empty());
        CHECK(manager.typeStore.empty());
        CHECK(manager.retiredTypes.empty());
        CHECK(ObjectTypeManager::objectFile == nullptr);
        for (long id : {-1L, 0L, 45L, 4096L}) {
            CHECK(manager.get(id) == nullptr);
            CHECK(manager.get(id, false) == nullptr);
            CHECK(manager.load(id) == nullptr);
            CHECK(manager.create(id) == nullptr);
        }
    };
    empty();
    REQUIRE_FALSE(std::filesystem::exists(state.stem + ".pak"));
    CHECK(manager.init(state.stem.c_str(), 0, 0) != NO_ERR);
    empty();
    std::fflush(state.capture);
    std::rewind(state.capture);
    std::string logged;
    char buffer[512];
    while (const auto n = std::fread(buffer, 1, sizeof buffer, state.capture))
        logged.append(buffer, n);
    const auto marker = logged.find("[OBJTYPE_INIT_FAILED]");
    REQUIRE(marker != std::string::npos);
    CHECK(logged.find("[OBJTYPE_INIT_FAILED]", marker + 1) == std::string::npos);
    CHECK(logged.find("mc2_objtype_init_failure.pak") != std::string::npos);
    CHECK(logged.find("reason=pak missing or unreadable") != std::string::npos);

    // Failed reinitialization must discard a previously allocated table too.
    {
        PacketFile out;
        REQUIRE(out.create((state.stem + ".pak").c_str()) == NO_ERR);
        out.reserve(1);
        unsigned char value = 0;
        REQUIRE(out.writePacket(0, &value, 1) > 0);
        out.close();
    }
    REQUIRE(manager.init(state.stem.c_str(), 0, 0) == NO_ERR);
    CHECK(manager.numObjectTypes == 4096);
    CHECK(manager.get(-1) == nullptr);
    CHECK(manager.get(4096) == nullptr);
    CHECK(manager.get(45, false) == nullptr);
    REQUIRE_FALSE(std::filesystem::exists(state.stem + "_missing.pak"));
    CHECK(manager.init((state.stem + "_missing").c_str(), 0, 0) != NO_ERR);
    empty();
    {
        struct LockedPak {
            int handle = -1;
            ~LockedPak() { if (handle != -1) _close(handle); }
        } locked;
        REQUIRE(_sopen_s(&locked.handle, (state.stem + ".pak").c_str(),
                        _O_RDONLY | _O_BINARY, _SH_DENYRW, 0) == 0);
        CHECK(manager.init(state.stem.c_str(), 0, 0) != NO_ERR);
        empty();
    }
    {
        std::ofstream out(state.stem + ".pak", std::ios::binary | std::ios::trunc);
        const unsigned int header[4] = {0x12345678, 0, 0, 0};
        out.write(reinterpret_cast<const char*>(header), sizeof header);
    }
    CHECK(manager.init(state.stem.c_str(), 0, 0) == PACKET_OUT_OF_RANGE);
    empty();
    manager.destroy();
    empty();
}
