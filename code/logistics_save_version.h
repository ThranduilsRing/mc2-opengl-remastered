#ifndef LOGISTICS_SAVE_VERSION_H
#define LOGISTICS_SAVE_VERSION_H
// MECHLAB-LOC-1: the save/export compatibility boundary for revised chassis layouts. PURE (no includes).
//
// A variant on a package chassis whose pack declares LayoutRev > 1 uses coordinates that an older build
// would restore blindly. Two mechanisms keep older builds out:
//   * the variant block carries `LayoutChassis` instead of `Chassis`, so an older reader (which asks for
//     `Chassis`) takes its existing clean reject: a multiplayer .var export is skipped;
//   * a campaign / in-mission save that holds such a variant is stamped 10005; older builds compare the
//     stamp for equality with 10004 and refuse it. Saves without one stay 10004, loadable everywhere.
// This build reads both stamps. (code/logistics_archive.h is a private codec nothing calls; left at 10004.)

namespace mc2::logistics_save {

inline constexpr long kBaseVersion = 10004;      // every build before the layout revision, and saves without a revised variant
inline constexpr long kRevisedVersion = 10005;   // a save holding a variant on a layout-revised package chassis

inline long saveVersionFor(bool holdsRevisedLayoutVariant) { return holdsRevisedLayoutVariant ? kRevisedVersion : kBaseVersion; }
inline bool saveVersionReadable(long v) { return v == kBaseVersion || v == kRevisedVersion; }

// FIT key under which a variant block names its chassis. Stock chassis and layout rev 1 keep `Chassis`.
inline const char* variantChassisKey(int layoutRev) { return layoutRev > 1 ? "LayoutChassis" : "Chassis"; }
inline bool layoutRevised(int layoutRev) { return layoutRev > 1; }

} // namespace mc2::logistics_save

#endif // LOGISTICS_SAVE_VERSION_H
