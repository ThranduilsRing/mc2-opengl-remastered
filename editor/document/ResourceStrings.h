#pragma once

#include "ObjectCatalog.h"

namespace editor::document {

struct ResourceString {
    std::int64_t id{};
    bool found{};
    std::string text, key;
    CatalogSource source;
    std::optional<CatalogSource> aliasSource;
};

// Read-only runtime resource IDs; a missing resolution retains the authored ID.
class ResourceStrings {
public:
    static ResourceStrings load(const FileResolver& resolver, Diagnostic log = {});
    ResourceString resolve(std::int64_t id) const;
    std::vector<ResourceString> search(std::string_view query = {}) const;
private:
    std::vector<ResourceString> entries_;
};

} // namespace editor::document
