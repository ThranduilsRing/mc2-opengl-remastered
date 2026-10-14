#!/usr/bin/env python3
"""apply_sortweapons_probe - patch a SCRATCH worktree with fixture P3 (Mover::sortWeapons on real mission data, old rule vs the tree's rule) and probe P4
(which sort site sees equal scores, and when). Never run it on the main worktree.

WHAT: P3 appends sortweapons_probe.inc to code/sim_identity.cpp and calls mc2B2SortWeaponsProbe(simFrame) from mc2SimIdentityTraceFrame, so every sim frame that
is a multiple of 30 prints one [B2P3 v1] line per mover-with-a-target and call shape. P4 gives mclib/score_sort.h a sort_scored_at(tag, items, order) that prints
one [B2P4 v1] line for every call whose input holds equal scores (the tag of the call site, the scenario time, the list length, the number of tie groups and the
ids of the tie group that sits at the front of the sorted list: the one a "take the first" consumer reads), and routes the five call sites through it.
WHY a probe and not a mission fixture: stock missions never call sortWeapons (no ABL script does), so the rule has no stock witness; the probe runs the real
scoring (calcAttackChance) on the real movers of a stock mission and sorts the same scores both ways in one exe. P4 names the sort site behind a divergence.
Run:  py -3 apply_sortweapons_probe.py WORKTREE   then build the worktree, run MC2_SIM_IDENTITY_TRACE=1 on mc2_24 (and mc2_16 / mc2_03), then
      py -3 summarize_sortweapons_probe.py LOG
"""
import os
import sys

root = sys.argv[1]
here = os.path.dirname(os.path.abspath(__file__))


def read(rel):
    raw = open(os.path.join(root, rel), "rb").read()
    return raw.decode("utf-8").replace("\r\n", "\n"), b"\r\n" in raw


def write(rel, text, crlf):
    open(os.path.join(root, rel), "wb").write((text.replace("\n", "\r\n") if crlf else text).encode("utf-8"))


# ---- P3 ----
text, crlf = read("code/sim_identity.cpp")
if "mc2B2SortWeaponsProbe" in text:
    sys.exit("already patched")
needle = 'extern "C" void mc2SimIdentityTraceFrame(double scenarioTimeNow, long simFrame) {\n'
assert text.count(needle) == 1
inc = open(os.path.join(here, "sortweapons_probe.inc"), encoding="utf-8").read().replace("\r\n", "\n")
# the definition goes in front of mc2SimIdentityTraceFrame so the call below sees it
text = text.replace(needle, "#include <string>\n" + inc + "\n" + needle + "\tmc2B2SortWeaponsProbe(simFrame);\n")
write("code/sim_identity.cpp", text, crlf)

# ---- P4 ----
header, crlf = read("mclib/score_sort.h")
tail = "\n} // namespace mc2\n"
assert header.rstrip("\n").endswith("} // namespace mc2")
probe = r'''
}

extern float scenarioTime;

namespace mc2
{
// P4: the same sort, plus a report of equal scores in the input.
inline void sort_scored_at(const char* tag, std::span<Scored> items, SortOrder order)
{
	int groups = 0;
	std::int32_t front[4] = {-1, -1, -1, -1};
	int frontCount = 0;
	std::size_t n = items.size();
	std::vector<Scored> copy(items.begin(), items.end());
	sort_scored(items, order);
	for (std::size_t i = 0; i < n;)
	{
		std::size_t j = i + 1;
		while (j < n && !std::isnan(items[i].score) && items[j].score == items[i].score)
			++j;
		if (j - i > 1)
		{
			++groups;
			if (i == 0)
				for (std::size_t k = i; k < j && frontCount < 4; ++k)
					front[frontCount++] = items[k].id;
		}
		i = j;
	}
	if (groups)
		std::printf("[B2P4 v1] site=%s t=%.3f n=%d groups=%d front=%d,%d,%d,%d\n", tag, static_cast<double>(scenarioTime), static_cast<int>(n), groups, front[0],
			front[1], front[2], front[3]);
	(void)copy;
}
'''
header = header.rstrip("\n")[: -len("} // namespace mc2")].rstrip("\n") + "\n" + probe + "\n} // namespace mc2\n"
header = header.replace("#include <span>\n", "#include <span>\n#include <cstdio>\n#include <vector>\n", 1)
write("mclib/score_sort.h", header, crlf)

SITES = {
    "code/ablmc2_camera.cpp": ["getMoversWithinRadius"],
    "code/contact.cpp": ["getContacts"],
    "code/mover_combat.cpp": ["calcOptimalRange", "sortWeapons_all", "sortWeapons_list"],
}
for rel, tags in SITES.items():
    t, crlf = read(rel)
    assert t.count("mc2::sort_scored(") == len(tags), (rel, t.count("mc2::sort_scored("))
    for tag in tags:
        t = t.replace("mc2::sort_scored(", 'mc2::sort_scored_at("%s", ' % tag, 1)
    write(rel, t, crlf)
print("patched", root)
