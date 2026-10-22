#pragma once
// True only while rendering a completed snapshot. Gameplay state is read-only
// in this phase; legacy appearance methods select their private render copy.
extern bool simPresentationPass;
