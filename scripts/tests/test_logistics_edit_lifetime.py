"""Run production inventory retirement bodies against lifetime-checking fake mechs."""
import os
from pathlib import Path
import subprocess

import pytest
import console_child

ROOT = Path(__file__).resolve().parents[2]


def body(path, signature):
    text = (ROOT / "code" / path).read_text()
    start = text.index(signature)
    opening = text.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def probe(negative):
    methods = "\n".join([
        body("logisticsdata_modify.cpp", "void LogisticsData::clearInventory()"),
        body("logisticsdata_modify.cpp", "std::expected<void, int> LogisticsData::cancelMechModfications()"),
        body("logisticsdata_save.cpp", "void LogisticsData::removeMechsInForceGroup()"),
        body("logisticsdata_inventory.cpp", "std::expected<void, int> LogisticsData::sellMech("),
        body("logisticsdata_save.cpp", "void LogisticsData::removeMechFromInventory("),
    ])
    if negative:
        # Exact pre-fix deployment retirement body: must attempt a stale write.
        methods = methods.replace(
            "if ( currentlyModifiedMech && currentlyModifiedMech->getForceGroup() )\n"
            "\t\tcancelMechModfications();", "")
    return r'''
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <expected>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>
namespace mc2 { using czstring = const char*; }
#define gosASSERT(x) assert(x)
struct LogisticsVariant { std::string name = "variant"; };
static std::set<const void*> live;
struct LogisticsMech {
    LogisticsVariant* variant;
    int forceGroup;
    LogisticsMech(LogisticsVariant* v, int fg): variant(v), forceGroup(fg) { live.insert(this); }
    ~LogisticsMech() { live.erase(this); }
    int getForceGroup() { return forceGroup; }
    const std::string& getName() { return variant->name; }
    LogisticsVariant* getVariant() { return variant; }
    void setVariant(LogisticsVariant* v) {
        // Detect the invalid write before touching reclaimed storage.
        if (!live.contains(this)) std::_Exit(86);
        variant = v;
    }
    void setPilot(void*) {}
    int getCost() { return 10; }
};
struct MissionInfo { void incrementCBills(int) {} };
struct LogisticsData {
    static LogisticsData* instance;
    std::vector<std::unique_ptr<LogisticsMech>> inventory;
    LogisticsMech* currentlyModifiedMech = nullptr;
    LogisticsVariant* oldVariant = nullptr;
    std::unique_ptr<LogisticsVariant> modifiedVariant;
    struct DeployedMechSnapshot { std::string variantName; LogisticsVariant* pVariant; };
    std::vector<DeployedMechSnapshot> deployedMechSnapshot;
    MissionInfo info;
    MissionInfo* missionInfo = &info;
    void clearInventory();
    void removeMechsInForceGroup();
    std::expected<void, int> cancelMechModfications();
    std::expected<void, int> sellMech(LogisticsMech*);
    void removeMechFromInventory(mc2::czstring, mc2::czstring);
    std::optional<std::reference_wrapper<LogisticsMech>> getMech(const char*, const char*) {
        return *inventory.front();
    }
    LogisticsMech* add(LogisticsVariant& original, int fg) {
        inventory.push_back(std::make_unique<LogisticsMech>(&original, fg));
        return inventory.back().get();
    }
    void edit(LogisticsMech* mech) {
        if (currentlyModifiedMech) assert(cancelMechModfications());
        currentlyModifiedMech = mech;
        oldVariant = mech->variant;
        modifiedVariant = std::make_unique<LogisticsVariant>(*oldVariant);
        mech->setVariant(modifiedVariant.get());
    }
};
LogisticsData* LogisticsData::instance;
''' + methods + r'''
int main() {
    for (int mode = 0; mode != 4; ++mode) {
        LogisticsVariant original;
        LogisticsData data;
        LogisticsData::instance = &data;
        auto* mech = data.add(original, mode == 0);
        data.edit(mech);
        if (mode == 0) data.removeMechsInForceGroup();
        if (mode == 1) data.clearInventory();
        if (mode == 2) assert(data.sellMech(mech));
        if (mode == 3) data.removeMechFromInventory("variant", "pilot");
        assert(data.inventory.empty());
        // This next edit was the ASan trigger after mission 10.
        data.edit(data.add(original, 1));
        assert(data.cancelMechModfications());
        if (mode == 0) {
            assert(data.deployedMechSnapshot.size() == 1);
            assert(data.deployedMechSnapshot[0].pVariant == &original);
        }
        assert(!data.currentlyModifiedMech && !data.oldVariant && !data.modifiedVariant);
    }
    // Retiring other mechs must leave an undeployed edit intact.
    LogisticsVariant original;
    LogisticsData data;
    LogisticsData::instance = &data;
    auto* kept = data.add(original, 0);
    data.edit(kept);
    data.add(original, 1);
    data.removeMechsInForceGroup();
    assert(data.currentlyModifiedMech == kept && data.modifiedVariant);
    assert(data.cancelMechModfications());
}
'''


@pytest.mark.parametrize("negative", [False, True], ids=["fixed", "pre-fix-stale-write"])
def test_inventory_retirement_ends_edit_before_delete(tmp_path, negative):
    source = tmp_path / "edit_lifetime.cpp"
    source.write_text(probe(negative))
    exe = tmp_path / ("edit_lifetime.exe" if os.name == "nt" else "edit_lifetime")
    compiled = subprocess.run(["clang++", "-std=c++23", "-O0", str(source), "-o", str(exe)],
                              capture_output=True, text=True,
                              **console_child.child_kwargs(redirected=True))
    assert compiled.returncode == 0, compiled.stderr
    result = subprocess.run([str(exe)], capture_output=True, text=True,
                            **console_child.child_kwargs(redirected=True))
    assert result.returncode == (86 if negative else 0), result.stderr
