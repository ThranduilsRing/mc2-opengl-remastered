# tools/unit_converter/
#
# OFFLINE, deterministic Python tool. Reads STOCK MC2 unit source data
# (chassis CSV / vehicle .fit / per-unit tgl INI / compbas.csv) and EMITS
# sidecar `UnitDefinition` .fit files using the EXACT [UnitProfile] /
# [UIProfile] / [PresentationSockets] / hardpoint block+token format the
# engine loaders already parse (code/unitprofile_fit.cpp,
# mclib/mech3d.cpp). It does NOT touch the engine runtime or the read-only
# stock source; generated files land under a NEW output dir.
#
# It is a *converter* (emits files), distinct from the in-memory runtime
# facade. The legacy-vs-generated diff is not part of this tool.
