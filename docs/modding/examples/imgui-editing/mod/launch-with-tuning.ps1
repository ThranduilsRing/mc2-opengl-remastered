# mods/imgui-tuning-demo/launch-with-tuning.ps1
# -------------------------------------------------------------------------
# Run THIS instead of mc2.exe (or the launcher) to actually see this mod's
# visual_tuning.json take effect.
#
# Why this script exists: GameOS/gameos/visual_tuning_profile.cpp loads
# "data/visual_tuning.json" with a raw fopen() against the process's working
# directory -- it does NOT go through the mod-aware VFS resolver that every
# other data/ asset uses, and mc2.exe never chdirs into a mod folder. So
# picking this mod in the launcher (or setting only $env:MC2_ACTIVE_MOD) is
# NOT enough: the engine will keep reading the BASE deploy's
# data/visual_tuning.json and silently ignore this file. See this folder's
# README.md, Part 1.3, for the full explanation.
#
# Ships inside mods/imgui-tuning-demo/ (next to mod.json and data/), NOT
# under data/ itself -- env vars aren't moddable "data/" content, this is
# just a convenience launcher.
#
# Usage: double-click this script from inside your installed
#   <deploy>/mods/imgui-tuning-demo/ folder, or run it from PowerShell.
#   $PSScriptRoot resolves to that folder, so paths below work as long as
#   this script stays where mc2mod.py install puts it.

$env:MC2_ACTIVE_MOD        = "imgui-tuning-demo"
$env:MC2_VISUAL_TUNING_FILE = "$PSScriptRoot\data\visual_tuning.json"

& "$PSScriptRoot\..\..\mc2.exe"
