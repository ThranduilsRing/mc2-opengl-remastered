# Tactical Overview — Unit Card / Squad Card UI Design Reference

> Reference notes for the **deferred squad-card / unit-card** feature (see
> `docs/superpowers/specs/2026-06-10-tactical-overview-camera-design.md` →
> "Deferred follow-ups → Squad-card roster at full overview"). Captured
> 2026-06-10. Warno/SupCom-style "military UI" card aesthetic.

This style is very doable. Think of it as a **stack of reusable layers**, not "a card."

What you want from the reference is basically:
1. a cold translucent HUD panel
2. a clear info hierarchy
3. thumbnail + faction + unit role + state
4. strong selected/hover/disabled states

## What makes those cards look like that

The visual recipe:
- blue/teal glassy panels
- thin bright borders
- slight inner shadow / vignette
- real unit thumbnail, not icon-only
- compact all-caps text
- small accent badges for faction, cost, veterancy, quantity, status
- very consistent spacing

The look is less about fancy art and more about:
- controlled padding
- good typography
- 2–3 accent colors only
- a subtle frame treatment

## Break the unit card into pieces

Build one `UnitCard` widget with these subparts:

### 1. Outer frame
A rectangular panel with: dark teal/navy fill, 80–90% opacity, 1px bright border,
slightly darker inner edge, optional soft noise texture. Use a **9-slice frame**
so you can resize without distortion.

Starting palette:
- panel base: `#18485E`
- darker edge: `#0C2A38`
- bright edge: `#5FA2C2`
- text primary: `#EAF6FF`
- text secondary: `#A9C3D2`
- warning/accent: `#F0C04A`
- enemy red: `#C94B4B`
- ally blue: `#52A7D8`

### 2. Thumbnail region
The strongest part of the reference is the unit image inside the card.
- reserve 55–70% of the card for art
- put a thumbnail/render of the unit there
- darken the bottom of the image slightly so text can sit over it if needed
- add a mild vignette or edge fade

No bespoke art? Use orthographic/unit render snapshots, or silhouette-on-gradient
as a fallback. (MC2 note: `SimpleCamera` already renders a single object to a
view — see `code/simplecamera.h` — usable for unit thumbnails.)

### 3. Nation / faction badge
Top-left small badge: flag icon or faction stripe, tiny beveled chip background,
always same placement. Gives instant identity.

### 4. Primary label
Bottom or lower-left: unit name, condensed uppercase font, white/warm-white,
strong contrast. e.g. `MIRAGE V [HE]`, `HARRIER GR.3 [AA]`.

### 5. Secondary label
Smaller and dimmer: category / class / role / trait. e.g. `AIR WING`, `IFV`,
`ATGM`, `RECON`.

### 6. Value/status chips
Tiny dense widgets: point cost, availability count, veterancy, ammo/fuel/HP,
suppression/readiness. Keep as small corner chips or edge badges, NOT giant bars.

## Recommended layout

```text
+--------------------------------------------------+
| [Flag]                          [Cost] [Count]   |
|                                                  |
|   [Unit thumbnail / render / silhouette]         |
|                                                  |
|  UNIT NAME                                       |
|  Role / class / weapon                           |
|                                                  |
| [Vet] [HP] [Ammo] [Fuel]          [Status chip]  |
+--------------------------------------------------+
```

For really small cards, strip to: flag, thumbnail, name, cost/count, one status line.

## Interaction states (5)

- **Normal:** calm blue panel, subtle border, full thumbnail.
- **Hover:** slightly brighter border, faint glow, +5–10% brightness.
- **Selected:** stronger outline, brighter cyan/white edge, corner marker / thicker
  border, slight animated pulse okay.
- **Disabled / unavailable:** desaturate thumbnail, lower opacity, mute text,
  optional diagonal stripe overlay.
- **Damaged / suppressed / out of ammo:** do NOT redesign — add a red/orange chip,
  warning icon, small bar or text state.

## Typography

- condensed sans, uppercase for names, smaller secondary font for metadata,
  tight (not too tight) tracking.
- Name: 14–18px semibold/bold. Meta: 10–12px. Chips: 9–11px.
- "Military UI" typography, not fantasy/rounded.

## Implement as a data-driven component

```cpp
struct UnitCardData {
    TextureHandle thumbnail;
    TextureHandle factionFlag;
    std::string name;
    std::string role;
    int cost;
    int count;
    int veterancy;
    float hp01;
    float ammo01;
    float fuel01;
    UnitCardState state;   // normal, hover, selected, disabled
    TeamAffiliation side;  // ally, enemy, neutral
};

void DrawUnitCard(const Rect& r, const UnitCardData& d);
```

Inside the renderer: (1) frame, (2) thumbnail, (3) top badges, (4) name/meta,
(5) status widgets, (6) state overlay. Tune the style once, update every card.

## To feel like the screenshot specifically — prioritize 6 details

1. Transparent blue panel, not opaque solid card
2. Real unit art inside the card
3. Flag chip in top-left
4. Small numeric badges in top-right
5. Bright white all-caps unit name
6. Clean selection glow, not a heavy cartoon highlight

## What NOT to do

- too many colors; every stat a full bar; thick fantasy borders; center alignment
  for dense cards; 5 font styles; giant icons everywhere. Dense but restrained.

## Fastest production path

- **Pass 1:** 9-slice frame, thumbnail, name, faction flag, cost chip, selected/hover.
- **Pass 2:** status chips, veterancy, disabled/damaged treatment, noise/vignette.
- **Pass 3:** animation polish, rarity/experience visuals, class icons, per-faction accents.

## Practical recommendation

Make one medium "golden" card first and tune until it feels right. Then derive:
small card, selected card, disabled card, enemy-spotted card. Don't design every
variation at once.
