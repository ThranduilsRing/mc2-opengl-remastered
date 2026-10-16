#pragma once
#include "MissionDocument.h"

namespace editor::document {
// Mirrors mclib/inifile.cpp so editor interpretation can move to the shared reader.
class FitReader {
public:
    explicit FitReader(std::string_view bytes);
    std::optional<Span> block(std::string_view name) const;
    std::expected<Scalar, std::string_view> read(std::string_view block, std::string_view prefix, std::string_view key) const;
private:
    std::string_view bytes_;
    std::vector<std::pair<std::string, Span>> blocks_;
};
std::expected<std::string, LoadError> readerText(std::string_view bytes, std::size_t equal);
std::expected<Scalar, std::string_view> readerNumber(std::string_view prefix, std::string wire);
std::string readerNumericWire(std::string_view bytes, std::size_t value);
}
