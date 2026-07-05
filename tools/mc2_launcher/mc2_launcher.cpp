// mc2_launcher.cpp — MC2 OpenGL Campaign Mod Launcher
//
// Scans ./mods/ for any folder that contains a data/ subdir and treats it as a
// mod — NO mod.json required. Folders sort into THREE buckets:
//   * Compatibility Base — mco-compat / mc2x-compat. Folder name contains
//                         "-compat", or mod.json says type="dependency". These
//                         BOTH replace object2.pak/buildings.csv, so they are
//                         MUTUALLY EXCLUSIVE -> shown as a RADIO group (pick one,
//                         plus a "None" entry). Default = None.
//   * Add-ons            — genuine stackable asset packs (a weapon pack etc.):
//                         any non-campaign folder with data/ that is NOT a base
//                         (mod.json type="assets", or no playable missions and
//                         not "-compat"). Shown as CHECKBOXES (stack any).
//   * Campaign          — has data/missions with real mission content (.fit) and
//                         is not a base/add-on, OR mod.json says type="campaign".
//                         Shown in the campaign list (pick one).
//
// Bucketing rule (load-bearing): the 3 ABL library files corebrain.abx /
// orders.abx / miscfunc.abx are AI plumbing a compat layer ships — they are NOT
// mission content and must never make a folder look like a campaign. (Campaign
// detection keys on .fit, which already excludes those .abx libs; the exclusion
// is made explicit below.) An explicit mod.json "type" always wins over the
// content guess.
//
// For the selected campaign the launcher auto-detects which compatibility base
// it needs and auto-SELECTS that base radio (CK_MCO -> mco-compat radio,
// CK_MC2X -> mc2x-compat radio; Stock/undetected -> the "None" radio):
//   * MechCommander Omnitech (MCO): mission Parts reference ObjectNumber > 1188
//     (mech FitIDs only mco-compat provides) AND warrior brains call magicAttack.
//   * MC2X "fat" packs: ObjectNumber > 1188 but no magicAttack -> mc2x-compat.
//   * otherwise ambiguous (stock-range objtypes, indistinguishable from pure
//     stock) -> base radio stays None; the player picks a base if needed.
//
// On launch it sets MC2_ACTIVE_MOD=<campaign folder> (EMPTY for Stock) and
// MC2_MOD_DEPS = [checked stackable add-ons] then [selected base, if not None],
// comma-separated, in mc2.exe's environment. Add-ons are listed BEFORE the base
// because file.cpp treats MC2_MOD_DEPS as highest-priority-first, so add-ons
// override the base. Stock + None + no add-ons = pure base game (empty deps).
// The "no base picked - are you sure?" warning fires ONLY for a campaign that
// auto-needs a compat base (MCO/MC2X) but whose base radio is still None;
// Stock never warns.
//
// "Import..." button: picks a raw install/pack folder and shells out to the
// python importer brain (tools/mc2_import/import_tool.py) — the launcher is a
// THIN shell. It probes the folder (read-only classify), confirms with the user,
// then runs the import on a worker thread with a modal progress dialog. The
// importer owns all detection / idempotency / staging / atomicity. After import
// the mod list, base radios and add-on checkboxes are refreshed in place (fixed
// slots — never destroyed/recreated).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define UNICODE 0
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commctrl.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define IDC_MODLIST   1001
#define IDC_LAUNCH    1002
#define IDC_CANCEL    1003
#define IDC_LABEL     1004
#define IDC_DESC      1005
#define IDC_BASELBL   1006  // "Compatibility Base (pick one):" label
#define IDC_IMPORT    1007
#define IDC_ADDONLBL  1008  // "Add-ons (stack any):" label
#define IDC_BASE0     1050  // base radios: IDC_BASE0 + slot index (slot 0 = "None")
#define IDC_ADDON0    1100  // (legacy) add-on checkboxes: IDC_ADDON0 + slot index
#define IDC_ADDONLIST 1110  // add-on checkbox ListView (replaces fixed add-on slots)
#define IDC_FASTERWEAPONS 1300  // gameplay toggle (NOT a mod): faster weapons

// Progress-dialog controls.
#define IDC_PROG_CANCEL 1200
#define IDC_PROG_BAR    1201
#define IDC_PROG_TEXT   1202

// Options dialog controls (1400-1499).
#define IDC_OPTIONS        1400  // "Engine Options..." button on main window
#define IDC_OPT_OK         1401
#define IDC_OPT_CANCEL     1402
#define IDC_OPT_CTRL_BASE  1410  // env-var controls start here (IDC_OPT_CTRL_BASE + index)

// Graphics Options dialog controls (1500-1599).
#define IDC_GRAPHICS           1500  // "Graphics..." button on main window
#define IDC_GFX_OK             1501
#define IDC_GFX_CANCEL         1502
#define IDC_GFX_TEXPACK_BASE   1503  // texture-pack radios: +0=None +1..N=packs (up to TEXPACK_DLG_SLOTS)
#define IDC_GFX_TEXQUAL_BASE   1510  // texture-quality radios: +0=Low +1=Med +2=High +3=Ultra
#define IDC_GFX_SHADQUAL_BASE  1520  // shadow-quality radios:  +0=Low +1=Med +2=High
#define IDC_GFX_FRAMECAP_CHK   1530  // "Limit frame rate" checkbox
#define IDC_GFX_FRAMECAP_EDIT  1531  // FPS edit control
#define IDC_GFX_VSYNC_CHK      1532  // "Enable VSync" checkbox
#define IDC_GFX_VRAM_TEXT      1540  // multi-line STATIC for VRAM estimate
#define IDC_GFX_PRESET_BASE    1560  // preset buttons: +0=Low +1=Medium +2=High +3=Ultra

#define WM_APP_IMPORT_DONE (WM_APP + 1)

#define MAX_MODS    64
// FIXED set of base-radio slots: slot 0 is always "None"; the rest hold detected
// base layers (mco-compat, mc2x-compat). 1 None + up to 3 bases.
#define BASE_SLOTS  4
// Add-ons are now an unbounded checkbox ListView (no fixed slot cap).
// Max texture-pack radio slots shown in Graphics dialog (slot 0 = "None"; +1..N = packs).
// Capped to keep the dialog manageable.
#define TEXPACK_DLG_SLOTS 7  // 1 "None" + up to 6 packs

enum CompatKind { CK_UNKNOWN = 0, CK_MCO = 1, CK_MC2X = 2, CK_PENDING = 3 };

struct ModEntry {
    char folderName[256];   // OS directory name — passed as MC2_ACTIVE_MOD / MC2_MOD_DEPS
    char name[256];         // display name (mod.json "name" or folder)
    bool isCompat;          // compatibility layer (base or add-on)
    bool isCampaign;        // has data/missions
    CompatKind needs;       // auto-detected compat for a campaign
    char radioGroup[64];    // mod.json "radioGroup" — routes to a mutually-exclusive radio bank
    char desc[1024];        // mod.json "description" — shown in the launcher description pane
    char envKeys[8][64];    // mod.json "env" — KEY names, applied to the child env when this mod is active
    char envVals[8][256];   // mod.json "env" — matching VALUEs
    int  envCount;
};

static ModEntry s_campaigns[MAX_MODS];
static int      s_campCount = 0;
// Compatibility BASES (mutually exclusive -> radio). Both mco-compat and
// mc2x-compat replace object2.pak/buildings.csv so only one can be active.
static ModEntry s_bases[MAX_MODS];
static int      s_baseCount = 0;
// Stackable ADD-ONS (genuine asset packs -> checkboxes; any number may stack).
static ModEntry s_addons[MAX_MODS];
static int      s_addonCount = 0;
// Texture-quality radio group (radioGroup="texture-quality" -> mutually exclusive).
static ModEntry s_texQual[MAX_MODS];
static int      s_texQualCount = 0;

static HINSTANCE s_hInst;
static HWND      s_hMainWnd;
static HWND      s_hListBox;
static HWND      s_hImport;
static HWND      s_hBase[BASE_SLOTS];     // FIXED radio slots — slot 0 = "None"; never destroy
static HWND      s_hAddonList;            // add-on checkbox ListView (unbounded, scrolls)
static HWND      s_hDescBox;              // multiline read-only description pane
static HWND      s_hFasterWeapons;        // gameplay toggle (NOT a mod): faster weapons

static char      s_launcherDir[MAX_PATH];   // trailing-slash launcher directory
static char      s_modsPath[MAX_PATH];      // <launcherDir>mods\

// ---- Engine Options (MC2_* env vars) -----------------------------------------

// Two control types: checkbox (bool) or label+edit (value).
struct EnvVarDef {
    const char* key;         // env var name, e.g. "MC2_SHADOW_CSM"
    const char* label;       // display label
    bool        isBool;      // true = checkbox; false = static+edit
    const char* defaultVal;  // shown in edit / initial check state; "" = unset by default
    const char* section;     // section header (grouped in dialog)
};

// Curated interactive list.  Excludes:
//   - MC2_DIRECT_FIRE_STRAIGHT / MC2_PROJECTILE_SPEED_MULT (covered by Faster Weapons)
//   - Pure debug/diagnostic vars (MC2_LOG, cost-split, trace, smoke clock, etc.)
//   - Default-ON vars where toggling is rarely useful interactively
static const EnvVarDef k_envVars[] = {
    // --- Rendering ---
    { "MC2_SHADOW_CSM",              "Cascaded shadow maps (CSM)",           true,  "1",    "Rendering" },
    { "MC2_CLOUD_SHADOW",            "Cloud shadows (default ON)",            true,  "1",    "Rendering" },
    { "MC2_SHADOW_BOUNDED_NEAR_FIT", "Crisp near shadows",                    true,  "",     "Rendering" },
    { "MC2_PROP_SHADOW_RECEIVE",     "Building self/prop-on-prop shadows",    true,  "",     "Rendering" },
    { "MC2_STATIC_PROP_PBR_V1",      "Static prop PBR specular",              true,  "1",    "Rendering" },
    { "MC2_STATIC_PROP_AMBIENT_V1",  "Static prop hemisphere ambient",        true,  "1",    "Rendering" },
    { "MC2_STATIC_PROP_IBL_SH",      "Static prop SH-L2 IBL ambient (ON)",    true,  "1",    "Rendering" },
    // --- Display ---
    { "MC2_FORCE_43",                "Force 4:3 aspect (pillarbox widescreen)", true, "",    "Display"   },
    { "MC2_WINDOWED",                "Windowed mode",                          true,  "",    "Display"   },
    { "MC2_WIDTH",                   "Window width (blank = options.cfg)",     false, "",    "Display"   },
    { "MC2_HEIGHT",                  "Window height (blank = options.cfg)",    false, "",    "Display"   },
    // --- Terrain ---
    { "MC2_TERRAIN_LOD_CHUNK",       "Chunk terrain LOD renderer (default ON)", true, "1",   "Terrain"   },
    { "MC2_COLORMAP_KTX2",           "BC7 KTX2 colormap atlas (default ON)",  true,  "1",    "Terrain"   },
    // --- Water / VFX ---
    { "MC2_WATER_SKYTINT",           "Water sky tint",                        true,  "1",    "Water/VFX" },
    { "MC2_WATER_REFLECTION",        "Water SH-L2 sky reflection",            true,  "",     "Water/VFX" },
    { "MC2_VFX_SOFT_PARTICLES",      "Soft particles (depth-fade)",           true,  "",     "Water/VFX" },
    { "MC2_VFX_LIT_PARTICLES",       "Lit particles",                         true,  "",     "Water/VFX" },
    // --- Debug ---
    { "MC2_IMGUI",                   "ImGui overlay",                         true,  "",     "Debug"     },
    { "MC2_IMGUI_INSPECTOR",         "ImGui inspector panel",                 true,  "",     "Debug"     },
    // --- Shadow tuning (value vars) ---
    // NOTE: MC2_SHADOW_MAP_SIZE moved to Graphics Options dialog (shadow quality radios).
    { "MC2_SHADOW_CSM_R0",           "CSM near cascade radius WU (def 512)",  false, "512",  "Shadow Tuning" },
    // --- Cheats (interactive play) ---
    { "MC2_CHEAT_INFINITE_MONEY",    "Infinite Money",                        true,  "",     "Cheats" },
    { "MC2_CHEAT_SALVAGE_ALL",       "Salvage Everything",                    true,  "",     "Cheats" },
    { "MC2_PURCHASE_ALL",            "Purchase All (every mech/weapon/pilot)", true, "",     "Cheats" },
    { "MC2_JUMPJETS_ALL",            "Jump jets on any mech",                 true,  "",     "Cheats" },
    { "MC2_ENGINE_XL",               "XL engines (install for +speed)",       true,  "",     "Cheats" },
};
static const int k_envVarCount = (int)(sizeof(k_envVars) / sizeof(k_envVars[0]));

// In-memory state: for bool vars "1" = set / "" = unset; for value vars: the text or "".
static char s_envVarValues[32][256];  // [k_envVarCount][MAX_VAL]

// Path to launcher_env.json.
static char s_envJsonPath[MAX_PATH];

// Options dialog window handle.
static HWND s_hOptionsDlg = NULL;

// Forward declarations (defined later in the file).
static bool JsonGetString(const char* json, const char* key, char* out, int outSz);
static void SelectBaseRadio(int slot);
static int  SelectedBaseSlot();
static void UpdateForSelection();

// ---- Graphics Options state ---------------------------------------------------
//
// Texture quality tier: 0=Low(128) 1=Medium(256) 2=High(512) 3=Ultra(1024)
// Shadow quality:       0=Low(2048) 1=Medium(4096) 2=High(8192)
// Frame cap: enabled flag + FPS value (0 = use default 60)
// VSync: enabled flag
//
// Engine vars driven by these:
//   MC2_STATICPROP_TEX_TIER  = "128" / "256" / "512" / "1024"
//     (NOTE: engine side must read this var to select the texture resolution tier;
//      launcher sets it here — wire the engine read in MaterialGpu/texarray loader)
//   MC2_SHADOW_MAP_SIZE = "2048" / "4096" / "8192"
//   MC2_FPS_CAP         = "<N>" or unset
//   MC2_VSYNC           = "1" or unset

static int  s_gfxTexQual      = 2;   // default High (512px)
static int  s_gfxShadQual     = 2;   // default High (8192)
static bool s_gfxFrameCapOn   = false;
static int  s_gfxFrameCapFps  = 60;
static bool s_gfxVsync        = false;
// Texture-pack mod selection: 0=None; index i -> s_texQual[i-1].folderName.
// Persisted as "tex_pack_mod" (folder name string) in launcher_env.json.
static int  s_gfxTexPackIdx   = 0;
// Raw folder name loaded from JSON; resolved to s_gfxTexPackIdx after ScanMods.
static char s_gfxTexPackSaved[256] = "";

// ---- Main-page persistence (last_* keys in launcher_env.json) ----------------
// Saved/restored across sessions: campaign folder, base folder, addon list, faster-weapons.
static char s_savedCampaign[256]  = "";   // folder name, "" = Stock
static char s_savedBase[256]      = "";   // folder name, "" = None
static char s_savedAddons[1024]   = "";   // comma-delimited folder names, "" = none
static char s_savedFasterWeapons[4] = ""; // "1" or "0" or ""

static const char* k_texTierValues[]    = { "128", "256", "512", "1024" };
static const char* k_texTierLabels[]    = { "Low (128px)", "Medium (256px)", "High (512px)", "Ultra (1024px)" };
static const char* k_shadSizeValues[]   = { "2048", "4096", "8192" };
static const char* k_shadQualLabels[]   = { "Low (2048)", "Medium (4096)", "High (8192)" };
static const int   k_texVramMB[]        = { 40, 160, 640, 2560 };  // texture array VRAM by tier
static const int   k_shadVramMB[]       = { 64, 256, 1024 };       // shadow map VRAM by quality (4 cascades)

struct GfxPreset { int texQual; int shadQual; bool frameCap; int fps; };
static const GfxPreset k_gfxPresets[] = {
    { 0, 0, true,  60  },   // Low
    { 1, 1, true,  60  },   // Medium
    { 2, 2, true,  165 },   // High
    { 3, 2, false, 165 },   // Ultra (uncapped)
};
static const char* k_gfxPresetLabels[] = { "Low", "Medium", "High", "Ultra" };

// Graphics Options dialog window handle.
static HWND s_hGraphicsDlg = NULL;

static void GfxSetDefaults() {
    s_gfxTexQual      = 2;
    s_gfxShadQual     = 2;
    s_gfxFrameCapOn   = true;
    s_gfxFrameCapFps  = 165;
    s_gfxVsync        = false;
    s_gfxTexPackIdx   = 0;
}

// Load graphics options from launcher_env.json (same file, extra keys).
// Called after LoadEnvJson() so both share the same file.
static void LoadGfxJson() {
    if (!s_envJsonPath[0]) return;
    HANDLE fh = CreateFileA(s_envJsonPath, GENERIC_READ, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE) return;
    static char buf[32768];
    DWORD got = 0;
    ReadFile(fh, buf, sizeof(buf)-1, &got, NULL);
    CloseHandle(fh);
    buf[got] = '\0';

    char val[256] = "";
    // Texture tier
    if (JsonGetString(buf, "MC2_STATICPROP_TEX_TIER", val, sizeof(val))) {
        int tier = atoi(val);
        if      (tier == 128)  s_gfxTexQual = 0;
        else if (tier == 256)  s_gfxTexQual = 1;
        else if (tier == 512)  s_gfxTexQual = 2;
        else if (tier == 1024) s_gfxTexQual = 3;
    }
    // Shadow quality via MC2_SHADOW_MAP_SIZE
    if (JsonGetString(buf, "MC2_SHADOW_MAP_SIZE", val, sizeof(val))) {
        int sz = atoi(val);
        if      (sz == 2048) s_gfxShadQual = 0;
        else if (sz == 4096) s_gfxShadQual = 1;
        else if (sz == 8192) s_gfxShadQual = 2;
    }
    // Frame cap
    val[0] = '\0';
    if (JsonGetString(buf, "MC2_FPS_CAP", val, sizeof(val)) && val[0]) {
        s_gfxFrameCapOn  = true;
        s_gfxFrameCapFps = atoi(val);
        if (s_gfxFrameCapFps <= 0) s_gfxFrameCapFps = 60;
    }
    // VSync
    val[0] = '\0';
    if (JsonGetString(buf, "MC2_VSYNC", val, sizeof(val))) {
        s_gfxVsync = (val[0] == '1');
    }
    // Texture pack mod: saved as folder name; resolved to index after ScanMods.
    val[0] = '\0';
    if (JsonGetString(buf, "tex_pack_mod", val, sizeof(val))) {
        _snprintf(s_gfxTexPackSaved, sizeof(s_gfxTexPackSaved), "%s", val);
        s_gfxTexPackSaved[sizeof(s_gfxTexPackSaved)-1] = '\0';
    }
}

// Write graphics options into launcher_env.json (merges with engine-option keys).
static void SaveGfxJson() {
    if (!s_envJsonPath[0]) return;
    FILE* f = fopen(s_envJsonPath, "w");
    if (!f) return;
    fprintf(f, "{\n");
    bool first = true;
    // Engine options
    for (int i = 0; i < k_envVarCount; i++) {
        if (!s_envVarValues[i][0]) continue;
        if (!first) fprintf(f, ",\n");
        fprintf(f, "  \"%s\": \"%s\"", k_envVars[i].key, s_envVarValues[i]);
        first = false;
    }
    // Texture tier (always write so the engine always gets a value)
    if (!first) fprintf(f, ",\n");
    fprintf(f, "  \"MC2_STATICPROP_TEX_TIER\": \"%s\"", k_texTierValues[s_gfxTexQual]);
    first = false;
    // Shadow map size (always write)
    fprintf(f, ",\n  \"MC2_SHADOW_MAP_SIZE\": \"%s\"", k_shadSizeValues[s_gfxShadQual]);
    // Frame cap (only if enabled)
    if (s_gfxFrameCapOn) {
        char fpsBuf[32];
        _snprintf(fpsBuf, sizeof(fpsBuf), "%d", s_gfxFrameCapFps);
        fprintf(f, ",\n  \"MC2_FPS_CAP\": \"%s\"", fpsBuf);
    }
    // VSync (only if enabled)
    if (s_gfxVsync) {
        fprintf(f, ",\n  \"MC2_VSYNC\": \"1\"");
    }
    // Texture pack mod (always write; empty string = None)
    {
        const char* packFolder = "";
        if (s_gfxTexPackIdx > 0 && (s_gfxTexPackIdx - 1) < s_texQualCount)
            packFolder = s_texQual[s_gfxTexPackIdx - 1].folderName;
        fprintf(f, ",\n  \"tex_pack_mod\": \"%s\"", packFolder);
    }
    // Main-page selections (always write)
    fprintf(f, ",\n  \"last_campaign\": \"%s\"", s_savedCampaign);
    fprintf(f, ",\n  \"last_base\": \"%s\"", s_savedBase);
    fprintf(f, ",\n  \"last_addons\": \"%s\"", s_savedAddons);
    fprintf(f, ",\n  \"last_faster_weapons\": \"%s\"", s_savedFasterWeapons);
    fprintf(f, "\n}\n");
    fclose(f);
}

// Replace the simple SaveEnvJson (engine-only) with one that also writes graphics vars.
// The old SaveEnvJson is superseded — call SaveGfxJson() everywhere instead.
static void SaveEnvJson() { SaveGfxJson(); }

// Load main-page selections from launcher_env.json into s_saved* globals.
// Called at startup before RestoreMainPageSelections (which needs the UI to exist).
static void LoadMainPageJson() {
    if (!s_envJsonPath[0]) return;
    HANDLE fh = CreateFileA(s_envJsonPath, GENERIC_READ, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE) return;
    static char buf[32768];
    DWORD got = 0;
    ReadFile(fh, buf, sizeof(buf)-1, &got, NULL);
    CloseHandle(fh);
    buf[got] = '\0';

    char val[1024] = "";
    if (JsonGetString(buf, "last_campaign", val, sizeof(val)))
        _snprintf(s_savedCampaign, sizeof(s_savedCampaign), "%s", val);
    val[0] = '\0';
    if (JsonGetString(buf, "last_base", val, sizeof(val)))
        _snprintf(s_savedBase, sizeof(s_savedBase), "%s", val);
    val[0] = '\0';
    if (JsonGetString(buf, "last_addons", val, sizeof(val)))
        _snprintf(s_savedAddons, sizeof(s_savedAddons), "%s", val);
    val[0] = '\0';
    if (JsonGetString(buf, "last_faster_weapons", val, sizeof(val)))
        _snprintf(s_savedFasterWeapons, sizeof(s_savedFasterWeapons), "%s", val);
}

// Read the current main-page UI state into s_saved* globals, then persist.
// Call before PostQuitMessage (on Launch or Cancel/Close) so the file is up to date.
// Requires the main window controls to exist (s_hListBox etc. non-NULL).
static void SaveMainPageSelections() {
    // Campaign: listbox sel 0 = Stock (""), sel i -> s_campaigns[i-1].folderName
    {
        int sel = (int)SendMessageA(s_hListBox, LB_GETCURSEL, 0, 0);
        if (sel == LB_ERR) sel = 0;
        if (sel > 0 && sel <= s_campCount)
            _snprintf(s_savedCampaign, sizeof(s_savedCampaign), "%s", s_campaigns[sel-1].folderName);
        else
            s_savedCampaign[0] = '\0';
    }
    // Base radio: slot 0 = None (""), slot i -> s_bases[i-1].folderName
    {
        int slot = SelectedBaseSlot();
        if (slot > 0 && (slot - 1) < s_baseCount)
            _snprintf(s_savedBase, sizeof(s_savedBase), "%s", s_bases[slot-1].folderName);
        else
            s_savedBase[0] = '\0';
    }
    // Add-ons: comma-delimited checked folder names
    {
        s_savedAddons[0] = '\0';
        for (int i = 0; i < s_addonCount; i++) {
            if (s_hAddonList && ListView_GetCheckState(s_hAddonList, i)) {
                if (s_savedAddons[0])
                    strncat(s_savedAddons, ",", sizeof(s_savedAddons)-strlen(s_savedAddons)-1);
                strncat(s_savedAddons, s_addons[i].folderName,
                        sizeof(s_savedAddons)-strlen(s_savedAddons)-1);
            }
        }
    }
    // Faster weapons
    {
        BOOL fw = s_hFasterWeapons ?
            (SendMessageA(s_hFasterWeapons, BM_GETCHECK, 0, 0) == BST_CHECKED) : FALSE;
        _snprintf(s_savedFasterWeapons, sizeof(s_savedFasterWeapons), "%s", fw ? "1" : "0");
    }
    SaveGfxJson();
}

// Restore main-page UI from s_saved* globals.  Call after ScanMods + RefreshAllSlots
// so that the fixed slots are populated and relabelled before we try to match them.
// Missing/removed mods are silently skipped.
static void RestoreMainPageSelections() {
    // Campaign listbox: find the entry whose folder matches s_savedCampaign.
    if (s_savedCampaign[0]) {
        for (int i = 0; i < s_campCount; i++) {
            if (_stricmp(s_campaigns[i].folderName, s_savedCampaign) == 0) {
                SendMessageA(s_hListBox, LB_SETCURSEL, i + 1, 0);  // +1 for Stock row
                break;
            }
        }
        // If not found, leave Stock (slot 0) selected — already default.
    }
    // Base radio.
    if (s_savedBase[0]) {
        for (int i = 0; i < s_baseCount; i++) {
            if (_stricmp(s_bases[i].folderName, s_savedBase) == 0) {
                SelectBaseRadio(i + 1);  // slot i+1
                break;
            }
        }
        // If not found, None remains (already defaulted by RefreshBaseSlots).
    }
    // Add-on checkboxes: parse comma-delimited list.
    if (s_savedAddons[0]) {
        char addonsBuf[1024];
        _snprintf(addonsBuf, sizeof(addonsBuf), "%s", s_savedAddons);
        addonsBuf[sizeof(addonsBuf)-1] = '\0';
        char* ctx = NULL;
        char* tok = strtok_s(addonsBuf, ",", &ctx);
        while (tok) {
            // Trim leading/trailing spaces (defensive).
            while (*tok == ' ') tok++;
            char* e = tok + strlen(tok) - 1;
            while (e > tok && *e == ' ') *e-- = '\0';
            if (tok[0]) {
                for (int i = 0; i < s_addonCount; i++) {
                    if (s_hAddonList && _stricmp(s_addons[i].folderName, tok) == 0) {
                        ListView_SetCheckState(s_hAddonList, i, TRUE);
                        break;
                    }
                }
            }
            tok = strtok_s(NULL, ",", &ctx);
        }
    }
    // Faster weapons checkbox.
    if (s_hFasterWeapons && s_savedFasterWeapons[0]) {
        SendMessageA(s_hFasterWeapons, BM_SETCHECK,
                     (s_savedFasterWeapons[0] == '1') ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    // Sync status bar / auto-detect for the restored campaign selection.
    UpdateForSelection();
}

// ---- Import state ---------------------------------------------------------
static volatile bool s_importing = false;
static HWND          s_hProgWnd  = NULL;
static HANDLE        s_importThread = NULL;
static char          s_importFolder[MAX_PATH] = "";  // folder being imported
static char          s_importerPath[MAX_PATH] = "";  // resolved import_tool.py OR frozen .exe
static bool          s_importerFrozen = false;       // true => s_importerPath is a standalone .exe (no python)
static char          s_pyExe[64] = "";               // "py" or "python" (dev-mode fallback only)
static volatile bool s_importForce = false;          // re-run with --force
static volatile bool s_importCancelled = false;
static HANDLE        s_importChildProc = NULL;        // child handle for Cancel/TerminateProcess
static char          s_autoJson[65536] = "";          // captured auto stdout
static DWORD         s_autoExit = 0;

// ---- Minimal JSON field extractor ----------------------------------------

static bool JsonGetString(const char* json, const char* key, char* out, int outSz) {
    char needle[256];
    _snprintf(needle, sizeof(needle), "\"%s\"", key);
    needle[sizeof(needle)-1] = '\0';
    const char* p = strstr(json, needle);
    if (!p) return false;
    p += strlen(needle);
    while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '"') return false;
    p++;
    const char* end = strchr(p, '"');
    if (!end) return false;
    int len = (int)(end - p);
    if (len >= outSz) len = outSz - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return true;
}

// Parse a flat mod.json "env": { "KEY": "VALUE", ... } object into parallel
// key/value arrays. Only string values are supported (matches every other
// mod.json field); a malformed or absent "env" block yields count 0 — a mod
// with no env needs is unaffected. Bracket-scoped so keys/values elsewhere in
// the file (e.g. inside "description") can't be mistaken for env entries.
static int JsonGetEnvBlock(const char* json, char outKeys[][64], char outVals[][256], int maxEntries) {
    // Anchor on `"env"` followed (past whitespace) by `:` then `{` — a bare
    // strstr for `"env"` would also match that literal token inside e.g. a
    // "description" string that happens to mention it.
    const char* p = json;
    const char* brace = NULL;
    while ((p = strstr(p, "\"env\"")) != NULL) {
        const char* q = p + 5;
        while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
        if (*q == ':') {
            q++;
            while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
            if (*q == '{') { brace = q; break; }
        }
        p += 5;
    }
    if (!brace) return 0;
    const char* start = brace + 1;

    // Depth-aware scan for the matching close brace — a bare strchr for '}'
    // would stop early if a value string itself contains a literal '}'.
    // Quote-tracking so a `{`/`}` inside a string value doesn't affect depth.
    int depth = 1;
    bool inStr = false;
    const char* end = NULL;
    for (const char* s = start; *s; s++) {
        if (inStr) {
            if (*s == '\\' && s[1]) { s++; continue; }
            if (*s == '"') inStr = false;
            continue;
        }
        if (*s == '"') { inStr = true; continue; }
        if (*s == '{') depth++;
        else if (*s == '}') { depth--; if (depth == 0) { end = s; break; } }
    }
    if (!end) return 0;

    char block[2048];
    int blockLen = (int)(end - start);
    if (blockLen >= (int)sizeof(block)) blockLen = sizeof(block) - 1;
    memcpy(block, start, blockLen);
    block[blockLen] = '\0';

    int count = 0;
    const char* cursor = block;
    while (count < maxEntries) {
        const char* keyStart = strchr(cursor, '"');
        if (!keyStart) break;
        const char* keyEnd = strchr(keyStart + 1, '"');
        if (!keyEnd) break;
        const char* colon = strchr(keyEnd + 1, ':');
        if (!colon) break;
        const char* valStart = strchr(colon + 1, '"');
        if (!valStart) break;
        const char* valEnd = strchr(valStart + 1, '"');
        if (!valEnd) break;

        int keyLen = (int)(keyEnd - (keyStart + 1));
        if (keyLen == 0) { cursor = valEnd + 1; continue; }  // skip empty-key entries (malformed mod.json)
        if (keyLen >= 64) keyLen = 63;
        memcpy(outKeys[count], keyStart + 1, keyLen);
        outKeys[count][keyLen] = '\0';

        int valLen = (int)(valEnd - (valStart + 1));
        if (valLen >= 256) valLen = 255;
        memcpy(outVals[count], valStart + 1, valLen);
        outVals[count][valLen] = '\0';

        count++;
        cursor = valEnd + 1;
    }
    return count;
}

// Apply a ModEntry's mod.json-declared env vars to the child process
// environment (SetEnvironmentVariableA mutates the launcher's own env block,
// which CreateProcessA's NULL lpEnvironment then inherits — same mechanism as
// every other env var DoLaunch sets).
static void ApplyModEnv(const ModEntry& e) {
    for (int i = 0; i < e.envCount; i++)
        SetEnvironmentVariableA(e.envKeys[i], e.envVals[i]);
}

// Unset every mod.json-declared env key across ALL scanned mods (all 4
// buckets), regardless of whether that mod is currently checked/selected.
// Must run before ApplyModEnv for the active set each launch — otherwise a
// var set by a mod that was checked on a PREVIOUS launch (same launcher
// session) bleeds into a launch where that mod is no longer active, since
// SetEnvironmentVariableA persists in the launcher's own process.
static void ClearAllModEnv() {
    ModEntry* buckets[] = { s_campaigns, s_addons, s_bases, s_texQual };
    int counts[] = { s_campCount, s_addonCount, s_baseCount, s_texQualCount };
    for (int b = 0; b < 4; b++)
        for (int i = 0; i < counts[b]; i++)
            for (int j = 0; j < buckets[b][i].envCount; j++)
                SetEnvironmentVariableA(buckets[b][i].envKeys[j], NULL);
}

// ---- Env-var persistence: launcher_env.json ----------------------------------

// Initialize s_envVarValues from the defaultVal fields (before any load).
static void EnvVarsSetDefaults() {
    for (int i = 0; i < k_envVarCount; i++) {
        _snprintf(s_envVarValues[i], sizeof(s_envVarValues[i]), "%s",
                  k_envVars[i].defaultVal ? k_envVars[i].defaultVal : "");
        s_envVarValues[i][sizeof(s_envVarValues[i])-1] = '\0';
    }
}

// Load launcher_env.json into s_envVarValues.  Uses the same strstr extractor as
// JsonGetString.  Missing key -> leave the default value untouched.
static void LoadEnvJson() {
    if (!s_envJsonPath[0]) return;
    HANDLE fh = CreateFileA(s_envJsonPath, GENERIC_READ, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE) return;
    static char buf[32768];
    DWORD got = 0;
    ReadFile(fh, buf, sizeof(buf)-1, &got, NULL);
    CloseHandle(fh);
    buf[got] = '\0';

    for (int i = 0; i < k_envVarCount; i++) {
        char val[256] = "";
        if (JsonGetString(buf, k_envVars[i].key, val, sizeof(val))) {
            _snprintf(s_envVarValues[i], sizeof(s_envVarValues[i]), "%s", val);
            s_envVarValues[i][sizeof(s_envVarValues[i])-1] = '\0';
        }
    }
}

// ---- Options dialog (in-process, no .rc file) --------------------------------
//
// Implemented as a modal child popup using a registered window class.
// Controls:
//   - Section header STATICs (not interactive)
//   - Bool vars: BS_AUTOCHECKBOX
//   - Value vars: a STATIC label + an EDIT control
//   - [OK] and [Cancel] pushbuttons
//
// Control IDs: IDC_OPT_CTRL_BASE + i  for the active-control at index i.
// For bool vars: IDC_OPT_CTRL_BASE+i is the checkbox.
// For value vars: IDC_OPT_CTRL_BASE+i is the edit (label is decorative, no ID).

#define OPT_DLG_W       420
#define OPT_ROW_H       20
#define OPT_ROW_STEP    24
#define OPT_SECTION_H   18
#define OPT_SECTION_STEP 26
#define OPT_LBL_W       240
#define OPT_EDIT_W      120
#define OPT_MARGIN      12
#define OPT_BTN_H       26
#define OPT_BTN_W       80

// Forward declaration for use in dialog proc.
static void OptionsDialogReadControls(HWND hwnd);

static LRESULT CALLBACK OptionsDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        HFONT hFont      = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HFONT hFontBold  = (HFONT)GetStockObject(ANSI_FIXED_FONT);

        int y = OPT_MARGIN;
        const char* lastSection = NULL;

        for (int i = 0; i < k_envVarCount; i++) {
            // Section header when section changes.
            if (!lastSection || strcmp(k_envVars[i].section, lastSection) != 0) {
                lastSection = k_envVars[i].section;
                HWND hSec = CreateWindowA("STATIC", lastSection,
                    WS_CHILD | WS_VISIBLE | SS_LEFT,
                    OPT_MARGIN, y, OPT_DLG_W - 2*OPT_MARGIN, OPT_SECTION_H,
                    hwnd, (HMENU)(INT_PTR)-1, s_hInst, NULL);
                SendMessageA(hSec, WM_SETFONT, (WPARAM)hFontBold, TRUE);
                y += OPT_SECTION_STEP;
            }

            if (k_envVars[i].isBool) {
                // Checkbox: full-width
                HWND hCk = CreateWindowA("BUTTON", k_envVars[i].label,
                    WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                    OPT_MARGIN + 8, y, OPT_DLG_W - 2*OPT_MARGIN - 8, OPT_ROW_H,
                    hwnd, (HMENU)(INT_PTR)(IDC_OPT_CTRL_BASE + i), s_hInst, NULL);
                SendMessageA(hCk, WM_SETFONT, (WPARAM)hFont, TRUE);
                // Pre-check if saved value == "1"
                SendMessageA(hCk, BM_SETCHECK,
                    (strcmp(s_envVarValues[i], "1") == 0) ? BST_CHECKED : BST_UNCHECKED, 0);
            } else {
                // Label + edit
                HWND hLbl = CreateWindowA("STATIC", k_envVars[i].label,
                    WS_CHILD | WS_VISIBLE | SS_LEFT,
                    OPT_MARGIN + 8, y + 2, OPT_LBL_W, OPT_ROW_H,
                    hwnd, (HMENU)(INT_PTR)-1, s_hInst, NULL);
                SendMessageA(hLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
                HWND hEd = CreateWindowA("EDIT", s_envVarValues[i],
                    WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                    OPT_MARGIN + 8 + OPT_LBL_W + 4, y, OPT_EDIT_W, OPT_ROW_H,
                    hwnd, (HMENU)(INT_PTR)(IDC_OPT_CTRL_BASE + i), s_hInst, NULL);
                SendMessageA(hEd, WM_SETFONT, (WPARAM)hFont, TRUE);
            }
            y += OPT_ROW_STEP;
        }

        // Store final content height so we know where to place buttons.
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)y);

        y += OPT_MARGIN;
        // [OK] and [Cancel] buttons.
        HWND hOK = CreateWindowA("BUTTON", "OK",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            OPT_DLG_W/2 - OPT_BTN_W - 4, y, OPT_BTN_W, OPT_BTN_H,
            hwnd, (HMENU)IDC_OPT_OK, s_hInst, NULL);
        SendMessageA(hOK, WM_SETFONT, (WPARAM)hFont, TRUE);
        HWND hCl = CreateWindowA("BUTTON", "Cancel",
            WS_CHILD | WS_VISIBLE,
            OPT_DLG_W/2 + 4, y, OPT_BTN_W, OPT_BTN_H,
            hwnd, (HMENU)IDC_OPT_CANCEL, s_hInst, NULL);
        SendMessageA(hCl, WM_SETFONT, (WPARAM)hFont, TRUE);
        break;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_OPT_OK) {
            OptionsDialogReadControls(hwnd);
            SaveEnvJson();
            DestroyWindow(hwnd);
            s_hOptionsDlg = NULL;
        } else if (LOWORD(wParam) == IDC_OPT_CANCEL) {
            DestroyWindow(hwnd);
            s_hOptionsDlg = NULL;
        }
        break;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        s_hOptionsDlg = NULL;
        break;

    case WM_KEYDOWN:
        if ((int)wParam == VK_ESCAPE) {
            DestroyWindow(hwnd);
            s_hOptionsDlg = NULL;
        }
        break;

    default:
        return DefWindowProcA(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// Read all controls back into s_envVarValues.
static void OptionsDialogReadControls(HWND hwnd) {
    for (int i = 0; i < k_envVarCount; i++) {
        HWND hCtrl = GetDlgItem(hwnd, IDC_OPT_CTRL_BASE + i);
        if (!hCtrl) continue;
        if (k_envVars[i].isBool) {
            bool checked = (SendMessageA(hCtrl, BM_GETCHECK, 0, 0) == BST_CHECKED);
            _snprintf(s_envVarValues[i], sizeof(s_envVarValues[i]), "%s", checked ? "1" : "");
        } else {
            GetWindowTextA(hCtrl, s_envVarValues[i], (int)sizeof(s_envVarValues[i]));
        }
    }
}

static void RegisterOptionsDlgClass() {
    static bool done = false;
    if (done) return;
    WNDCLASSA wc = {};
    wc.lpfnWndProc   = OptionsDlgProc;
    wc.hInstance     = s_hInst;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor       = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.lpszClassName = "MC2OptionsDlg";
    RegisterClassA(&wc);
    done = true;
}

static void ShowOptionsDialog(HWND owner) {
    if (s_hOptionsDlg) {
        SetForegroundWindow(s_hOptionsDlg);
        return;
    }
    RegisterOptionsDlgClass();

    // Compute dialog height: count section changes + rows.
    int contentH = OPT_MARGIN;
    const char* lastSec = NULL;
    for (int i = 0; i < k_envVarCount; i++) {
        if (!lastSec || strcmp(k_envVars[i].section, lastSec) != 0) {
            lastSec = k_envVars[i].section;
            contentH += OPT_SECTION_STEP;
        }
        contentH += OPT_ROW_STEP;
    }
    contentH += OPT_MARGIN + OPT_BTN_H + OPT_MARGIN;

    int clientW = OPT_DLG_W;
    int clientH = contentH;
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    RECT rc = { 0, 0, clientW, clientH };
    AdjustWindowRect(&rc, style, FALSE);
    int ww = rc.right - rc.left;
    int wh = rc.bottom - rc.top;

    RECT orc; GetWindowRect(owner, &orc);
    int ox = orc.left + ((orc.right  - orc.left) - ww) / 2;
    int oy = orc.top  + ((orc.bottom - orc.top)  - wh) / 2;

    s_hOptionsDlg = CreateWindowA("MC2OptionsDlg", "Engine Options",
        style, ox, oy, ww, wh, owner, NULL, s_hInst, NULL);
    ShowWindow(s_hOptionsDlg, SW_SHOW);
    UpdateWindow(s_hOptionsDlg);
}

// ---- Graphics Options dialog --------------------------------------------------
//
// Layout (fixed width 440px):
//   Section: Texture Quality  — 4 radios
//   Section: Shadow Quality   — 3 radios
//   Section: VRAM Estimate    — multi-line STATIC (monospace, 8 lines)
//   Section: Frame Cap        — checkbox + edit (both active: MC2_FPS_CAP wired)
//   Section: VSync            — checkbox (active: MC2_VSYNC wired)
//   [OK] [Cancel]

#define GFX_DLG_W       440
#define GFX_ROW_H       20
#define GFX_ROW_STEP    24
#define GFX_SEC_H       18
#define GFX_SEC_STEP    26
#define GFX_MARGIN      12
#define GFX_BTN_H       26
#define GFX_BTN_W       80
#define GFX_VRAM_H      130   // tall enough for ~8 lines of monospace text

// Build the VRAM estimate string into out[]. outSz should be >= 512.
// Reads s_gfxTexQual, s_gfxShadQual; reads MC2_HDRI_BC6H from s_envVarValues.
// MC2_HDRI_BC6H default is ON (BC6H 4K = 8 MB); set =0 for RGBA16F (134 MB).
static void RecalcVram(char* out, int outSz) {
    int texMB  = k_texVramMB[s_gfxTexQual];
    int shadMB = k_shadVramMB[s_gfxShadQual];
    int fboMB  = 96;    // GL_RGBA16F 1080p x3 attachments + bloom

    // MC2_HDRI_BC6H: default ON (absent = BC6H 4K = 8 MB).
    // Only use RGBA16F (134 MB) when key is present in k_envVars and set to "0".
    bool bc6hOff = false;
    for (int i = 0; i < k_envVarCount; i++) {
        if (strcmp(k_envVars[i].key, "MC2_HDRI_BC6H") == 0) {
            bc6hOff = (s_envVarValues[i][0] == '0');
            break;
        }
    }
    const int  hdriMB      = bc6hOff ? 134 : 8;
    const char* hdriLabel  = bc6hOff
        ? "HDRI (RGBA16F 4K): %d MB"
        : "HDRI (BC6H 4K):    %d MB";

    int baseMB  = 256;
    int totalMB = texMB + shadMB + fboMB + hdriMB + baseMB;

    char hdriLine[64];
    _snprintf(hdriLine, sizeof(hdriLine), hdriLabel, hdriMB);
    hdriLine[sizeof(hdriLine)-1] = '\0';

    _snprintf(out, outSz,
        "Estimated VRAM: ~%d MB\r\n"
        "  Textures:      %d MB\r\n"
        "  Shadow maps:   %d MB\r\n"
        "  Scene FBOs:     %d MB\r\n"
        "  %s\r\n"
        "  (16K BC6H ~128 MB, 16K RGBA16F ~1 GB)\r\n"
        "  Base overhead: %d MB\r\n"
        "  ---------------------\r\n"
        "  (excl. terrain, water, mission geometry)",
        totalMB, texMB, shadMB, fboMB, hdriLine, baseMB);
    out[outSz-1] = '\0';
}

// Read graphics dialog controls back into s_gfx* globals.
static void GfxDialogReadControls(HWND hwnd) {
    // Texture pack radios (slot 0 = None; slot i -> s_texQual[i-1])
    {
        int nSlots = s_texQualCount + 1;
        if (nSlots > TEXPACK_DLG_SLOTS) nSlots = TEXPACK_DLG_SLOTS;
        for (int i = 0; i < nSlots; i++) {
            HWND h = GetDlgItem(hwnd, IDC_GFX_TEXPACK_BASE + i);
            if (h && SendMessageA(h, BM_GETCHECK, 0, 0) == BST_CHECKED) {
                s_gfxTexPackIdx = i; break;
            }
        }
    }
    // Texture quality radios
    for (int i = 0; i < 4; i++) {
        HWND h = GetDlgItem(hwnd, IDC_GFX_TEXQUAL_BASE + i);
        if (h && SendMessageA(h, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            s_gfxTexQual = i; break;
        }
    }
    // Shadow quality radios
    for (int i = 0; i < 3; i++) {
        HWND h = GetDlgItem(hwnd, IDC_GFX_SHADQUAL_BASE + i);
        if (h && SendMessageA(h, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            s_gfxShadQual = i; break;
        }
    }
    // Frame cap
    HWND hCk = GetDlgItem(hwnd, IDC_GFX_FRAMECAP_CHK);
    HWND hEd = GetDlgItem(hwnd, IDC_GFX_FRAMECAP_EDIT);
    if (hCk) s_gfxFrameCapOn = (SendMessageA(hCk, BM_GETCHECK, 0, 0) == BST_CHECKED);
    if (hEd) {
        char buf[32]; GetWindowTextA(hEd, buf, sizeof(buf));
        int v = atoi(buf);
        s_gfxFrameCapFps = (v > 0) ? v : 60;
    }
    // VSync
    HWND hVs = GetDlgItem(hwnd, IDC_GFX_VSYNC_CHK);
    if (hVs) s_gfxVsync = (SendMessageA(hVs, BM_GETCHECK, 0, 0) == BST_CHECKED);
}

static LRESULT CALLBACK GraphicsDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        HFONT hFont     = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HFONT hFontBold = (HFONT)GetStockObject(ANSI_FIXED_FONT);
        HFONT hMono     = (HFONT)GetStockObject(SYSTEM_FIXED_FONT);

        int y = GFX_MARGIN;

        // --- Presets ---
        {
            HWND hPrLbl = CreateWindowA("STATIC", "Presets",
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                GFX_MARGIN, y, GFX_DLG_W - 2*GFX_MARGIN, GFX_SEC_H,
                hwnd, (HMENU)(INT_PTR)-1, s_hInst, NULL);
            SendMessageA(hPrLbl, WM_SETFONT, (WPARAM)hFontBold, TRUE);
            y += GFX_SEC_STEP;

            int nPr = 4;
            int prW = (GFX_DLG_W - 2*GFX_MARGIN - (nPr-1)*8) / nPr;
            for (int i = 0; i < nPr; i++) {
                HWND hPr = CreateWindowA("BUTTON", k_gfxPresetLabels[i],
                    WS_CHILD | WS_VISIBLE,
                    GFX_MARGIN + i * (prW + 8), y, prW, GFX_BTN_H,
                    hwnd, (HMENU)(INT_PTR)(IDC_GFX_PRESET_BASE + i), s_hInst, NULL);
                SendMessageA(hPr, WM_SETFONT, (WPARAM)hFont, TRUE);
            }
            y += GFX_BTN_H + 8;
        }

        // --- Texture Pack Mods (installed art replacements — mutually exclusive) ---
        {
            HWND hTP = CreateWindowA("STATIC", "Texture Pack Mods  (installed art replacements)",
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                GFX_MARGIN, y, GFX_DLG_W - 2*GFX_MARGIN, GFX_SEC_H,
                hwnd, (HMENU)(INT_PTR)-1, s_hInst, NULL);
            SendMessageA(hTP, WM_SETFONT, (WPARAM)hFontBold, TRUE);
            y += GFX_SEC_STEP;

            int nSlots = s_texQualCount + 1;  // +1 for "None"
            if (nSlots > TEXPACK_DLG_SLOTS) nSlots = TEXPACK_DLG_SLOTS;
            for (int i = 0; i < nSlots; i++) {
                DWORD st = WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON;
                if (i == 0) st |= WS_GROUP;
                const char* lbl = "None";
                char nameBuf[300];
                if (i > 0) {
                    _snprintf(nameBuf, sizeof(nameBuf), "%s", s_texQual[i-1].name);
                    nameBuf[sizeof(nameBuf)-1] = '\0';
                    lbl = nameBuf;
                }
                HWND hR = CreateWindowA("BUTTON", lbl, st,
                    GFX_MARGIN + 8, y, GFX_DLG_W - 2*GFX_MARGIN - 8, GFX_ROW_H,
                    hwnd, (HMENU)(INT_PTR)(IDC_GFX_TEXPACK_BASE + i), s_hInst, NULL);
                SendMessageA(hR, WM_SETFONT, (WPARAM)hFont, TRUE);
                SendMessageA(hR, BM_SETCHECK,
                    (i == s_gfxTexPackIdx) ? BST_CHECKED : BST_UNCHECKED, 0);
                y += GFX_ROW_STEP;
            }
            // If no packs found, show a placeholder label.
            if (s_texQualCount == 0) {
                HWND hNone = CreateWindowA("STATIC", "(No texture packs installed)",
                    WS_CHILD | WS_VISIBLE | SS_LEFT,
                    GFX_MARGIN + 8, y, GFX_DLG_W - 2*GFX_MARGIN - 8, GFX_ROW_H,
                    hwnd, (HMENU)(INT_PTR)-1, s_hInst, NULL);
                SendMessageA(hNone, WM_SETFONT, (WPARAM)hFont, TRUE);
                y += GFX_ROW_STEP;
            }
        }
        y += 6;

        // --- Texture Resolution (engine-side atlas detail — not a mod) ---
        HWND hS = CreateWindowA("STATIC", "Texture Resolution  (engine atlas detail level)",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            GFX_MARGIN, y, GFX_DLG_W - 2*GFX_MARGIN, GFX_SEC_H,
            hwnd, (HMENU)(INT_PTR)-1, s_hInst, NULL);
        SendMessageA(hS, WM_SETFONT, (WPARAM)hFontBold, TRUE);
        y += GFX_SEC_STEP;

        for (int i = 0; i < 4; i++) {
            DWORD st = WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON;
            if (i == 0) st |= WS_GROUP;
            HWND hR = CreateWindowA("BUTTON", k_texTierLabels[i], st,
                GFX_MARGIN + 8, y, GFX_DLG_W - 2*GFX_MARGIN - 8, GFX_ROW_H,
                hwnd, (HMENU)(INT_PTR)(IDC_GFX_TEXQUAL_BASE + i), s_hInst, NULL);
            SendMessageA(hR, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessageA(hR, BM_SETCHECK,
                (i == s_gfxTexQual) ? BST_CHECKED : BST_UNCHECKED, 0);
            y += GFX_ROW_STEP;
        }
        y += 6;

        // --- Shadow Quality ---
        HWND hSh = CreateWindowA("STATIC", "Shadow Quality",
            WS_CHILD | WS_VISIBLE | SS_LEFT | WS_GROUP,
            GFX_MARGIN, y, GFX_DLG_W - 2*GFX_MARGIN, GFX_SEC_H,
            hwnd, (HMENU)(INT_PTR)-1, s_hInst, NULL);
        SendMessageA(hSh, WM_SETFONT, (WPARAM)hFontBold, TRUE);
        y += GFX_SEC_STEP;

        for (int i = 0; i < 3; i++) {
            DWORD st = WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON;
            if (i == 0) st |= WS_GROUP;
            HWND hR = CreateWindowA("BUTTON", k_shadQualLabels[i], st,
                GFX_MARGIN + 8, y, GFX_DLG_W - 2*GFX_MARGIN - 8, GFX_ROW_H,
                hwnd, (HMENU)(INT_PTR)(IDC_GFX_SHADQUAL_BASE + i), s_hInst, NULL);
            SendMessageA(hR, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessageA(hR, BM_SETCHECK,
                (i == s_gfxShadQual) ? BST_CHECKED : BST_UNCHECKED, 0);
            y += GFX_ROW_STEP;
        }
        y += 6;

        // --- VRAM Estimator ---
        HWND hVramLbl = CreateWindowA("STATIC", "VRAM Estimate",
            WS_CHILD | WS_VISIBLE | SS_LEFT | WS_GROUP,
            GFX_MARGIN, y, GFX_DLG_W - 2*GFX_MARGIN, GFX_SEC_H,
            hwnd, (HMENU)(INT_PTR)-1, s_hInst, NULL);
        SendMessageA(hVramLbl, WM_SETFONT, (WPARAM)hFontBold, TRUE);
        y += GFX_SEC_STEP;

        char vramBuf[512];
        RecalcVram(vramBuf, sizeof(vramBuf));
        HWND hVramText = CreateWindowA("STATIC", vramBuf,
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            GFX_MARGIN + 4, y, GFX_DLG_W - 2*GFX_MARGIN - 4, GFX_VRAM_H,
            hwnd, (HMENU)(INT_PTR)IDC_GFX_VRAM_TEXT, s_hInst, NULL);
        SendMessageA(hVramText, WM_SETFONT, (WPARAM)hMono, TRUE);
        y += GFX_VRAM_H + 6;

        // --- Frame Cap (MC2_FPS_CAP is wired in gameosmain.cpp) ---
        HWND hFcLbl = CreateWindowA("STATIC", "Frame Rate",
            WS_CHILD | WS_VISIBLE | SS_LEFT | WS_GROUP,
            GFX_MARGIN, y, GFX_DLG_W - 2*GFX_MARGIN, GFX_SEC_H,
            hwnd, (HMENU)(INT_PTR)-1, s_hInst, NULL);
        SendMessageA(hFcLbl, WM_SETFONT, (WPARAM)hFontBold, TRUE);
        y += GFX_SEC_STEP;

        HWND hFcCk = CreateWindowA("BUTTON", "Limit frame rate",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            GFX_MARGIN + 8, y, 140, GFX_ROW_H,
            hwnd, (HMENU)(INT_PTR)IDC_GFX_FRAMECAP_CHK, s_hInst, NULL);
        SendMessageA(hFcCk, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageA(hFcCk, BM_SETCHECK, s_gfxFrameCapOn ? BST_CHECKED : BST_UNCHECKED, 0);

        char fpsBuf[16]; _snprintf(fpsBuf, sizeof(fpsBuf), "%d", s_gfxFrameCapFps);
        HWND hFcEd = CreateWindowA("EDIT", fpsBuf,
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL,
            GFX_MARGIN + 8 + 146, y, 52, GFX_ROW_H,
            hwnd, (HMENU)(INT_PTR)IDC_GFX_FRAMECAP_EDIT, s_hInst, NULL);
        SendMessageA(hFcEd, WM_SETFONT, (WPARAM)hFont, TRUE);

        HWND hFcUnit = CreateWindowA("STATIC", "FPS",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            GFX_MARGIN + 8 + 146 + 56, y + 2, 40, GFX_ROW_H,
            hwnd, (HMENU)(INT_PTR)-1, s_hInst, NULL);
        SendMessageA(hFcUnit, WM_SETFONT, (WPARAM)hFont, TRUE);
        y += GFX_ROW_STEP + 4;

        // --- VSync (MC2_VSYNC is wired in gos_render.cpp) ---
        HWND hVsLbl = CreateWindowA("STATIC", "VSync",
            WS_CHILD | WS_VISIBLE | SS_LEFT | WS_GROUP,
            GFX_MARGIN, y, GFX_DLG_W - 2*GFX_MARGIN, GFX_SEC_H,
            hwnd, (HMENU)(INT_PTR)-1, s_hInst, NULL);
        SendMessageA(hVsLbl, WM_SETFONT, (WPARAM)hFontBold, TRUE);
        y += GFX_SEC_STEP;

        HWND hVsCk = CreateWindowA("BUTTON", "Enable VSync",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            GFX_MARGIN + 8, y, GFX_DLG_W - 2*GFX_MARGIN - 8, GFX_ROW_H,
            hwnd, (HMENU)(INT_PTR)IDC_GFX_VSYNC_CHK, s_hInst, NULL);
        SendMessageA(hVsCk, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageA(hVsCk, BM_SETCHECK, s_gfxVsync ? BST_CHECKED : BST_UNCHECKED, 0);
        y += GFX_ROW_STEP;

        SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)y);

        y += GFX_MARGIN;
        HWND hOK = CreateWindowA("BUTTON", "OK",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            GFX_DLG_W/2 - GFX_BTN_W - 4, y, GFX_BTN_W, GFX_BTN_H,
            hwnd, (HMENU)(INT_PTR)IDC_GFX_OK, s_hInst, NULL);
        SendMessageA(hOK, WM_SETFONT, (WPARAM)hFont, TRUE);
        HWND hCl = CreateWindowA("BUTTON", "Cancel",
            WS_CHILD | WS_VISIBLE,
            GFX_DLG_W/2 + 4, y, GFX_BTN_W, GFX_BTN_H,
            hwnd, (HMENU)(INT_PTR)IDC_GFX_CANCEL, s_hInst, NULL);
        SendMessageA(hCl, WM_SETFONT, (WPARAM)hFont, TRUE);
        break;
    }

    case WM_COMMAND: {
        WORD id = LOWORD(wParam);
        WORD notif = HIWORD(wParam);

        // Recalc VRAM whenever a quality radio changes.
        bool qualChanged = false;
        if (notif == BN_CLICKED) {
            if (id >= IDC_GFX_TEXQUAL_BASE && id < IDC_GFX_TEXQUAL_BASE + 4) qualChanged = true;
            if (id >= IDC_GFX_SHADQUAL_BASE && id < IDC_GFX_SHADQUAL_BASE + 3) qualChanged = true;
        }
        if (qualChanged) {
            // Read current radio selections for recalc (don't commit to globals yet)
            int tq = s_gfxTexQual, sq = s_gfxShadQual;
            for (int i = 0; i < 4; i++) {
                HWND h = GetDlgItem(hwnd, IDC_GFX_TEXQUAL_BASE + i);
                if (h && SendMessageA(h, BM_GETCHECK, 0, 0) == BST_CHECKED) { tq = i; break; }
            }
            for (int i = 0; i < 3; i++) {
                HWND h = GetDlgItem(hwnd, IDC_GFX_SHADQUAL_BASE + i);
                if (h && SendMessageA(h, BM_GETCHECK, 0, 0) == BST_CHECKED) { sq = i; break; }
            }
            int savedTq = s_gfxTexQual, savedSq = s_gfxShadQual;
            s_gfxTexQual = tq; s_gfxShadQual = sq;
            char vramBuf[512]; RecalcVram(vramBuf, sizeof(vramBuf));
            HWND hVt = GetDlgItem(hwnd, IDC_GFX_VRAM_TEXT);
            if (hVt) SetWindowTextA(hVt, vramBuf);
            s_gfxTexQual = savedTq; s_gfxShadQual = savedSq;
        }

        // Preset button clicked: apply preset values to all controls.
        int pidx = (int)id - IDC_GFX_PRESET_BASE;
        if (pidx >= 0 && pidx < 4 && notif == BN_CLICKED) {
            const GfxPreset& p = k_gfxPresets[pidx];
            for (int i = 0; i < 4; i++) {
                HWND h = GetDlgItem(hwnd, IDC_GFX_TEXQUAL_BASE + i);
                if (h) SendMessageA(h, BM_SETCHECK, (i == p.texQual) ? BST_CHECKED : BST_UNCHECKED, 0);
            }
            for (int i = 0; i < 3; i++) {
                HWND h = GetDlgItem(hwnd, IDC_GFX_SHADQUAL_BASE + i);
                if (h) SendMessageA(h, BM_SETCHECK, (i == p.shadQual) ? BST_CHECKED : BST_UNCHECKED, 0);
            }
            HWND hCk = GetDlgItem(hwnd, IDC_GFX_FRAMECAP_CHK);
            HWND hEd = GetDlgItem(hwnd, IDC_GFX_FRAMECAP_EDIT);
            if (hCk) SendMessageA(hCk, BM_SETCHECK, p.frameCap ? BST_CHECKED : BST_UNCHECKED, 0);
            if (hEd) {
                char fpsBuf[16]; _snprintf(fpsBuf, sizeof(fpsBuf), "%d", p.fps);
                SetWindowTextA(hEd, fpsBuf);
            }
            // Update VRAM display for the preset.
            int savedTq = s_gfxTexQual, savedSq = s_gfxShadQual;
            s_gfxTexQual = p.texQual; s_gfxShadQual = p.shadQual;
            char vramBuf[512]; RecalcVram(vramBuf, sizeof(vramBuf));
            HWND hVt = GetDlgItem(hwnd, IDC_GFX_VRAM_TEXT);
            if (hVt) SetWindowTextA(hVt, vramBuf);
            s_gfxTexQual = savedTq; s_gfxShadQual = savedSq;
        }

        if (id == IDC_GFX_OK) {
            GfxDialogReadControls(hwnd);
            SaveGfxJson();
            DestroyWindow(hwnd);
            s_hGraphicsDlg = NULL;
        } else if (id == IDC_GFX_CANCEL) {
            DestroyWindow(hwnd);
            s_hGraphicsDlg = NULL;
        }
        break;
    }

    case WM_CLOSE:
        DestroyWindow(hwnd);
        s_hGraphicsDlg = NULL;
        break;

    case WM_KEYDOWN:
        if ((int)wParam == VK_ESCAPE) {
            DestroyWindow(hwnd);
            s_hGraphicsDlg = NULL;
        }
        break;

    default:
        return DefWindowProcA(hwnd, msg, wParam, lParam);
    }
    return 0;
}

static void RegisterGraphicsDlgClass() {
    static bool done = false;
    if (done) return;
    WNDCLASSA wc = {};
    wc.lpfnWndProc   = GraphicsDlgProc;
    wc.hInstance     = s_hInst;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor       = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.lpszClassName = "MC2GraphicsDlg";
    RegisterClassA(&wc);
    done = true;
}

static void ShowGraphicsDialog(HWND owner) {
    if (s_hGraphicsDlg) {
        SetForegroundWindow(s_hGraphicsDlg);
        return;
    }
    RegisterGraphicsDlgClass();

    // Compute dialog height: sections + rows + VRAM box + buttons.
    int contentH = GFX_MARGIN;
    // Presets: 1 header + 1 button row
    contentH += GFX_SEC_STEP + GFX_BTN_H + 8;
    // Texture Packs: 1 header + (nSlots) rows  (at least 1 "None" row or placeholder)
    {
        int nPackSlots = s_texQualCount + 1;
        if (nPackSlots > TEXPACK_DLG_SLOTS) nPackSlots = TEXPACK_DLG_SLOTS;
        if (s_texQualCount == 0) nPackSlots = 1;  // placeholder row
        contentH += GFX_SEC_STEP + nPackSlots * GFX_ROW_STEP + 6;
    }
    // Texture Quality
    contentH += GFX_SEC_STEP + 4 * GFX_ROW_STEP + 6;
    // Shadow Quality
    contentH += GFX_SEC_STEP + 3 * GFX_ROW_STEP + 6;
    // VRAM Estimator
    contentH += GFX_SEC_STEP + GFX_VRAM_H + 6;
    // Frame Rate
    contentH += GFX_SEC_STEP + GFX_ROW_STEP + 4;
    // VSync
    contentH += GFX_SEC_STEP + GFX_ROW_STEP;
    contentH += GFX_MARGIN + GFX_BTN_H + GFX_MARGIN;

    int clientW = GFX_DLG_W;
    int clientH = contentH;
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    RECT rc = { 0, 0, clientW, clientH };
    AdjustWindowRect(&rc, style, FALSE);
    int ww = rc.right - rc.left;
    int wh = rc.bottom - rc.top;

    RECT orc; GetWindowRect(owner, &orc);
    int ox = orc.left + ((orc.right  - orc.left) - ww) / 2;
    int oy = orc.top  + ((orc.bottom - orc.top)  - wh) / 2;

    s_hGraphicsDlg = CreateWindowA("MC2GraphicsDlg", "Graphics Options",
        style, ox, oy, ww, wh, owner, NULL, s_hInst, NULL);
    ShowWindow(s_hGraphicsDlg, SW_SHOW);
    UpdateWindow(s_hGraphicsDlg);
}

static bool DirExists(const char* path) {
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool FileExists(const char* path) {
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Case-insensitive substring search.
static bool ContainsCI(const char* hay, const char* needle) {
    size_t nl = strlen(needle);
    for (const char* p = hay; *p; p++)
        if (_strnicmp(p, needle, nl) == 0) return true;
    return false;
}

// ---- Campaign compat auto-detection --------------------------------------
//
// Scan a directory's files: .fit -> max ObjectNumber; .abl -> magicAttack.
// Reads into a fixed buffer (mission/brain files are small text). Sets the two
// flags; short-circuits each once its signal is found.

static void ScanDirSignals(const char* dir, bool* hasHighObj, bool* hasMagic) {
    char pattern[MAX_PATH];
    _snprintf(pattern, sizeof(pattern), "%s\\*", dir);
    pattern[sizeof(pattern)-1] = '\0';
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const char* ext = strrchr(fd.cFileName, '.');
        if (!ext) continue;
        bool isFit = _stricmp(ext, ".fit") == 0;
        bool isAbl = _stricmp(ext, ".abl") == 0 || _stricmp(ext, ".abi") == 0;
        if (!isFit && !isAbl) continue;
        if (isFit && *hasHighObj) continue;
        if (isAbl && *hasMagic) continue;

        char path[MAX_PATH];
        _snprintf(path, sizeof(path), "%s\\%s", dir, fd.cFileName);
        path[sizeof(path)-1] = '\0';
        HANDLE fh = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (fh == INVALID_HANDLE_VALUE) continue;
        // Read up to 256 KB — enough for any mission .fit / brain.
        static char buf[262144];
        DWORD got = 0;
        ReadFile(fh, buf, sizeof(buf)-1, &got, NULL);
        CloseHandle(fh);
        buf[got] = '\0';

        if (isAbl) {
            if (ContainsCI(buf, "magicattack")) *hasMagic = true;
            continue;
        }
        // .fit: find every "ObjectNumber" and parse the int after '='.
        for (const char* p = buf; (p = strstr(p, "ObjectNumber")) != NULL; ) {
            p += 12;
            while (*p == ' ' || *p == '\t' || *p == '=') p++;
            int v = atoi(p);
            if (v > 1188) { *hasHighObj = true; break; }
        }
    } while (FindNextFileA(h, &fd) && !(*hasHighObj && *hasMagic));
    FindClose(h);
}

static CompatKind DetectCompat(const char* modFolderAbs) {
    char missions[MAX_PATH], warriors[MAX_PATH];
    _snprintf(missions, sizeof(missions), "%s\\data\\missions", modFolderAbs);
    _snprintf(warriors, sizeof(warriors), "%s\\data\\missions\\warriors", modFolderAbs);
    bool hasHighObj = false, hasMagic = false;
    ScanDirSignals(missions, &hasHighObj, &hasMagic);
    if (!(hasHighObj && hasMagic)) ScanDirSignals(warriors, &hasHighObj, &hasMagic);
    // magicAttack is an Omnitech-only brain routine -> definitive MCO signal, even
    // when the pack stays in stock objtype range (e.g. MC2-Exodus: objNum 878 but
    // magicAttack everywhere). A high ObjectNumber without it means an MC2X "fat"
    // pack (e.g. cveg, 1443). Otherwise stock-range -> can't tell from pure stock.
    if (hasMagic)   return CK_MCO;
    if (hasHighObj) return CK_MC2X;
    return CK_UNKNOWN;
}

// ---- Mod discovery --------------------------------------------------------

// The three ABL library files a compat layer ships under data/missions. They are
// campaign-brain plumbing (magicAttack etc.), NOT playable missions. A folder
// that contains ONLY these (e.g. mco-compat) must stay an add-on, never a
// campaign — so the "has playable missions" test below skips them explicitly.
static bool IsAblLibName(const char* fileName) {
    return _stricmp(fileName, "corebrain.abx") == 0 ||
           _stricmp(fileName, "orders.abx")    == 0 ||
           _stricmp(fileName, "miscfunc.abx")  == 0;
}

// True if <missDir> holds at least one real mission file (.fit) that is not one
// of the 3 ABL libs. (.abx libs are excluded outright; .fit is the mission key.)
static bool HasPlayableMissions(const char* missDir) {
    char pattern[MAX_PATH];
    _snprintf(pattern, sizeof(pattern), "%s\\*", missDir);
    pattern[sizeof(pattern)-1] = '\0';
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (IsAblLibName(fd.cFileName)) continue;   // skip corebrain/orders/miscfunc
        const char* ext = strrchr(fd.cFileName, '.');
        if (!ext) continue;
        if (_stricmp(ext, ".fit") == 0) { found = true; break; }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return found;
}

static void ScanMods(const char* modsPath) {
    char pattern[MAX_PATH];
    _snprintf(pattern, sizeof(pattern), "%s*", modsPath);
    pattern[sizeof(pattern)-1] = '\0';

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == '.') continue;

        char modDir[MAX_PATH], dataDir[MAX_PATH], missDir[MAX_PATH], jsonPath[MAX_PATH];
        _snprintf(modDir,  sizeof(modDir),  "%s%s", modsPath, fd.cFileName);
        _snprintf(dataDir, sizeof(dataDir), "%s\\data", modDir);
        _snprintf(missDir, sizeof(missDir), "%s\\data\\missions", modDir);
        _snprintf(jsonPath,sizeof(jsonPath),"%s\\mod.json", modDir);

        // A mod is any folder with a data/ subdir — no mod.json required.
        if (!DirExists(dataDir)) continue;

        // Optional mod.json for nicer names / explicit type / radioGroup.
        char name[256] = "", type[32] = "", radioGroup[64] = "", desc[1024] = "";
        char envKeys[8][64] = {}; char envVals[8][256] = {}; int envCount = 0;
        HANDLE fh = CreateFileA(jsonPath, GENERIC_READ, FILE_SHARE_READ, NULL,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (fh != INVALID_HANDLE_VALUE) {
            char buf[4096] = {}; DWORD got = 0;
            ReadFile(fh, buf, sizeof(buf)-1, &got, NULL); CloseHandle(fh); buf[got] = '\0';
            JsonGetString(buf, "name", name, sizeof(name));
            JsonGetString(buf, "type", type, sizeof(type));
            JsonGetString(buf, "radioGroup", radioGroup, sizeof(radioGroup));
            JsonGetString(buf, "description", desc, sizeof(desc));
            envCount = JsonGetEnvBlock(buf, envKeys, envVals, 8);
        }
        if (!name[0]) _snprintf(name, sizeof(name), "%s", fd.cFileName);

        // --- Bucketing ---
        // 1. Explicit mod.json "type" wins over content-detection:
        //      "campaign"               -> Campaign bucket
        //      "dependency" / "assets"  -> Add-on bucket
        // 2. Name "-compat" -> Add-on bucket.
        // 3. Otherwise content-detect: a folder with real playable missions
        //    (.fit, excluding the 3 ABL libs) -> Campaign; everything else with
        //    a data/ dir (a pure asset pack) -> Add-on (never vanish).
        bool typeCampaign = (_stricmp(type, "campaign") == 0);
        bool typeAddon    = (_stricmp(type, "dependency") == 0) ||
                            (_stricmp(type, "assets") == 0);

        bool isCampaign;
        if (typeCampaign)                          isCampaign = true;       // explicit override
        else if (typeAddon)                        isCampaign = false;      // explicit override
        else if (ContainsCI(fd.cFileName, "-compat")) isCampaign = false;   // name convention
        else                                       isCampaign = HasPlayableMissions(missDir);

        if (!isCampaign) {
            // Non-campaign content. Split into BASE vs stackable ADD-ON:
            //   BASE  = mod.json type=="dependency", OR folder name has "-compat".
            //           Bases (mco-compat/mc2x-compat) replace object2.pak/
            //           buildings.csv -> mutually exclusive -> radio group.
            //   ADD-ON= everything else (mod.json type=="assets", or any other
            //           non-campaign folder with data/) -> stackable -> checkbox.
            bool typeDependency = (_stricmp(type, "dependency") == 0);
            bool isBase = typeDependency || ContainsCI(fd.cFileName, "-compat");
            bool isTexQual = (_stricmp(radioGroup, "texture-quality") == 0);
            if (isBase) {
                if (s_baseCount >= MAX_MODS) continue;
                ModEntry& e = s_bases[s_baseCount++];
                _snprintf(e.folderName, sizeof(e.folderName), "%s", fd.cFileName);
                _snprintf(e.name, sizeof(e.name), "%s", name);
                _snprintf(e.desc, sizeof(e.desc), "%s", desc);
                e.envCount = envCount; memcpy(e.envKeys, envKeys, sizeof(envKeys)); memcpy(e.envVals, envVals, sizeof(envVals));
                e.isCompat = true; e.isCampaign = false; e.needs = CK_UNKNOWN;
                e.radioGroup[0] = '\0';
            } else if (isTexQual) {
                if (s_texQualCount >= MAX_MODS) continue;
                ModEntry& e = s_texQual[s_texQualCount++];
                _snprintf(e.folderName, sizeof(e.folderName), "%s", fd.cFileName);
                _snprintf(e.name, sizeof(e.name), "%s", name);
                _snprintf(e.desc, sizeof(e.desc), "%s", desc);
                e.envCount = envCount; memcpy(e.envKeys, envKeys, sizeof(envKeys)); memcpy(e.envVals, envVals, sizeof(envVals));
                e.isCompat = false; e.isCampaign = false; e.needs = CK_UNKNOWN;
                _snprintf(e.radioGroup, sizeof(e.radioGroup), "%s", radioGroup);
            } else {
                if (s_addonCount >= MAX_MODS) continue;
                ModEntry& e = s_addons[s_addonCount++];
                _snprintf(e.folderName, sizeof(e.folderName), "%s", fd.cFileName);
                _snprintf(e.name, sizeof(e.name), "%s", name);
                _snprintf(e.desc, sizeof(e.desc), "%s", desc);
                e.envCount = envCount; memcpy(e.envKeys, envKeys, sizeof(envKeys)); memcpy(e.envVals, envVals, sizeof(envVals));
                e.isCompat = true; e.isCampaign = false; e.needs = CK_UNKNOWN;
                e.radioGroup[0] = '\0';
            }
        } else {
            // Campaign bucket (radio / list).
            if (s_campCount >= MAX_MODS) continue;
            ModEntry& e = s_campaigns[s_campCount++];
            _snprintf(e.folderName, sizeof(e.folderName), "%s", fd.cFileName);
            _snprintf(e.name, sizeof(e.name), "%s", name);
            _snprintf(e.desc, sizeof(e.desc), "%s", desc);
            e.envCount = envCount; memcpy(e.envKeys, envKeys, sizeof(envKeys)); memcpy(e.envVals, envVals, sizeof(envVals));
            e.isCompat = false; e.isCampaign = true;
            e.needs = CK_PENDING;   // lazy: detected on first selection, not at startup
            e.radioGroup[0] = '\0';
        }
    } while (FindNextFileA(h, &fd));

    FindClose(h);
}

// Find the BASE index whose folder matches a detected kind, or -1. (Radio slot
// for base i is i+1, since base-radio slot 0 is the "None" entry.)
static int BaseIndexForKind(CompatKind k) {
    const char* tag = (k == CK_MCO) ? "mco" : (k == CK_MC2X) ? "mc2x" : NULL;
    if (!tag) return -1;
    for (int i = 0; i < s_baseCount; i++)
        if (ContainsCI(s_bases[i].folderName, tag)) return i;
    return -1;
}

// ---- UI sync --------------------------------------------------------------

// ---- Two-column layout geometry (single source of truth) ------------------
// Both WM_CREATE control creation and the refresh helpers use these so a rescan
// keeps the two-column layout identical to the initial build.
//
// Layout: left column holds the campaign listbox; right column holds base
// radios, add-ons, texture-quality, and options.  The listbox fills the full
// height of the right column so both columns bottom out at the same Y.
#define LP_MARGIN     10
// LEFT column: campaign listbox (scrolls) with the compatibility-base radios
// stacked beneath it. RIGHT column: the add-on checkbox ListView (scrolls) with
// the Faster-weapons toggle beneath it. A full-width Description box spans below
// both columns, then the Engine/Graphics aux buttons, then Launch/Cancel.
#define LP_LIST_X     LP_MARGIN
#define LP_LIST_Y     40
#define LP_LIST_W     320
#define LP_LIST_H     168                             // campaign list (fixed; scrolls)
#define LP_RLBL_Y     14                              // top label row (campaign + add-on)
#define LP_LBL_H      18                              // sub-group label height
#define LP_ROW_H      20                              // radio/checkbox height
#define LP_ROW_STEP   22                              // radio vertical step
// Base radios: under the campaign list (left column).
#define LP_BASE_LBL_Y (LP_LIST_Y + LP_LIST_H + 12)   // base group label top
#define LP_BASE_Y     (LP_BASE_LBL_Y + LP_LBL_H + 2) // first base radio top
#define LP_BASE_BOTTOM (LP_BASE_Y + BASE_SLOTS * LP_ROW_STEP)
#define LP_LCOL_BOTTOM LP_BASE_BOTTOM
// Right column: add-on ListView + Faster-weapons toggle (bottom-aligned to left col).
#define LP_RCOL_X     (LP_LIST_X + LP_LIST_W + 15)   // right column left edge
#define LP_RCOL_W     230                            // right column width
#define LP_FW_Y       (LP_LCOL_BOTTOM - LP_ROW_H)    // faster-weapons toggle top
#define LP_ADDON_LIST_Y LP_LIST_Y                    // add-on ListView top (aligns campaign list)
#define LP_ADDON_LIST_H (LP_FW_Y - 6 - LP_ADDON_LIST_Y)
#define LP_RCOL_BOTTOM LP_LCOL_BOTTOM
#define LP_COLS_BOTTOM LP_LCOL_BOTTOM
// Description box (full width, below both columns) — ~2x the old height.
#define LP_DESC_LBL_Y (LP_COLS_BOTTOM + 8)           // "Description" label top
#define LP_DESC_Y     (LP_DESC_LBL_Y + LP_LBL_H + 2) // description box top
#define LP_DESC_H     90
// Aux buttons (Engine Options / Graphics) then Launch/Cancel.
#define LP_AUX_BTN_Y  (LP_DESC_Y + LP_DESC_H + 8)
#define LP_AUX_BTN_H  26
#define LP_CLIENT_W   (LP_RCOL_X + LP_RCOL_W + LP_MARGIN)  // 10+320+15+230+10 = 585
#define LP_BTN_Y      (LP_AUX_BTN_Y + LP_AUX_BTN_H + 8)    // Launch/Cancel row top
#define LP_BTN_H      28

// Show/hide + relabel the FIXED base-radio slots from the current s_bases[] set.
// Slot 0 is always the "None" radio (always shown). Slot i+1 maps to base i.
// Never destroys/recreates a window — geometry is constant. Default selection is
// "None" (slot 0) whenever the set changes.
static void RefreshBaseSlots() {
    for (int i = 0; i < BASE_SLOTS; i++) {
        if (!s_hBase[i]) continue;
        if (i == 0) {
            SetWindowTextA(s_hBase[i], "None");
            ShowWindow(s_hBase[i], SW_SHOW);
        } else if ((i - 1) < s_baseCount) {
            char label[300];
            _snprintf(label, sizeof(label), "%s  (%s)",
                      s_bases[i-1].name, s_bases[i-1].folderName);
            label[sizeof(label)-1] = '\0';
            SetWindowTextA(s_hBase[i], label);
            ShowWindow(s_hBase[i], SW_SHOW);
        } else {
            SetWindowTextA(s_hBase[i], "");
            ShowWindow(s_hBase[i], SW_HIDE);
        }
    }
    // Default the exclusive group to "None".
    for (int i = 0; i < BASE_SLOTS; i++)
        if (s_hBase[i])
            SendMessageA(s_hBase[i], BM_SETCHECK,
                         (i == 0) ? BST_CHECKED : BST_UNCHECKED, 0);
}

// Repopulate the add-on checkbox ListView from s_addons[]. Clears all rows and
// inserts one per add-on (unbounded — no fixed slot cap). Check state resets to
// unchecked; RestoreMainPageSelections re-applies the saved checks afterwards.
static void RefreshAddonList() {
    if (!s_hAddonList) return;
    ListView_DeleteAllItems(s_hAddonList);
    for (int i = 0; i < s_addonCount; i++) {
        char label[512];
        _snprintf(label, sizeof(label), "%s  (%s)",
                  s_addons[i].name, s_addons[i].folderName);
        label[sizeof(label)-1] = '\0';
        LVITEMA it = {};
        it.mask = LVIF_TEXT;
        it.iItem = i;
        it.iSubItem = 0;
        it.pszText = label;
        // Explicit ...A message: the ListView_* macros gate on #ifdef UNICODE,
        // which is DEFINED (as 0) here, so the macro would send the WIDE message
        // and misread our ANSI text as UTF-16 (ASCII byte-pairs -> CJK glyphs).
        SendMessageA(s_hAddonList, LVM_INSERTITEMA, 0, (LPARAM)&it);
    }
}

// Select base-radio slot `slot` (0 = None) exclusively. Mirrors what
// BS_AUTORADIOBUTTON does on a user click, but lets code drive auto-detect.
static void SelectBaseRadio(int slot) {
    for (int i = 0; i < BASE_SLOTS; i++)
        if (s_hBase[i])
            SendMessageA(s_hBase[i], BM_SETCHECK,
                         (i == slot) ? BST_CHECKED : BST_UNCHECKED, 0);
}

// Return the currently selected base-radio slot (0 = None), or 0 if none.
static int SelectedBaseSlot() {
    for (int i = 0; i < BASE_SLOTS; i++)
        if (s_hBase[i] && SendMessageA(s_hBase[i], BM_GETCHECK, 0, 0) == BST_CHECKED)
            return i;
    return 0;
}

// Built-in description for entries that ship no mod.json "description" (Stock,
// the compat bases). Matched case-insensitively on folder name; returns "" if
// none, so the caller can fall back to the mod's own description.
static const char* BuiltinBlurb(const char* folderName) {
    if (!folderName || !folderName[0])
        return "Stock MechCommander 2 base game. No mods active. Pick a base only "
               "if a campaign needs one; stack any add-ons on top.";
    if (ContainsCI(folderName, "mco-compat"))
        return "MechCommander Omnitech compatibility base. Replaces object2.pak / "
               "buildings.csv so MCO campaigns load. Mutually exclusive with other bases.";
    if (ContainsCI(folderName, "mc2x-compat"))
        return "MC2X 'fat pack' compatibility base. Replaces object2.pak / "
               "buildings.csv for MC2X content. Mutually exclusive with other bases.";
    return "";
}

// Set the description pane text. NULL/empty shows a neutral placeholder.
static void SetDescBox(const char* text) {
    if (!s_hDescBox) return;
    SetWindowTextA(s_hDescBox, (text && text[0]) ? text : "(no description provided)");
}

// Show the description for a base radio slot (0 = None).
static void DescribeBase(int slot) {
    if (slot <= 0 || (slot - 1) >= s_baseCount) {
        SetDescBox(BuiltinBlurb(""));   // None -> stock blurb
        return;
    }
    const ModEntry& b = s_bases[slot - 1];
    SetDescBox(b.desc[0] ? b.desc : BuiltinBlurb(b.folderName));
}

// Show the description for the focused add-on row.
static void DescribeAddon(int row) {
    if (row < 0 || row >= s_addonCount) return;
    const ModEntry& a = s_addons[row];
    SetDescBox(a.desc[0] ? a.desc : BuiltinBlurb(a.folderName));
}

static void UpdateForSelection() {
    int sel = (int)SendMessageA(s_hListBox, LB_GETCURSEL, 0, 0);
    // sel 0 = Stock (base game); sel-1 indexes s_campaigns.
    const char* status = "Stock campaign. Pick a base only if needed; stack any add-ons.";
    if (sel > 0 && sel <= s_campCount) {
        // Lazy compat detection: first time a campaign is selected, scan its files.
        // Deferred from ScanMods to keep startup fast (no I/O before window appears).
        if (s_campaigns[sel-1].needs == CK_PENDING) {
            char modDir[MAX_PATH];
            _snprintf(modDir, sizeof(modDir), "%s%s", s_modsPath, s_campaigns[sel-1].folderName);
            modDir[sizeof(modDir)-1] = '\0';
            s_campaigns[sel-1].needs = DetectCompat(modDir);
        }
        CompatKind needs = s_campaigns[sel-1].needs;
        int baseIdx = BaseIndexForKind(needs);   // index into s_bases[], or -1
        if (needs == CK_MCO && baseIdx >= 0)
            status = "Detected: MechCommander Omnitech campaign. mco-compat base auto-selected.";
        else if (needs == CK_MC2X && baseIdx >= 0)
            status = "Detected: MC2X campaign. mc2x-compat base auto-selected.";
        else if (needs == CK_MCO || needs == CK_MC2X)
            status = "Detected a compat need, but no matching base layer is installed.";
        else
            status = "Could not auto-detect. If this is an MC2X pack, pick a base; pure-stock needs none.";
        // Auto-SELECT the matching base radio (slot = baseIdx+1; slot 0 = None).
        // Stock or undetected -> None (slot 0). Add-on checkboxes left as-is so the
        // player's stacked picks survive a campaign change.
        SelectBaseRadio(baseIdx >= 0 ? baseIdx + 1 : 0);
    } else {
        // Stock: base radio -> None. Add-on checkboxes preserved.
        SelectBaseRadio(0);
    }
    // Description pane: campaign blurb (mod.json description or Stock built-in)
    // followed by the auto-detect/compat status line.
    char text[2048];
    const char* campDesc = (sel > 0 && sel <= s_campCount && s_campaigns[sel-1].desc[0])
                         ? s_campaigns[sel-1].desc
                         : (sel <= 0 ? BuiltinBlurb("") : "");
    if (campDesc[0])
        _snprintf(text, sizeof(text), "%s\r\n\r\n%s", campDesc, status);
    else
        _snprintf(text, sizeof(text), "%s", status);
    text[sizeof(text)-1] = '\0';
    SetDescBox(text);
}

// Reset + re-scan mods, repopulate listbox + compat slots. Used after import.
static void RescanAndRepopulate(const char* selectFolder) {
    // Preserve the live add-on check state across the ListView rebuild below:
    // RefreshAddonList() -> ListView_DeleteAllItems() wipes per-row check state
    // (the old fixed-slot HWNDs kept it; the ListView does not). Campaign/base
    // selection is re-derived from selectFolder + auto-detect, so only the
    // stacked add-on checks need carrying over. Captured by folder name so it
    // survives reordering/additions from the rescan.
    char prevAddons[1024]; prevAddons[0] = '\0';
    for (int i = 0; i < s_addonCount; i++) {
        if (s_hAddonList && ListView_GetCheckState(s_hAddonList, i)) {
            if (prevAddons[0]) strncat(prevAddons, ",", sizeof(prevAddons)-strlen(prevAddons)-1);
            strncat(prevAddons, s_addons[i].folderName, sizeof(prevAddons)-strlen(prevAddons)-1);
        }
    }

    s_campCount = 0;
    s_baseCount = 0;
    s_addonCount = 0;
    s_texQualCount = 0;
    ScanMods(s_modsPath);
    // Re-resolve the selected texture-pack index against the refreshed mod list.
    // If the previously-selected pack is no longer present, fall back to None.
    if (s_gfxTexPackIdx > 0) {
        int prev = s_gfxTexPackIdx;
        s_gfxTexPackIdx = 0;
        if ((prev - 1) < s_texQualCount) {
            // Verify the folder is still there at the same slot.
            s_gfxTexPackIdx = prev;
        }
    }

    SendMessageA(s_hListBox, LB_RESETCONTENT, 0, 0);
    SendMessageA(s_hListBox, LB_ADDSTRING, 0, (LPARAM)"Stock (base game)");
    int wantSel = 0;
    for (int i = 0; i < s_campCount; i++) {
        SendMessageA(s_hListBox, LB_ADDSTRING, 0, (LPARAM)s_campaigns[i].name);
        if (selectFolder && selectFolder[0] &&
            _stricmp(s_campaigns[i].folderName, selectFolder) == 0)
            wantSel = i + 1;   // +1 for the Base Game row
    }
    SendMessageA(s_hListBox, LB_SETCURSEL, wantSel, 0);
    RefreshBaseSlots();
    RefreshAddonList();
    // Re-apply the preserved add-on checks against the freshly-inserted rows.
    if (prevAddons[0]) {
        char buf[1024]; _snprintf(buf, sizeof(buf), "%s", prevAddons); buf[sizeof(buf)-1] = '\0';
        char* ctx = NULL;
        for (char* tok = strtok_s(buf, ",", &ctx); tok; tok = strtok_s(NULL, ",", &ctx)) {
            for (int i = 0; i < s_addonCount; i++)
                if (_stricmp(s_addons[i].folderName, tok) == 0) {
                    ListView_SetCheckState(s_hAddonList, i, TRUE);
                    break;
                }
        }
    }
    UpdateForSelection();
}

// ---- Mod-pack installer (drag-drop .exe onto window) ----------------------

static void CopyToClipboard(HWND hwnd, const char* text) {
    if (!OpenClipboard(hwnd)) return;
    EmptyClipboard();
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, strlen(text) + 1);
    if (hMem) {
        memcpy(GlobalLock(hMem), text, strlen(text) + 1);
        GlobalUnlock(hMem);
        SetClipboardData(CF_TEXT, hMem);
    }
    CloseClipboard();
}

static void TryInstallModPack(HWND hwnd, const char* exePath) {
    // Derive target folder name from the exe filename (lowercase, spaces->dashes).
    const char* fname = strrchr(exePath, '\\');
    fname = fname ? fname + 1 : exePath;
    char modName[256];
    _snprintf(modName, sizeof(modName), "%s", fname);
    char* dot = strrchr(modName, '.');
    if (dot && _stricmp(dot, ".exe") == 0) *dot = '\0';
    for (char* p = modName; *p; p++) {
        *p = (char)tolower((unsigned char)*p);
        if (*p == ' ') *p = '-';
    }

    char targetDir[MAX_PATH];
    _snprintf(targetDir, sizeof(targetDir), "%s%s", s_modsPath, modName);

    // Copy target path to clipboard so user can paste if the installer asks.
    CopyToClipboard(hwnd, targetDir);

    char msg[MAX_PATH + 512];
    _snprintf(msg, sizeof(msg),
        "Run installer?\n\n%s\n\n"
        "Target folder (copied to clipboard):\n%s\n\n"
        "The installer will be asked to install there automatically.\n"
        "If it shows its own UI, paste the path above when asked.",
        exePath, targetDir);
    if (MessageBoxA(hwnd, msg, "Install Mod Pack", MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;

    // Pass target dir via NSIS /D= and Inno /DIR= simultaneously.
    char params[MAX_PATH + 64];
    _snprintf(params, sizeof(params), "/S /D=%s /SILENT /DIR=\"%s\"", targetDir, targetDir);

    SHELLEXECUTEINFOA sei = {};
    sei.cbSize       = sizeof(sei);
    sei.fMask        = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd         = hwnd;
    sei.lpFile       = exePath;
    sei.lpParameters = params;
    sei.nShow        = SW_SHOWNORMAL;
    if (!ShellExecuteExA(&sei)) {
        MessageBoxA(hwnd, "Failed to start installer.", "Error", MB_ICONERROR | MB_OK);
        return;
    }

    WaitForSingleObject(sei.hProcess, INFINITE);
    CloseHandle(sei.hProcess);

    // Rescan and refresh the campaign list.
    RescanAndRepopulate("");
}

// ---- Launch mc2.exe -------------------------------------------------------

static void DoLaunch(HWND hwnd) {
    int sel = (int)SendMessageA(s_hListBox, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR) sel = 0;

    // Build MC2_MOD_DEPS = [checked stackable add-ons] then [selected base].
    // file.cpp treats MC2_MOD_DEPS as highest-priority-FIRST, so add-ons are
    // listed BEFORE the base and thus override it. The base (mco/mc2x-compat),
    // if not "None", goes last.
    ClearAllModEnv();

    char deps[1024] = "";
    int addonChecked = 0;
    for (int i = 0; i < s_addonCount; i++) {
        if (s_hAddonList && ListView_GetCheckState(s_hAddonList, i)) {
            if (deps[0]) strncat(deps, ",", sizeof(deps)-strlen(deps)-1);
            strncat(deps, s_addons[i].folderName, sizeof(deps)-strlen(deps)-1);
            ApplyModEnv(s_addons[i]);
            addonChecked++;
        }
    }
    // Texture-pack mod from Graphics dialog (s_gfxTexPackIdx: 0=None; i -> s_texQual[i-1]).
    if (s_gfxTexPackIdx > 0 && (s_gfxTexPackIdx - 1) < s_texQualCount) {
        if (deps[0]) strncat(deps, ",", sizeof(deps)-strlen(deps)-1);
        strncat(deps, s_texQual[s_gfxTexPackIdx - 1].folderName, sizeof(deps)-strlen(deps)-1);
        ApplyModEnv(s_texQual[s_gfxTexPackIdx - 1]);
    }
    int baseSlot = SelectedBaseSlot();   // 0 = None; slot i -> s_bases[i-1]
    if (baseSlot > 0 && (baseSlot - 1) < s_baseCount) {
        if (deps[0]) strncat(deps, ",", sizeof(deps)-strlen(deps)-1);
        strncat(deps, s_bases[baseSlot-1].folderName, sizeof(deps)-strlen(deps)-1);
        ApplyModEnv(s_bases[baseSlot-1]);
    }

    if (sel == 0) {
        // Stock (base game). With add-ons checked this is the "stock campaign +
        // borrowed mechs/weapons" path: export an EMPTY active mod + the checked
        // deps. Slice 1's engine mounts deps with no active campaign (and guards
        // the dep ABL libs from overriding stock AI). No warning either way:
        //   - Stock + nothing  = pure base game.
        //   - Stock + add-ons  = a valid, intentional combo.
        SetEnvironmentVariableA("MC2_ACTIVE_MOD", "");          // empty, not unset
        SetEnvironmentVariableA("MC2_MOD_DEPS", deps[0] ? deps : NULL);
    } else if (sel <= s_campCount) {
        const ModEntry& camp = s_campaigns[sel-1];
        // Warn ONLY for a campaign that auto-NEEDS a compat base (MCO/MC2X
        // detected) but the base radio is still "None". An auto-undetected
        // campaign (pure-stock-range) is assumed fine; Stock is handled above.
        if (baseSlot == 0 && (camp.needs == CK_MCO || camp.needs == CK_MC2X)) {
            char msg[512];
            _snprintf(msg, sizeof(msg),
                "Campaign \"%s\" selected with NO compatibility base picked.\n\n%s\n\nLaunch anyway?",
                camp.name,
                camp.needs == CK_MCO  ? "It looks like a MechCommander Omnitech pack and will almost certainly fail to load without the mco-compat base." :
                                        "It looks like an MC2X pack and will likely fail without the mc2x-compat base.");
            if (MessageBoxA(hwnd, msg, "No base picked",
                            MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES)
                return;
        }
        SetEnvironmentVariableA("MC2_ACTIVE_MOD", camp.folderName);
        SetEnvironmentVariableA("MC2_MOD_DEPS", deps[0] ? deps : NULL);
        ApplyModEnv(camp);
    }

    // Gameplay "Faster weapons" toggle (NOT a mod): drive engine env vars in the
    // child process. NULL unsets the var so unchecked == vanilla weapons.
    BOOL fw = (SendMessageA(s_hFasterWeapons, BM_GETCHECK, 0, 0) == BST_CHECKED);
    SetEnvironmentVariableA("MC2_DIRECT_FIRE_STRAIGHT", fw ? "1" : NULL);
    SetEnvironmentVariableA("MC2_PROJECTILE_SPEED_MULT", fw ? "2" : NULL);

    // Apply engine-option env vars from the Options dialog (launcher_env.json).
    // Skip any vars that DoLaunch already sets explicitly above.
    for (int i = 0; i < k_envVarCount; i++) {
        // MC2_DIRECT_FIRE_STRAIGHT and MC2_PROJECTILE_SPEED_MULT are driven by
        // the Faster Weapons toggle — skip them to avoid double-set.
        if (strcmp(k_envVars[i].key, "MC2_DIRECT_FIRE_STRAIGHT") == 0) continue;
        if (strcmp(k_envVars[i].key, "MC2_PROJECTILE_SPEED_MULT") == 0) continue;

        const char* val = s_envVarValues[i];
        if (val[0]) {
            SetEnvironmentVariableA(k_envVars[i].key, val);
        } else {
            // Explicitly unset so a previously-set ambient value in the shell
            // doesn't bleed into the child.
            SetEnvironmentVariableA(k_envVars[i].key, NULL);
        }
    }

    // Apply Graphics Options env vars.
    // MC2_STATICPROP_TEX_TIER: texture resolution tier (engine reads this in the
    //   texture-array loader to pick which mip level / atlas to load).
    SetEnvironmentVariableA("MC2_STATICPROP_TEX_TIER", k_texTierValues[s_gfxTexQual]);
    // MC2_SHADOW_MAP_SIZE: per-cascade shadow texture size.
    SetEnvironmentVariableA("MC2_SHADOW_MAP_SIZE", k_shadSizeValues[s_gfxShadQual]);
    // MC2_FPS_CAP: frame cap in FPS (gameosmain.cpp reads this).
    if (s_gfxFrameCapOn) {
        char fpsBuf[32];
        _snprintf(fpsBuf, sizeof(fpsBuf), "%d", s_gfxFrameCapFps);
        SetEnvironmentVariableA("MC2_FPS_CAP", fpsBuf);
    } else {
        SetEnvironmentVariableA("MC2_FPS_CAP", NULL);  // unset = no cap
    }
    // MC2_VSYNC: vsync toggle (gos_render.cpp reads this via SDL_GL_SetSwapInterval).
    SetEnvironmentVariableA("MC2_VSYNC", s_gfxVsync ? "1" : NULL);

    char mc2Path[MAX_PATH];
    _snprintf(mc2Path, sizeof(mc2Path), "%smc2.exe", s_launcherDir);
    mc2Path[sizeof(mc2Path)-1] = '\0';
    if (s_launcherDir[0]) SetCurrentDirectoryA(s_launcherDir);

    // [LAUNCHER-BOOTSTRAP v1] Sentinel so the child mc2.exe knows it was started
    // by the launcher and runs the game directly instead of re-spawning us.
    SetEnvironmentVariableA("MC2_LAUNCHED", "1");

    STARTUPINFOA si = {}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(mc2Path, NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        char msg[512];
        _snprintf(msg, sizeof(msg), "Failed to launch %s\n\nError code: %lu", mc2Path, GetLastError());
        MessageBoxA(hwnd, msg, "Launch Error", MB_ICONERROR | MB_OK);
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    SaveMainPageSelections();
    PostQuitMessage(0);
}

// ---- Import: helpers ------------------------------------------------------

// Resolve the python launcher: prefer "py" (-3), then "python". Returns ""
// (empty) if neither is on PATH. Stores the chosen verb in s_pyExe.
static bool ResolvePython() {
    char found[MAX_PATH];
    if (SearchPathA(NULL, "py", ".exe", sizeof(found), found, NULL)) {
        strcpy(s_pyExe, "py");
        return true;
    }
    if (SearchPathA(NULL, "python", ".exe", sizeof(found), found, NULL)) {
        strcpy(s_pyExe, "python");
        return true;
    }
    return false;
}

// Resolve the importer. PREFER the standalone frozen exe (no python needed):
//   <launcherDir>importer\import_tool\import_tool.exe   (PyInstaller onedir)
// then %MC2_IMPORTER_DIR%\import_tool.exe (frozen, env override),
// then FALL BACK to the dev python tree:
//   <launcherDir>importer\mc2_import\import_tool.py
//   %MC2_IMPORTER_DIR%\import_tool.py
// Sets s_importerFrozen accordingly. Returns true and fills s_importerPath.
static bool ResolveImporter() {
    char cand[MAX_PATH];
    char envDir[MAX_PATH] = "";
    DWORD n = GetEnvironmentVariableA("MC2_IMPORTER_DIR", envDir, sizeof(envDir));
    bool haveEnv = (n > 0 && n < sizeof(envDir));
    char envSep = haveEnv && (envDir[n-1] == '\\' || envDir[n-1] == '/') ? '\0' : '\\';

    // (1) frozen exe under the deploy's importer\ (preferred — ships without python).
    _snprintf(cand, sizeof(cand), "%simporter\\import_tool\\import_tool.exe", s_launcherDir);
    cand[sizeof(cand)-1] = '\0';
    if (FileExists(cand)) { strcpy(s_importerPath, cand); s_importerFrozen = true; return true; }

    // (2) frozen exe via MC2_IMPORTER_DIR.
    if (haveEnv) {
        if (envSep) _snprintf(cand, sizeof(cand), "%s\\import_tool.exe", envDir);
        else        _snprintf(cand, sizeof(cand), "%simport_tool.exe", envDir);
        cand[sizeof(cand)-1] = '\0';
        if (FileExists(cand)) { strcpy(s_importerPath, cand); s_importerFrozen = true; return true; }
    }

    // (3) dev python tree under importer\mc2_import\.
    _snprintf(cand, sizeof(cand), "%simporter\\mc2_import\\import_tool.py", s_launcherDir);
    cand[sizeof(cand)-1] = '\0';
    if (FileExists(cand)) { strcpy(s_importerPath, cand); s_importerFrozen = false; return true; }

    // (4) dev .py via MC2_IMPORTER_DIR.
    if (haveEnv) {
        if (envSep) _snprintf(cand, sizeof(cand), "%s\\import_tool.py", envDir);
        else        _snprintf(cand, sizeof(cand), "%simport_tool.py", envDir);
        cand[sizeof(cand)-1] = '\0';
        if (FileExists(cand)) { strcpy(s_importerPath, cand); s_importerFrozen = false; return true; }
    }
    return false;
}

// Run a child process with stdout (and stderr) redirected to a pipe, blocking
// until exit. Captures up to outSz-1 bytes of stdout into outBuf. If childProcOut
// is non-NULL it receives the live process handle (for Cancel/TerminateProcess);
// the caller must NOT close it (this fn closes it on return). Returns false if the
// process could not be created; *exitCode set on success.
static bool RunCaptured(const char* cmdLine, char* outBuf, int outSz,
                        DWORD* exitCode, HANDLE* childProcOut) {
    if (outBuf && outSz) outBuf[0] = '\0';

    SECURITY_ATTRIBUTES sa = {}; sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);   // read end not inherited

    STARTUPINFOA si = {}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError  = wr;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi = {};
    // CreateProcessA may modify the command line buffer -> use a writable copy.
    char* cl = (char*)malloc(strlen(cmdLine) + 1);
    if (!cl) { CloseHandle(rd); CloseHandle(wr); return false; }
    strcpy(cl, cmdLine);

    BOOL ok = CreateProcessA(NULL, cl, NULL, NULL, TRUE,
                             CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    free(cl);
    CloseHandle(wr);   // parent's write end — child holds the other
    if (!ok) { CloseHandle(rd); return false; }

    if (childProcOut) *childProcOut = pi.hProcess;

    // Drain stdout while the child runs (prevents pipe-full deadlock).
    int used = 0;
    for (;;) {
        char chunk[4096];
        DWORD got = 0;
        if (!ReadFile(rd, chunk, sizeof(chunk), &got, NULL) || got == 0) break;
        if (outBuf && used < outSz - 1) {
            int copy = (int)got;
            if (copy > outSz - 1 - used) copy = outSz - 1 - used;
            memcpy(outBuf + used, chunk, copy);
            used += copy;
        }
    }
    if (outBuf) outBuf[used] = '\0';

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    if (exitCode) *exitCode = code;

    CloseHandle(rd);
    if (childProcOut) *childProcOut = NULL;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

// Build a quoted command line. FROZEN exe runs directly (no python):
//   "<importer.exe>" <sub> "<folder>" [extra] --json
// DEV python tree:
//   py -3 "<importer.py>" <sub> "<folder>" [extra] --json
static void BuildCmd(char* out, int outSz, const char* sub, const char* folder,
                     const char* extra) {
    // Strip a trailing slash off the folder: a quoted arg ending in '\' escapes the
    // closing quote (e.g. a drive-root pick "A:\") and mangles the command line.
    char fld[MAX_PATH];
    _snprintf(fld, sizeof(fld), "%s", folder ? folder : "");
    fld[sizeof(fld)-1] = '\0';
    for (size_t fl = strlen(fld); fl > 0 && (fld[fl-1] == '\\' || fld[fl-1] == '/'); )
        fld[--fl] = '\0';

    if (s_importerFrozen)
        _snprintf(out, outSz, "\"%s\" %s \"%s\" %s--json",
                  s_importerPath, sub, fld, extra ? extra : "");
    else if (_stricmp(s_pyExe, "py") == 0)
        _snprintf(out, outSz, "py -3 \"%s\" %s \"%s\" %s--json",
                  s_importerPath, sub, fld, extra ? extra : "");
    else
        _snprintf(out, outSz, "python \"%s\" %s \"%s\" %s--json",
                  s_importerPath, sub, fld, extra ? extra : "");
    out[outSz-1] = '\0';
}

// ---- Import: worker thread ------------------------------------------------
//
// Runs `auto` (optionally with --force), captures stdout/exit into globals, then
// PostMessage(WM_APP_IMPORT_DONE). All UI work happens back on the main thread.

static unsigned __stdcall ImportWorker(void* arg) {
    // s_launcherDir keeps a TRAILING SLASH. A quoted arg ending in a backslash --
    // --deploy "...rc1\" -- has the closing quote ESCAPED by that backslash under the
    // Windows CRT/CommandLineToArgv rules, so the deploy argument swallows the rest of
    // the command line (--force, --json) and the importer mis-parses. Strip trailing
    // slashes before quoting. (This broke EVERY GUI import; the CLI path passed an
    // unslashed --deploy, which is why it worked there.)
    char deployDir[MAX_PATH];
    _snprintf(deployDir, sizeof(deployDir), "%s", s_launcherDir);
    deployDir[sizeof(deployDir)-1] = '\0';
    for (size_t dl = strlen(deployDir); dl > 0 && (deployDir[dl-1] == '\\' || deployDir[dl-1] == '/'); )
        deployDir[--dl] = '\0';

    char deployArg[MAX_PATH + 16];
    _snprintf(deployArg, sizeof(deployArg), "--deploy \"%s\" ", deployDir);
    deployArg[sizeof(deployArg)-1] = '\0';

    char extra[MAX_PATH + 32];
    if (s_importForce)
        _snprintf(extra, sizeof(extra), "%s--force ", deployArg);
    else
        _snprintf(extra, sizeof(extra), "%s", deployArg);
    extra[sizeof(extra)-1] = '\0';

    char cmd[2 * MAX_PATH + 256];
    BuildCmd(cmd, sizeof(cmd), "auto", s_importFolder, extra);

    s_autoExit = 1;
    s_autoJson[0] = '\0';
    HANDLE dummy = NULL;
    // Publish the child handle for the Cancel button via s_importChildProc.
    // RunCaptured sets *childProcOut to the live handle then clears it on exit.
    RunCaptured(cmd, s_autoJson, sizeof(s_autoJson), &s_autoExit, &s_importChildProc);

    PostMessageA(s_hMainWnd, WM_APP_IMPORT_DONE, (WPARAM)s_autoExit, 0);
    return 0;
}

// ---- Import: progress dialog ----------------------------------------------

static LRESULT CALLBACK ProgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HWND t = CreateWindowA("STATIC",
            "Importing... (this can take a few minutes)",
            WS_CHILD | WS_VISIBLE, 16, 14, 320, 18, hwnd, (HMENU)IDC_PROG_TEXT, s_hInst, NULL);
        SendMessageA(t, WM_SETFONT, (WPARAM)hFont, TRUE);
        HWND bar = CreateWindowExA(0, PROGRESS_CLASSA, NULL,
            WS_CHILD | WS_VISIBLE | PBS_MARQUEE, 16, 40, 320, 18,
            hwnd, (HMENU)IDC_PROG_BAR, s_hInst, NULL);
        SendMessageA(bar, PBM_SETMARQUEE, TRUE, 30);
        HWND c = CreateWindowA("BUTTON", "Cancel",
            WS_CHILD | WS_VISIBLE, 138, 70, 80, 26, hwnd, (HMENU)IDC_PROG_CANCEL, s_hInst, NULL);
        SendMessageA(c, WM_SETFONT, (WPARAM)hFont, TRUE);
        break;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_PROG_CANCEL) {
            s_importCancelled = true;
            HANDLE c = s_importChildProc;
            if (c) TerminateProcess(c, 1);   // staging makes a killed import safe
            EnableWindow(GetDlgItem(hwnd, IDC_PROG_CANCEL), FALSE);
            SetWindowTextA(GetDlgItem(hwnd, IDC_PROG_TEXT), "Cancelling...");
        }
        break;
    case WM_CLOSE:
        // Block manual close while running — Cancel is the path out.
        return 0;
    default:
        return DefWindowProcA(hwnd, msg, wParam, lParam);
    }
    return 0;
}

static void RegisterProgClass() {
    static bool done = false;
    if (done) return;
    WNDCLASSA wc = {};
    wc.lpfnWndProc   = ProgProc;
    wc.hInstance     = s_hInst;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor       = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.lpszClassName = "MC2ProgressWnd";
    RegisterClassA(&wc);
    done = true;
}

static void ShowProgressDialog(HWND owner) {
    RegisterProgClass();
    int w = 360, h = 130;
    RECT rc = { 0, 0, w, h };
    DWORD style = WS_POPUP | WS_CAPTION | WS_VISIBLE;
    AdjustWindowRect(&rc, style, FALSE);
    int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
    RECT orc; GetWindowRect(owner, &orc);
    int ox = orc.left + ((orc.right - orc.left) - ww) / 2;
    int oy = orc.top  + ((orc.bottom - orc.top) - wh) / 2;
    s_hProgWnd = CreateWindowA("MC2ProgressWnd", "Importing",
        style, ox, oy, ww, wh, owner, NULL, s_hInst, NULL);
    UpdateWindow(s_hProgWnd);
}

// ---- Import: orchestration ------------------------------------------------

// SHBrowseForFolder pick. Returns true + fills out[] (MAX_PATH).
static bool PickFolder(HWND owner, char* out, int outSz) {
    BROWSEINFOA bi = {};
    bi.hwndOwner = owner;
    bi.lpszTitle = "Pick the install ROOT (where the .fst files are), not a data/ or subfolder.";
    bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    if (!pidl) return false;
    char path[MAX_PATH] = "";
    bool ok = SHGetPathFromIDListA(pidl, path) != FALSE;
    CoTaskMemFree(pidl);
    if (!ok || !path[0]) return false;
    _snprintf(out, outSz, "%s", path);
    out[outSz-1] = '\0';
    return true;
}

// Enable/disable the main-window interactive controls during import.
static void SetMainControlsEnabled(BOOL en) {
    EnableWindow(s_hListBox, en);
    EnableWindow(s_hImport, en);
    EnableWindow(GetDlgItem(s_hMainWnd, IDC_LAUNCH), en);
    EnableWindow(GetDlgItem(s_hMainWnd, IDC_CANCEL), en);
    for (int i = 0; i < BASE_SLOTS; i++)
        if (s_hBase[i]) EnableWindow(s_hBase[i], en);
    if (s_hAddonList) EnableWindow(s_hAddonList, en);
    if (s_hFasterWeapons) EnableWindow(s_hFasterWeapons, en);
}

// Start the worker thread + show the modal progress dialog. s_importForce must
// be set before calling. Assumes s_importing already true + button disabled.
static void StartImportRun(HWND hwnd) {
    SetMainControlsEnabled(FALSE);
    EnableWindow(hwnd, FALSE);              // disable main window (re-entrancy guard, M1)
    s_importCancelled = false;
    s_importChildProc = NULL;
    ShowProgressDialog(hwnd);

    unsigned tid = 0;
    s_importThread = (HANDLE)_beginthreadex(NULL, 0, ImportWorker, NULL, 0, &tid);
    if (!s_importThread) {
        // Could not spawn — recover synchronously.
        if (s_hProgWnd) { DestroyWindow(s_hProgWnd); s_hProgWnd = NULL; }
        EnableWindow(hwnd, TRUE);
        SetMainControlsEnabled(TRUE);
        s_importing = false;
        MessageBoxA(hwnd, "Could not start the import worker thread.", "Import Error",
                    MB_ICONERROR | MB_OK);
    }
}

static void OnImportButton(HWND hwnd) {
    // Re-entrancy guard (M1): disable SYNCHRONOUSLY before any async work.
    if (s_importing) return;

    if (!ResolveImporter()) {
        MessageBoxA(hwnd,
            "Importer not found.\n\nExpected at:\n"
            "  <launcher>\\importer\\import_tool\\import_tool.exe   (frozen)\n"
            "  <launcher>\\importer\\mc2_import\\import_tool.py      (dev)\n\n"
            "Or set the MC2_IMPORTER_DIR environment variable to its folder.",
            "Importer not found", MB_ICONERROR | MB_OK);
        return;
    }
    // Python is only needed for the dev (.py) fallback. The frozen exe is self-contained.
    if (!s_importerFrozen && !ResolvePython()) {
        MessageBoxA(hwnd, "Python required for import (dev build).\n\n"
            "Neither 'py' nor 'python' was found on PATH.",
            "Python required", MB_ICONERROR | MB_OK);
        return;
    }

    char folder[MAX_PATH];
    if (!PickFolder(hwnd, folder, sizeof(folder))) return;   // user cancelled

    // Probe (read-only classify).
    char probeCmd[2 * MAX_PATH + 64];
    BuildCmd(probeCmd, sizeof(probeCmd), "probe", folder, NULL);
    char json[16384]; DWORD ex = 1; HANDLE dummy = NULL;
    if (!RunCaptured(probeCmd, json, sizeof(json), &ex, &dummy)) {
        MessageBoxA(hwnd, "Failed to run the importer (probe).", "Import Error",
                    MB_ICONERROR | MB_OK);
        return;
    }

    char kind[64] = "", action[64] = "", reject[1024] = "";
    JsonGetString(json, "kind", kind, sizeof(kind));
    JsonGetString(json, "action", action, sizeof(action));
    JsonGetString(json, "reject_reason", reject, sizeof(reject));

    bool rejected = (_stricmp(kind, "UNKNOWN") == 0) || (_stricmp(kind, "STOCK") == 0) ||
                    (_stricmp(action, "reject") == 0) || !kind[0];
    if (rejected) {
        char msg[1400];
        _snprintf(msg, sizeof(msg),
            "Cannot import this folder.\n\n%s\n\nHint: pick the install ROOT (where the .fst files are), "
            "not a data/ or subfolder.",
            reject[0] ? reject : "The folder was not recognized as an importable install or pack.");
        msg[sizeof(msg)-1] = '\0';
        MessageBoxA(hwnd, msg, "Cannot import", MB_ICONWARNING | MB_OK);
        return;
    }

    // Confirm — human sentence per kind.
    char confirm[1024];
    if (_stricmp(kind, "MCO") == 0)
        strcpy(confirm, "Detected a MechCommander Omnitech install. Import it as a playable campaign "
                        "(this can take several minutes)?");
    else if (ContainsCI(kind, "MC2X"))
        strcpy(confirm, "Detected a MechCommander 2 X-pack install. Import it as a playable campaign "
                        "(this can take several minutes)?");
    else if (_stricmp(kind, "PACK") == 0)
        strcpy(confirm, "Detected a content pack. Import it as a playable campaign "
                        "(this can take a few minutes)?");
    else
        _snprintf(confirm, sizeof(confirm),
            "Detected: %s. Import it as a playable campaign (this can take several minutes)?", kind);
    if (MessageBoxA(hwnd, confirm, "Import", MB_ICONQUESTION | MB_YESNO) != IDYES)
        return;

    // Commit to importing — synchronous guard BEFORE the async worker.
    s_importing = true;
    EnableWindow(s_hImport, FALSE);
    _snprintf(s_importFolder, sizeof(s_importFolder), "%s", folder);
    s_importFolder[sizeof(s_importFolder)-1] = '\0';
    s_importForce = false;

    StartImportRun(hwnd);
}

// WM_APP_IMPORT_DONE handler: worker finished. Parse auto JSON, handle
// guard-refusal -> --force re-run, then refresh UI + re-enable.
static void OnImportDone(HWND hwnd) {
    // Close progress dialog + reap worker thread.
    if (s_hProgWnd) { DestroyWindow(s_hProgWnd); s_hProgWnd = NULL; }
    if (s_importThread) {
        WaitForSingleObject(s_importThread, INFINITE);
        CloseHandle(s_importThread);
        s_importThread = NULL;
    }

    char status[64] = "", message[2048] = "";
    JsonGetString(s_autoJson, "status", status, sizeof(status));
    JsonGetString(s_autoJson, "message", message, sizeof(message));

    bool cancelled = s_importCancelled;
    bool guardRefusal = (s_autoExit != 0) || (_stricmp(status, "error") == 0);

    if (cancelled) {
        // Re-enable; staging means the real mod is untouched.
        EnableWindow(hwnd, TRUE);
        SetMainControlsEnabled(TRUE);
        EnableWindow(s_hImport, TRUE);
        s_importing = false;
        MessageBoxA(hwnd, "Import cancelled. No changes were applied.", "Import cancelled",
                    MB_ICONINFORMATION | MB_OK);
        return;
    }

    if (guardRefusal && !s_importForce) {
        // Offer overwrite via --force.
        int r = MessageBoxA(hwnd,
            "This looks already-imported or modified.\n\nRe-import and overwrite?",
            "Re-import?", MB_ICONWARNING | MB_YESNO);
        if (r == IDYES) {
            s_importForce = true;
            StartImportRun(hwnd);   // re-run with --force; another WM_APP_IMPORT_DONE follows
            return;
        }
        // User declined — surface the message + bail.
        EnableWindow(hwnd, TRUE);
        SetMainControlsEnabled(TRUE);
        EnableWindow(s_hImport, TRUE);
        s_importing = false;
        char msg[2200];
        _snprintf(msg, sizeof(msg), "Import did not complete.\n\n%s",
                  message[0] ? message : "The importer refused the operation.");
        msg[sizeof(msg)-1] = '\0';
        MessageBoxA(hwnd, msg, "Import not applied", MB_ICONERROR | MB_OK);
        return;
    }

    if (guardRefusal) {
        // Already tried --force and still failed -> human error, no traceback.
        EnableWindow(hwnd, TRUE);
        SetMainControlsEnabled(TRUE);
        EnableWindow(s_hImport, TRUE);
        s_importing = false;
        char msg[2200];
        _snprintf(msg, sizeof(msg), "Import failed.\n\n%s",
                  message[0] ? message : "The importer reported an error.");
        msg[sizeof(msg)-1] = '\0';
        MessageBoxA(hwnd, msg, "Import failed", MB_ICONERROR | MB_OK);
        return;
    }

    // Success (ok / skipped / warn). Identify the newly imported campaign so we
    // can re-select it: first entry of mods_written if present.
    char written[256] = "";
    JsonGetString(s_autoJson, "mods_written", written, sizeof(written));
    // mods_written is a JSON array; JsonGetString won't grab it (no leading '"').
    // Fall back to parsing the first string element of the array.
    char firstMod[256] = "";
    {
        const char* p = strstr(s_autoJson, "\"mods_written\"");
        if (p) {
            const char* q = strchr(p, '[');
            if (q) {
                const char* s = strchr(q, '"');
                const char* lb = strchr(q, ']');
                if (s && (!lb || s < lb)) {
                    const char* e = strchr(s + 1, '"');
                    if (e) {
                        int len = (int)(e - (s + 1));
                        if (len > 0 && len < (int)sizeof(firstMod)) {
                            memcpy(firstMod, s + 1, len);
                            firstMod[len] = '\0';
                        }
                    }
                }
            }
        }
    }

    // Re-enable + repopulate in place (fixed slots — no destroy/recreate).
    EnableWindow(hwnd, TRUE);
    SetMainControlsEnabled(TRUE);
    EnableWindow(s_hImport, TRUE);
    s_importing = false;

    RescanAndRepopulate(firstMod[0] ? firstMod : NULL);

    char msg[2300];
    if (_stricmp(status, "warn") == 0) {
        char warnings[1024] = "";
        // Surface warnings array's first string if present.
        const char* p = strstr(s_autoJson, "\"warnings\"");
        if (p) {
            const char* q = strchr(p, '[');
            if (q) { const char* s = strchr(q, '"'); if (s) {
                const char* e = strchr(s + 1, '"');
                if (e) { int len = (int)(e - (s+1));
                    if (len > 0 && len < (int)sizeof(warnings)) { memcpy(warnings, s+1, len); warnings[len]='\0'; } } } }
        }
        _snprintf(msg, sizeof(msg), "Import completed with warnings.\n\n%s%s%s",
                  message[0] ? message : "Imported.",
                  warnings[0] ? "\n\nWarning: " : "", warnings);
    } else if (_stricmp(status, "skipped") == 0) {
        _snprintf(msg, sizeof(msg), "%s",
                  message[0] ? message : "Already imported and unchanged — nothing to do.");
    } else {
        _snprintf(msg, sizeof(msg), "%s", message[0] ? message : "Import complete.");
    }
    msg[sizeof(msg)-1] = '\0';
    MessageBoxA(hwnd, msg, "Import", MB_ICONINFORMATION | MB_OK);
}

// ---- Window procedure -----------------------------------------------------

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_DROPFILES: {
        HDROP hDrop = (HDROP)wParam;
        char droppedPath[MAX_PATH] = {};
        DragQueryFileA(hDrop, 0, droppedPath, sizeof(droppedPath));
        DragFinish(hDrop);
        const char* ext = strrchr(droppedPath, '.');
        if (ext && _stricmp(ext, ".exe") == 0)
            TryInstallModPack(hwnd, droppedPath);
        break;
    }

    case WM_CREATE: {
        s_hMainWnd = hwnd;
        DragAcceptFiles(hwnd, TRUE);
        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        // ---- LEFT COLUMN: campaign label + Import button + campaign listbox +
        //      compatibility-base radios beneath it ----
        HWND hLabel = CreateWindowA("STATIC", "Campaign (pick one):",
            WS_CHILD | WS_VISIBLE, LP_LIST_X, LP_RLBL_Y, 220, LP_LBL_H,
            hwnd, (HMENU)IDC_LABEL, s_hInst, NULL);
        SendMessageA(hLabel, WM_SETFONT, (WPARAM)hFont, TRUE);

        s_hImport = CreateWindowA("BUTTON", "Import...",
            WS_CHILD | WS_VISIBLE, LP_LIST_X + LP_LIST_W - 90, 8, 90, 26,
            hwnd, (HMENU)IDC_IMPORT, s_hInst, NULL);
        SendMessageA(s_hImport, WM_SETFONT, (WPARAM)hFont, TRUE);

        s_hListBox = CreateWindowA("LISTBOX", NULL,
            WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
            LP_LIST_X, LP_LIST_Y, LP_LIST_W, LP_LIST_H, hwnd, (HMENU)IDC_MODLIST, s_hInst, NULL);
        SendMessageA(s_hListBox, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageA(s_hListBox, LB_ADDSTRING, 0, (LPARAM)"Stock (base game)");
        for (int i = 0; i < s_campCount; i++)
            SendMessageA(s_hListBox, LB_ADDSTRING, 0, (LPARAM)s_campaigns[i].name);
        SendMessageA(s_hListBox, LB_SETCURSEL, 0, 0);

        // "Compatibility Base (pick one):" label + radios (left column, under list).
        HWND hBaseLbl = CreateWindowA("STATIC", "Compatibility Base (pick one):",
            WS_CHILD | WS_VISIBLE, LP_LIST_X, LP_BASE_LBL_Y, LP_LIST_W, LP_LBL_H,
            hwnd, (HMENU)IDC_BASELBL, s_hInst, NULL);
        SendMessageA(hBaseLbl, WM_SETFONT, (WPARAM)hFont, TRUE);

        // FIXED base-radio slots: ONE exclusive group (first radio carries
        // WS_GROUP; BS_AUTORADIOBUTTON makes them mutually exclusive). Slot 0 is
        // always "None". Geometry never changes — refresh = show/hide + relabel.
        for (int i = 0; i < BASE_SLOTS; i++) {
            int by = LP_BASE_Y + i * LP_ROW_STEP;
            DWORD st = WS_CHILD | BS_AUTORADIOBUTTON;
            if (i == 0) st |= WS_GROUP;   // start of the exclusive radio group
            s_hBase[i] = CreateWindowA("BUTTON", "", st,
                LP_LIST_X, by, LP_LIST_W, LP_ROW_H,
                hwnd, (HMENU)(INT_PTR)(IDC_BASE0 + i), s_hInst, NULL);
            SendMessageA(s_hBase[i], WM_SETFONT, (WPARAM)hFont, TRUE);
        }

        // ---- RIGHT COLUMN: "Add-ons (stack any):" label + scrolling checkbox
        //      ListView + Faster-weapons toggle beneath it ----
        HWND hAddonLbl = CreateWindowA("STATIC", "Add-ons (stack any):",
            WS_CHILD | WS_VISIBLE, LP_RCOL_X, LP_RLBL_Y, LP_RCOL_W, LP_LBL_H,
            hwnd, (HMENU)IDC_ADDONLBL, s_hInst, NULL);
        SendMessageA(hAddonLbl, WM_SETFONT, (WPARAM)hFont, TRUE);

        // Add-on ListView: report mode + checkboxes, single (header-less) column.
        // Unbounded — scrolls when add-ons exceed the visible height. No fixed cap.
        s_hAddonList = CreateWindowExA(0, WC_LISTVIEWA, NULL,
            WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | WS_GROUP |
            LVS_REPORT | LVS_SINGLESEL | LVS_NOCOLUMNHEADER | LVS_SHOWSELALWAYS,
            LP_RCOL_X, LP_ADDON_LIST_Y, LP_RCOL_W, LP_ADDON_LIST_H,
            hwnd, (HMENU)(INT_PTR)IDC_ADDONLIST, s_hInst, NULL);
        SendMessageA(s_hAddonList, WM_SETFONT, (WPARAM)hFont, TRUE);
        ListView_SetExtendedListViewStyle(s_hAddonList,
            LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);
        {
            LVCOLUMNA col = {};
            col.mask = LVCF_WIDTH;
            col.cx = LP_RCOL_W - 4 - GetSystemMetrics(SM_CXVSCROLL);
            SendMessageA(s_hAddonList, LVM_INSERTCOLUMNA, 0, (LPARAM)&col);  // ...A: see InsertItem note
        }

        RefreshBaseSlots();    // show/hide + label per current scan; default None
        RefreshAddonList();    // populate ListView rows per current scan

        // Faster-weapons toggle — grouped under the add-ons pane. NOT a mod: it
        // drives engine env vars on Launch, separate from MC2_ACTIVE_MOD/
        // MC2_MOD_DEPS. WS_GROUP terminates the base radio group. Defaults OFF.
        s_hFasterWeapons = CreateWindowA("BUTTON", "Faster weapons (experimental)",
            WS_CHILD | WS_VISIBLE | WS_GROUP | BS_AUTOCHECKBOX,
            LP_RCOL_X, LP_FW_Y, LP_RCOL_W, LP_ROW_H,
            hwnd, (HMENU)(INT_PTR)IDC_FASTERWEAPONS, s_hInst, NULL);
        SendMessageA(s_hFasterWeapons, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageA(s_hFasterWeapons, BM_SETCHECK, BST_UNCHECKED, 0);  // default OFF

        // Tooltip for Faster Weapons: explain what the two env vars actually do.
        {
            HWND hTip = CreateWindowExA(0, TOOLTIPS_CLASSA, NULL,
                WS_POPUP | TTS_ALWAYSTIP | TTS_BALLOON,
                CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                hwnd, NULL, s_hInst, NULL);
            if (hTip) {
                TOOLINFOA ti = {}; ti.cbSize = sizeof(ti);
                ti.uFlags  = TTF_IDISHWND | TTF_SUBCLASS;
                ti.hwnd    = hwnd;
                ti.uId     = (UINT_PTR)s_hFasterWeapons;
                ti.lpszText = (LPSTR)"Projectiles fly straight (no arc) and travel at 2x speed.\r\n"
                                     "Affects all weapons globally. Experimental — may unbalance\r\n"
                                     "some missions or affect AI targeting.";
                SendMessageA(hTip, TTM_ADDTOOLA, 0, (LPARAM)&ti);
                SendMessageA(hTip, TTM_SETMAXTIPWIDTH, 0, 260);
            }
        }

        // ---- DESCRIPTION (full width, below both columns) ----
        HWND hDescLbl = CreateWindowA("STATIC", "Description:",
            WS_CHILD | WS_VISIBLE, LP_MARGIN, LP_DESC_LBL_Y, 220, LP_LBL_H,
            hwnd, (HMENU)(INT_PTR)-1, s_hInst, NULL);
        SendMessageA(hDescLbl, WM_SETFONT, (WPARAM)hFont, TRUE);

        // Multiline read-only pane. Updates on campaign / base / add-on focus.
        s_hDescBox = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
            LP_MARGIN, LP_DESC_Y, LP_CLIENT_W - 2 * LP_MARGIN, LP_DESC_H,
            hwnd, (HMENU)IDC_DESC, s_hInst, NULL);
        SendMessageA(s_hDescBox, WM_SETFONT, (WPARAM)hFont, TRUE);

        // ---- AUX buttons (Engine Options / Graphics) ----
        HWND hOptBtn = CreateWindowA("BUTTON", "Engine Options...",
            WS_CHILD | WS_VISIBLE,
            LP_MARGIN, LP_AUX_BTN_Y, 150, LP_AUX_BTN_H,
            hwnd, (HMENU)(INT_PTR)IDC_OPTIONS, s_hInst, NULL);
        SendMessageA(hOptBtn, WM_SETFONT, (WPARAM)hFont, TRUE);

        HWND hGfxBtn = CreateWindowA("BUTTON", "Graphics...",
            WS_CHILD | WS_VISIBLE,
            LP_MARGIN + 160, LP_AUX_BTN_Y, 150, LP_AUX_BTN_H,
            hwnd, (HMENU)(INT_PTR)IDC_GRAPHICS, s_hInst, NULL);
        SendMessageA(hGfxBtn, WM_SETFONT, (WPARAM)hFont, TRUE);

        // ---- BOTTOM: Launch / Cancel ----
        int btnW = (LP_CLIENT_W - 2 * LP_MARGIN - 10) / 2;   // two buttons + 10px gap
        HWND hLaunch = CreateWindowA("BUTTON", "Launch",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, LP_MARGIN, LP_BTN_Y, btnW, LP_BTN_H,
            hwnd, (HMENU)IDC_LAUNCH, s_hInst, NULL);
        SendMessageA(hLaunch, WM_SETFONT, (WPARAM)hFont, TRUE);
        HWND hCancel = CreateWindowA("BUTTON", "Cancel",
            WS_CHILD | WS_VISIBLE, LP_MARGIN + btnW + 10, LP_BTN_Y, btnW, LP_BTN_H,
            hwnd, (HMENU)IDC_CANCEL, s_hInst, NULL);
        SendMessageA(hCancel, WM_SETFONT, (WPARAM)hFont, TRUE);

        UpdateForSelection();
        break;
    }

    case WM_NOTIFY: {
        LPNMHDR nh = (LPNMHDR)lParam;
        if (nh && nh->idFrom == IDC_ADDONLIST && nh->code == LVN_ITEMCHANGED) {
            LPNMLISTVIEW lv = (LPNMLISTVIEW)lParam;
            // Update the description pane when the focused row changes.
            if (lv->uChanged & LVIF_STATE && (lv->uNewState & LVIS_FOCUSED))
                DescribeAddon(lv->iItem);
        }
        break;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_MODLIST && HIWORD(wParam) == LBN_SELCHANGE)
            UpdateForSelection();
        // Base radio clicked: show that base's description.
        if (LOWORD(wParam) >= IDC_BASE0 && LOWORD(wParam) < IDC_BASE0 + BASE_SLOTS &&
            HIWORD(wParam) == BN_CLICKED)
            DescribeBase((int)LOWORD(wParam) - IDC_BASE0);
        if (LOWORD(wParam) == IDC_IMPORT && HIWORD(wParam) == BN_CLICKED)
            OnImportButton(hwnd);
        if (LOWORD(wParam) == IDC_OPTIONS && HIWORD(wParam) == BN_CLICKED)
            ShowOptionsDialog(hwnd);
        if (LOWORD(wParam) == IDC_GRAPHICS && HIWORD(wParam) == BN_CLICKED)
            ShowGraphicsDialog(hwnd);
        if (LOWORD(wParam) == IDC_LAUNCH ||
            (LOWORD(wParam) == IDC_MODLIST && HIWORD(wParam) == LBN_DBLCLK)) {
            if (!s_importing) DoLaunch(hwnd);
        }
        if (LOWORD(wParam) == IDC_CANCEL) {
            if (!s_importing) {
                SaveMainPageSelections();
                PostQuitMessage(0);
            }
        }
        break;

    case WM_APP_IMPORT_DONE:
        OnImportDone(hwnd);
        break;

    case WM_CLOSE:
        if (s_importing) return 0;   // block close during import
        SaveMainPageSelections();
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProcA(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// ---- Entry point ----------------------------------------------------------

// Headless diagnostic: print discovered campaigns + auto-detected compat and the
// compat layers, then exit. Invoked with "--list" (no GUI). For verification/CI.
static void PrintListing() {
    FILE* f = fopen("launcher_list.txt", "w");
    if (!f) return;
    fprintf(f, "campaigns (%d):\n", s_campCount);
    for (int i = 0; i < s_campCount; i++) {
        const char* k = s_campaigns[i].needs == CK_MCO ? "MCO -> mco-compat"
                      : s_campaigns[i].needs == CK_MC2X ? "MC2X -> mc2x-compat"
                      : "unknown (none auto)";
        fprintf(f, "  %-24s  %s\n", s_campaigns[i].folderName, k);
    }
    fprintf(f, "bases (%d) [radio, mutually exclusive]:\n", s_baseCount);
    for (int i = 0; i < s_baseCount; i++)
        fprintf(f, "  %s\n", s_bases[i].folderName);
    fprintf(f, "add-ons (%d) [checkbox, stackable]:\n", s_addonCount);
    for (int i = 0; i < s_addonCount; i++)
        fprintf(f, "  %s\n", s_addons[i].folderName);
    fprintf(f, "texture-quality (%d) [radio, mutually exclusive]:\n", s_texQualCount);
    for (int i = 0; i < s_texQualCount; i++)
        fprintf(f, "  %s\n", s_texQual[i].folderName);
    fclose(f);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR lpCmd, int) {
    s_hInst = hInst;

    GetModuleFileNameA(NULL, s_launcherDir, sizeof(s_launcherDir));
    char* ls = strrchr(s_launcherDir, '\\');
    if (!ls) ls = strrchr(s_launcherDir, '/');
    if (ls) *(ls+1) = '\0'; else strcpy(s_launcherDir, ".\\");

    _snprintf(s_modsPath, sizeof(s_modsPath), "%smods\\", s_launcherDir);
    s_modsPath[sizeof(s_modsPath)-1] = '\0';

    // Headless diagnostic: skip single-instance guard and GUI entirely.
    if (lpCmd && strstr(lpCmd, "--list")) {
        EnvVarsSetDefaults(); GfxSetDefaults();
        _snprintf(s_envJsonPath, sizeof(s_envJsonPath), "%slauncher_env.json", s_launcherDir);
        s_envJsonPath[sizeof(s_envJsonPath)-1] = '\0';
        LoadEnvJson(); LoadGfxJson(); LoadMainPageJson();
        ScanMods(s_modsPath);
        // Eagerly resolve compat kind for all campaigns (deferred from ScanMods for GUI speed).
        for (int i = 0; i < s_campCount; i++) {
            if (s_campaigns[i].needs == CK_PENDING) {
                char modDir[MAX_PATH];
                _snprintf(modDir, sizeof(modDir), "%s%s", s_modsPath, s_campaigns[i].folderName);
                modDir[sizeof(modDir)-1] = '\0';
                s_campaigns[i].needs = DetectCompat(modDir);
            }
        }
        PrintListing();
        return 0;
    }

    // Single-instance guard: if a launcher window already exists, raise it and exit.
    // Without this, slow mod-scanning means multiple instances queue up before the
    // first window appears, resulting in N windows popping at once.
    HANDLE hMutex = CreateMutexA(NULL, TRUE, "MC2LauncherSingleInstance");
    if (hMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND hExist = FindWindowA("MC2LauncherWnd", NULL);
        if (hExist) {
            ShowWindow(hExist, SW_RESTORE);
            SetForegroundWindow(hExist);
        }
        CloseHandle(hMutex);
        return 0;
    }
    // hMutex held open for the lifetime of this process — do not CloseHandle.

    // Env-var options: set defaults then load persisted values from launcher_env.json.
    _snprintf(s_envJsonPath, sizeof(s_envJsonPath), "%slauncher_env.json", s_launcherDir);
    s_envJsonPath[sizeof(s_envJsonPath)-1] = '\0';
    EnvVarsSetDefaults();
    GfxSetDefaults();
    LoadEnvJson();
    LoadGfxJson();
    LoadMainPageJson();

    ScanMods(s_modsPath);

    // Resolve the saved texture-pack folder name to an index now that s_texQual[] is populated.
    if (s_gfxTexPackSaved[0]) {
        for (int i = 0; i < s_texQualCount; i++) {
            if (_stricmp(s_texQual[i].folderName, s_gfxTexPackSaved) == 0) {
                s_gfxTexPackIdx = i + 1;
                break;
            }
        }
    }

    // Always show the GUI. (Previously, "no campaigns discovered" launched the base game
    // directly and skipped the launcher entirely — which hid the GUI and its Import button
    // on a fresh install with no mods/, exactly the shipped-release case where users import
    // their own campaigns. The campaign listbox already includes "Stock campaign" as slot 0,
    // so stock is a single Launch click and community campaigns are added via Import.)

    // Init common controls (marquee progress bar) + COM (SHBrowseForFolder).
    INITCOMMONCONTROLSEX icc = {}; icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);
    CoInitialize(NULL);

    WNDCLASSA wc = {};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor       = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.lpszClassName = "MC2LauncherWnd";
    RegisterClassA(&wc);

    // Size the window to fit the two-column layout (must match WM_CREATE geometry):
    //   LEFT col: campaign listbox + base radios beneath; RIGHT col: add-on
    //   ListView + faster-weapons toggle; then full-width Description box, the
    //   Engine/Graphics aux buttons, and Launch/Cancel. Height clears LP_BTN_Y.
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    int clientW = LP_CLIENT_W;                           // 10+320+15+230+10 = 585
    int clientH = LP_BTN_Y + LP_BTN_H + LP_MARGIN;       // bottom button row + margin
    RECT rc = { 0, 0, clientW, clientH };
    AdjustWindowRect(&rc, style, FALSE);
    int wW = rc.right - rc.left;
    int wH = rc.bottom - rc.top;
    int sx = GetSystemMetrics(SM_CXSCREEN);
    int sy = GetSystemMetrics(SM_CYSCREEN);
    HWND hwnd = CreateWindowA("MC2LauncherWnd",
        "MechCommander 2 OpenGL - Launcher",
        style, (sx - wW) / 2, (sy - wH) / 2, wW, wH,
        NULL, NULL, hInst, NULL);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    // Restore last-used main-page selections (campaign, base, add-ons, faster-weapons).
    // WM_CREATE has already built the fixed slots; restore runs after so all HWNDs exist.
    RestoreMainPageSelections();

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0)) {
        // Route keyboard messages (Tab, Enter, Escape) to the modeless options
        // dialog when it is open, so IsDialogMessage handles focus traversal.
        if (s_hOptionsDlg   && IsDialogMessage(s_hOptionsDlg,   &msg)) continue;
        if (s_hGraphicsDlg  && IsDialogMessage(s_hGraphicsDlg,  &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    CoUninitialize();
    return 0;
}
