\
#ifndef EDITOR_OBJECT_CATALOG_H
#define EDITOR_OBJECT_CATALOG_H

#include <vector>
#include <string>

struct EditorObjectCatalogEntry
{
    std::string menuPath;
    std::string displayName;
    std::string objectType;
    std::string assetName;
    std::string groupName;
    std::string objectName;
    std::string flags;
    long commandId;
};

class EditorObjectCatalog
{
public:
    EditorObjectCatalog();

    bool loadFromCSV(const char* fileName, long firstCommandId);
    void clear();

    int count() const;
    const EditorObjectCatalogEntry* entry(int index) const;
    const EditorObjectCatalogEntry* findByCommandId(long commandId) const;

private:
    std::vector<EditorObjectCatalogEntry> m_entries;

    static std::string trim(const std::string& value);
    static void splitCSVLine(const std::string& line, std::vector<std::string>& out);
};

#endif
