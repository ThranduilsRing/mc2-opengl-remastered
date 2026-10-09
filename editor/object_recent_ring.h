#pragma once
// ---------------------------------------------------------------------------
// ObjectRecentRing — most-recently-used list of placed editor objects, keyed by
// (group, indexInGroup). Backs the "Recent" strip of the object companion panel.
//
// Dependency-free (std only) so it can be unit-tested standalone without the
// editor/ImGui link. Behaviour:
//   * push() moves an existing (group,index) to the front (dedupe), else inserts
//     at the front; the list is capped at kCap, oldest dropped.
//   * items() is most-recent-first.
// ---------------------------------------------------------------------------

#include <vector>

struct RecentObject {
    int group;
    int indexInGroup;
    bool operator==(const RecentObject& o) const {
        return group == o.group && indexInGroup == o.indexInGroup;
    }
};

class ObjectRecentRing {
public:
    static const int kCap = 8;

    void push(int group, int indexInGroup) {
        const RecentObject entry{ group, indexInGroup };
        for (size_t i = 0; i < m_items.size(); ++i) {
            if (m_items[i] == entry) {
                m_items.erase(m_items.begin() + i);   // remove, re-insert at front
                break;
            }
        }
        m_items.insert(m_items.begin(), entry);
        if ((int)m_items.size() > kCap)
            m_items.resize(kCap);
    }

    const std::vector<RecentObject>& items() const { return m_items; }
    void clear() { m_items.clear(); }

private:
    std::vector<RecentObject> m_items;   // most-recent first
};
