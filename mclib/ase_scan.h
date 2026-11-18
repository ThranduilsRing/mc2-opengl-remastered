#pragma once

#include <cstdio>
#include <cstring>
#include <limits>
#include <unordered_map>

namespace mc2_ase {
enum class IdFormat { vertex, face, normal };

// Immutable, NUL-terminated haystack; query ids are nonnegative. Index the
// first occurrence of every formatted PREFIX, not just whole numeric tokens.
class FirstRecord {
    char* cursor;
    const char* prefix;
    IdFormat format;
    std::unordered_map<int, char*> first;

public:
    FirstRecord(char* text, const char* recordPrefix, IdFormat idFormat)
        : cursor(text), prefix(recordPrefix), format(idFormat) {}

    char* find(int id) {
        if (auto it = first.find(id); it != first.end())
            return it->second;
        while (cursor) {
            char* hit = std::strstr(cursor, prefix);
            if (!hit) {
                cursor = nullptr;
                break;
            }
            cursor = hit + 1;
            const char* suffix = hit + std::strlen(prefix);
            const char* digit = suffix;
            while (*digit == ' ')
                ++digit;
            int value = 0;
            for (; *digit >= '0' && *digit <= '9'; ++digit) {
                const int next = *digit - '0';
                if (value > (std::numeric_limits<int>::max() - next) / 10)
                    break;
                value = value * 10 + next;
                char needle[32];
                const char* pattern = format == IdFormat::vertex ? "% 5d"
                    : format == IdFormat::face ? "% 5d:" : " %d";
                std::snprintf(needle, sizeof(needle), pattern, value);
                if (std::strncmp(suffix, needle, std::strlen(needle)) == 0)
                    first.try_emplace(value, hit);
            }
            if (auto it = first.find(id); it != first.end())
                return it->second;
        }
        return nullptr;
    }
};
} // namespace mc2_ase
