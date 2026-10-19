#pragma once
// Stable declarations: git identity lives in one generated translation unit.
extern const char MC2_BUILD_GIT_SHA[];
extern const int MC2_BUILD_GIT_DIRTY;
extern const char MC2_BUILD_GIT_BRANCH[];
extern const char MC2_BUILD_TIME_ISO[];
extern const char MC2_BUILD_SRC_WORKTREE[];
extern const char MC2_BUILD_FP_LITERAL[];
void mc2PrintBuildFingerprint();
