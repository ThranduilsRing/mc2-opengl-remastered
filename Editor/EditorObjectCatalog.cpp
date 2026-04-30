\
#include "stdafx.h"
#include "EditorObjectCatalog.h"

#include <fstream>
#include <sstream>

EditorObjectCatalog::EditorObjectCatalog()
{
}

void EditorObjectCatalog::clear()
{
    m_entries.clear();
}

int EditorObjectCatalog::count() const
{
    return (int)m_entries.size();
}

const EditorObjectCatalogEntry* EditorObjectCatalog::entry(int index) const
{
    if (index < 0 || index >= (int)m_entries.size())
        return 0;

    return &m_entries[index];
}

const EditorObjectCatalogEntry* EditorObjectCatalog::findByCommandId(long commandId) const
{
    for (std::vector<EditorObjectCatalogEntry>::const_iterator it = m_entries.begin(); it != m_entries.end(); ++it)
    {
        if (it->commandId == commandId)
            return &(*it);
    }

    return 0;
}

std::string EditorObjectCatalog::trim(const std::string& value)
{
    std::string::size_type first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return std::string();

    std::string::size_type last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

void EditorObjectCatalog::splitCSVLine(const std::string& line, std::vector<std::string>& out)
{
    out.clear();

    std::string field;
    bool quoted = false;

    for (std::string::size_type i = 0; i < line.length(); ++i)
    {
        char c = line[i];

        if (c == '"')
        {
            if (quoted && i + 1 < line.length() && line[i + 1] == '"')
            {
                field += '"';
                ++i;
            }
            else
            {
                quoted = !quoted;
            }
        }
        else if (c == ',' && !quoted)
        {
            out.push_back(trim(field));
            field.erase();
        }
        else
        {
            field += c;
        }
    }

    out.push_back(trim(field));
}

bool EditorObjectCatalog::loadFromCSV(const char* fileName, long firstCommandId)
{
    clear();

    std::ifstream file(fileName);
    if (!file.good())
        return false;

    std::string line;
    std::vector<std::string> fields;
    long nextCommandId = firstCommandId;
    bool sawHeader = false;

    while (std::getline(file, line))
    {
        if (line.empty())
            continue;

        if (line[0] == '#')
            continue;

        splitCSVLine(line, fields);

        if (!sawHeader)
        {
            sawHeader = true;
            continue;
        }

        if (fields.size() < 7)
            continue;

        EditorObjectCatalogEntry entry;
        entry.menuPath = fields[0];
        entry.displayName = fields[1];
        entry.objectType = fields[2];
        entry.assetName = fields[3];
        entry.groupName = fields[4];
        entry.objectName = fields[5];
        entry.flags = fields[6];
        entry.commandId = nextCommandId++;

        if (!entry.menuPath.empty() && !entry.displayName.empty())
            m_entries.push_back(entry);
    }

    return !m_entries.empty();
}
