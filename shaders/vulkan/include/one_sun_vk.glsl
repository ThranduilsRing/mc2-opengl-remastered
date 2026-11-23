#ifndef MC2_ONE_SUN_VK_GLSL
#define MC2_ONE_SUN_VK_GLSL
// One sun (S4). Needs uEnv (include/environment_ubo_vk.glsl) in scope.
//   Live sun (uEnv.sunDirLive): shadow maps, cloud shadows, water glint position.
//   Authored sun (sunDirLive rotated by sunAuthFromLive, plus pitchUndo / ambRelight / sunRelight): the shading of props,
//   crowns, mechs and vehicles, so they move by the same colour ratio as the ground. A pitch key changes shadows, not
//   face shading, until the ground can be unbaked (S6).
// With no keys, at night and with no mission every term is exact identity / 1.0, so no shader branches on it.
mat3 oneSunAuthFromLive() {
    return mat3(uEnv.sunAuthFromLive[0].xyz, uEnv.sunAuthFromLive[1].xyz, uEnv.sunAuthFromLive[2].xyz);
}
#endif
