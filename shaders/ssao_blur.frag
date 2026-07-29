//#version 430 (version provided by prefix)
//
// SSAO-BLUR-1. Depth-aware (joint bilateral) blur over the half-res AO buffer.
//
// WHY THIS EXISTS: ssao.frag integrates a 16-sample hemisphere with a per-pixel
// hash12 rotation of the TBN basis. That is a stochastic estimator -- its output
// is signal + per-pixel noise, and the noise amplitude GROWS with aoRadius
// (a larger radius spreads the taps across more depth variation, so neighbouring
// pixels disagree more). Before this pass the ONLY denoise in the chain was a
// 4-tap box spanning +/-0.5 of a half-res texel inside ssao_apply.frag, whose
// comment claimed it "denoises". It cannot: a 1-texel box averages 4 samples of
// an estimator whose rotation decorrelates every pixel, so it removes almost no
// variance. That is why raising aoRadius to a useful value (SSAO-RETUNE-1)
// produced visible speckled dither on FLAT faces -- faces where nothing occludes
// -- rather than clean contact shading.
//
// SEPARABLE: run twice, horizontal then vertical, via the blurDir uniform. Two
// 9-tap passes give a 9x9 effective support for 18 taps instead of 81, at HALF
// resolution (a quarter of the full-res pixels), which is roughly an order of
// magnitude cheaper than doing an equivalent joint-bilateral gather at full res
// inside ssao_apply.
//
// DEPTH-AWARE: a plain Gaussian would bleed occlusion across silhouettes --
// dragging a building's contact darkening out onto the tarmac behind it, and
// smearing the sky edge. Each tap is therefore weighted by how close its scene
// depth is to the centre pixel's, so the blur averages only within a surface and
// stops hard at depth discontinuities.
//
// Depth convention matches ssao.frag / shadow_screen.frag: reverse-Z, far=0,
// near=1, larger=closer, sky/cleared ~0.0 (NOT 1.0).

in vec2 TexCoord;
layout(location = 0) out vec4 FragColor;

uniform sampler2D ssaoTex;        // unit 0: half-res AO (the buffer being blurred)
uniform sampler2D sceneDepthTex;  // unit 1: full-res scene depth (edge-stopping)
uniform vec2  blurDir;            // half-res texel step: (1/w,0) then (0,1/h)
uniform float depthSigma;         // RELATIVE depth tolerance (fraction of centre depth)

// 9-tap Gaussian (sigma ~2 texels), normalised. Weights are re-normalised at
// runtime anyway once bilateral rejection kicks in, so these are shape only.
const float kW[5] = float[](0.2270270270, 0.1945945946, 0.1216216216,
                            0.0540540541, 0.0162162162);

void main()
{
    float centreDepth = texture(sceneDepthTex, TexCoord).r;

    // Sky / cleared: ssao.frag wrote AO = 1 here and the scene must stay intact.
    // Passing it through untouched also stops sky depth (~0) from poisoning the
    // relative-depth weight below.
    if (centreDepth <= 0.0001) {
        FragColor = vec4(1.0);
        return;
    }

    float centreAO = texture(ssaoTex, TexCoord).r;
    float sum      = centreAO * kW[0];
    float wsum     = kW[0];

    for (int i = 1; i < 5; ++i) {
        vec2 off = blurDir * float(i);

        for (int s = 0; s < 2; ++s) {
            vec2  uv = (s == 0) ? (TexCoord + off) : (TexCoord - off);

            // Off-screen taps would clamp-sample the edge texel and bias the
            // result; drop them and let renormalisation handle it.
            if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) continue;

            float d = texture(sceneDepthTex, uv).r;
            if (d <= 0.0001) continue;            // sky tap: not part of this surface

            // Relative depth difference. Window depth under reverse-Z is highly
            // non-linear, so an ABSOLUTE epsilon would be far too permissive up
            // close and far too strict in the distance. Normalising by the centre
            // depth makes the tolerance behave like a constant fraction of view
            // distance across the whole depth range.
            float dz = abs(d - centreDepth) / centreDepth;
            float wz = exp(-(dz * dz) / (2.0 * depthSigma * depthSigma));

            float w = kW[i] * wz;
            sum  += texture(ssaoTex, uv).r * w;
            wsum += w;
        }
    }

    float ao = (wsum > 1e-5) ? (sum / wsum) : centreAO;
    FragColor = vec4(ao, ao, ao, 1.0);
}
