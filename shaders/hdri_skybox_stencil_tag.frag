// SKYBOX-FOG-EXCLUDE-1: stencil-only tag pass, drawn immediately after the
// normal HDRI skybox color draw (renderHdriSkybox), gated MC2_SKYBOX_FOG_EXCLUDE.
//
// Purpose: mark stencil=1 for pixels the fog passes must treat as "true sky,
// do not fog" -- the same DEEP-SKY region above fog_oob.frag's cloud band
// top, expressed in THIS shader's frame (elevation on worldDir.y; see the
// SKYBOX-FOG-EXCLUDE-2 fix note in main -- v1 wrongly reused fog_oob's
// worldDir.z test across mismatched frames and tagged an azimuth wedge).
// Pixels inside/below that band (near-horizon and the OOB void) are left
// untouched (discard) so their stencil stays at the frame-clear value (0);
// the fog passes still apply there, matching current behavior. Pixels above
// the band (deep sky) are tagged 1 so the fog passes exclude them (feathered
// on the fog side -- see fog_oob.frag / edge_fog.frag).
//
// Color writes are masked off by the caller (glColorMask all FALSE) --
// this pass only affects the stencil buffer. Same worldDir reconstruction
// as hdri_skybox.frag's default (frameFix==0) path: invProj + invViewRot.
// Must be kept in sync with that path; frameFix variants (1/2) are dead
// call sites (not reachable from the live frame loop) so they are
// intentionally not mirrored here.

in vec2 vNdc;

layout(location = 0) out vec4 FragColor;  // unused: color writes masked off

uniform mat4  invProj;
uniform mat3  invViewRot;
// (skyYaw uniform REMOVED in SKYBOX-FOG-EXCLUDE-2: the sun-sync yaw only
// remaps azimuth; the elevation test below is yaw-invariant. The C++ side
// no longer uploads it.)

void main()
{
    vec4 clip = vec4(vNdc, 1.0, 1.0);
    vec3 viewDir = (invProj * clip).xyz;
    vec3 worldDir = normalize(invViewRot * viewDir);

    // SKYBOX-FOG-EXCLUDE-2 FIX (split-sky vertical-edged dark mass): v1
    // copied fog_oob.frag's "worldDir.z < -0.22" test verbatim, but the two
    // shaders reconstruct worldDir in DIFFERENT frames. fog_oob unprojects
    // through inverse(worldToClipGL) where z carries (negated) ELEVATION;
    // this pass uses the skybox's invProj+invViewRot frame, where elevation
    // is on worldDir.y (the color pass samples equirect with asin(worldDir.y);
    // +y = zenith) and worldDir.z is a HORIZONTAL azimuth axis -- further
    // rotated by the sun-sync skyYaw. So v1 tagged an AZIMUTH WEDGE of the
    // sky (vertical screen edges, position set by camera yaw + sun sync),
    // hard-excluding the sea-of-clouds fog inside the wedge: the user-visible
    // vertical-edged dark mass. Correct test: ELEVATION in this frame.
    // fog frame z < -0.22 (deep sky) <=> elevation > 0.22 <=> worldDir.y > 0.22.
    // At/below that band top: leave stencil alone (discard) so fog still
    // applies (horizon band + OOB void unchanged).
    if (worldDir.y <= 0.22) {
        discard;
    }

    FragColor = vec4(0.0);
}
