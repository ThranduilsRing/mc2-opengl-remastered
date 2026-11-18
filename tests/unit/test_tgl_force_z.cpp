// forceZ (authored forward-Z) -> reverse-Z clip depth, for the vertex path and the ib_/vb_ matrix path.
#include "doctest.h"

#include "tgl_force_z.h"
#include "ase_scan.h"
#include <algorithm>
#include <numeric>
#include <random>
#include <string>
#include <vector>

TEST_CASE("forceZ: front is near (reverse-Z 1), back is far") {
    CHECK(mc2_tgl::forceZClipDepth(0.0f) == doctest::Approx(1.0f));
    CHECK(mc2_tgl::forceZClipDepth(1.0f) == doctest::Approx(0.0f));
}

TEST_CASE("ASE first records match strstr including numeric prefix aliases") {
    std::mt19937 random(0x415345);
    for (auto format : {mc2_ase::IdFormat::vertex, mc2_ase::IdFormat::face, mc2_ase::IdFormat::normal}) {
        const char* pattern = format == mc2_ase::IdFormat::vertex ? "% 5d"
            : format == mc2_ase::IdFormat::face ? "% 5d:" : " %d";
        const char* prefix = "*MESH_RECORD";
        auto needle = [&](int id) {
            char digits[32];
            std::snprintf(digits, sizeof(digits), pattern, id);
            return std::string(prefix) + digits;
        };
        std::vector<int> ids(150);
        std::iota(ids.begin(), ids.end(), 0);
        for (int id : {10000, 100000, 100001, 100010, 1000000, 2147483647})
            ids.push_back(id);
        for (int variant = 0; variant < 4; ++variant) {
            auto order = ids;
            if (variant)
                std::shuffle(order.begin(), order.end(), random);
            std::string text;
            for (int id : order) {
                if (variant == 2 && id % 3 == 0)
                    continue;
                text += needle(id) + " 0.25\n";
                if (variant == 3)
                    text += needle(id) + " duplicate\n";
            }
            // Noncanonical digits/spaces, overflow, and partial prefix at EOF.
            text += "*MESH_RECORD 00001:*MESH_RECORD    12345678901234567890\n";
            text += "*MESH_RECOR";
            text.push_back('\0');
            text += needle(999) + " following node";
            mc2_ase::FirstRecord scan(text.data(), prefix, format);
            // Shuffled queries must also retain all earlier first occurrences.
            std::shuffle(ids.begin(), ids.end(), random);
            for (int id : ids) {
                const auto key = needle(id);
                CHECK(scan.find(id) == std::strstr(text.data(), key.c_str()));
                CHECK(scan.find(id) == std::strstr(text.data(), key.c_str()));
            }
            CHECK(scan.find(999) == nullptr);
        }
    }
}
