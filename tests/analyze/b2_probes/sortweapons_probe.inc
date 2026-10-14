// sortweapons_probe.inc -- B2.4 fixture P3: Mover::sortWeapons on real mission data, the new rule against the old SortList on the same scores.
// Included at the end of code/sim_identity.cpp of a SCRATCH worktree by apply_sortweapons_probe.py (never committed into the game).
//
// WHAT: every PROBE_EVERY sim frames, for every mover that has a current target, it scores the mover's weapons the way sortWeapons does, sorts them with
// (a) the old rule (the removed SortList: a 4000-slot array padded with -3.4E38 and the C runtime qsort, descending) and (b) the real Mover::sortWeapons of
// the tree, in BOTH call shapes (listSize -1 = all weapons; an explicit list with a trailing -1 "none" slot), and prints one line per call.
// WHY: stock missions never call sortWeapons, so no SIMID window can show what the rule does to it; this runs the real scoring on real movers.
#include <cstdlib>

namespace b2p3
{
struct OldNode
{
	float value;
	unsigned long id;
};

static int descendingCompare(const void* a, const void* b)
{
	const float v1 = static_cast<const OldNode*>(a)->value;
	const float v2 = static_cast<const OldNode*>(b)->value;
	return (v1 > v2) ? -1 : ((v1 < v2) ? 1 : 0);
}

// The old call: slot ids/values set for the first n items, the rest left at id=i / value=-3.4E38, then qsort over all 4000 slots.
static void oldSort(const long* ids, const float* values, int n, long* outIds)
{
	static OldNode list[4000];
	for (int i = 0; i < 4000; ++i)
	{
		list[i].id = i;
		list[i].value = float(-3.4E38);
	}
	for (int i = 0; i < n; ++i)
	{
		list[i].id = ids[i];
		list[i].value = values[i];
	}
	qsort(list, 4000, sizeof(OldNode), descendingCompare);
	for (int i = 0; i < n; ++i)
		outIds[i] = static_cast<long>(list[i].id);
}

static void oneCall(long simFrame, MoverPtr m, bool explicitList)
{
	GameObjectPtr target = m->getPilot()->getCurrentTarget();
	long aimLocation = -1;
	if (m->getPilot()->getCurTacOrder()->isCombatOrder())
		aimLocation = m->getPilot()->getCurTacOrder()->attackParams.aimLocation;
	const int nw = m->numWeapons;
	if (nw <= 0 || nw > 31)
		return;
	int weaponList[32];
	long valueList[32];
	long ids[32];
	float scores[32];
	int n = 0;
	for (int i = 0; i < nw; ++i)
	{
		ids[n] = m->numOther + i;
		scores[n] = m->calcAttackChance(target, aimLocation, scenarioTime, ids[n], 0.0, nullptr);
		++n;
	}
	if (explicitList)
	{
		ids[n] = -1;
		scores[n] = -999.0f;
		++n;
	}
	for (int i = 0; i < n; ++i)
		weaponList[i] = static_cast<int>(ids[i]);
	long oldOrder[32];
	oldSort(ids, scores, n, oldOrder);
	const long rc = m->sortWeapons(weaponList, valueList, explicitList ? n : -1, WEAPONSORT_ATTACKCHANCE, true);
	bool ties = false;
	for (int i = 0; i < n && !ties; ++i)
		for (int j = i + 1; j < n; ++j)
			if (scores[i] == scores[j])
			{
				ties = true;
				break;
			}
	bool differ = false;
	for (int i = 0; i < (explicitList ? n : nw); ++i)
		if (oldOrder[i] != weaponList[i])
			differ = true;
	std::string oldS, newS, scoreS;
	for (int i = 0; i < n; ++i)
	{
		oldS += (i ? "," : "") + std::to_string(oldOrder[i]);
		newS += (i ? "," : "") + std::to_string(weaponList[i]);
		scoreS += (i ? "," : "") + std::to_string(static_cast<int>(scores[i] * 1000.0f));
	}
	mc2::fmt::print_out("[B2P3 v1] simframe={:d} mover={:d} shape={} n={:d} rc={:d} ties={:d} differ={:d} scores_x1000={} old={} new={}\n",
		mc2::fmt::as_i32(simFrame), mc2::fmt::as_i32(m->getHandle()), explicitList ? "list" : "all", mc2::fmt::as_i32(n), mc2::fmt::as_i32(rc),
		mc2::fmt::as_i32(ties ? 1 : 0), mc2::fmt::as_i32(differ ? 1 : 0), scoreS.c_str(), oldS.c_str(), newS.c_str());
}
} // namespace b2p3

extern "C" void mc2B2SortWeaponsProbe(long simFrame)
{
	if (!ObjectManager || (simFrame % 30) != 0)
		return;
	for (long i = 0; i < ObjectManager->getNumMovers(); ++i)
	{
		MoverPtr m = ObjectManager->getMover(i);
		if (!m || !m->getExists() || m->isDisabled() || !m->getPilot() || !m->getPilot()->getCurrentTarget())
			continue;
		b2p3::oneCall(simFrame, m, false);
		b2p3::oneCall(simFrame, m, true);
	}
}
