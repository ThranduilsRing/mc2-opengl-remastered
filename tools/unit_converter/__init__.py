# tools/unit_converter/  (LEGACY-UNIT-CONVERTER-1, Phase-3 opener of the unit-profile arc)
#
# OFFLINE, deterministic Python tool. Reads STOCK MC2 unit source data
# (chassis CSV / vehicle .fit / per-unit tgl INI / compbas.csv) and EMITS
# sidecar `UnitDefinition` .fit files using the EXACT [UnitProfile] /
# [UIProfile] / [PresentationSockets] / hardpoint block+token format the
# Phase-2 engine loaders already parse (code/unitprofile_fit.cpp,
# mclib/mech3d.cpp). It does NOT touch the engine runtime or the read-only
# stock source; generated files land under a NEW output dir.
#
# It is a *converter* (emits files), distinct from the in-memory runtime
# LEGACY-UNIT-DEFINITION-SYNTHESIS-1 facade. The legacy-vs-generated diff is
# the NEXT slice (UNIT-DEFINITION-SHADOW-COMPARE-1) — NOT built here.
