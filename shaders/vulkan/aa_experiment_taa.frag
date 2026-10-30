#version 450
// DEV ONLY: final encoded scene, before HUD. Delete with the owner experiment.
layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 outColor;
layout(set=0,binding=0) uniform sampler2D sceneTex;
layout(set=0,binding=1) uniform sampler2D historyTex;
layout(set=0,binding=2) uniform sampler2D motionTex;
layout(push_constant) uniform Params { vec4 metrics; } aa;
void main() {
    vec3 current = texture(sceneTex, TexCoord).rgb;
    vec3 lo = current, hi = current;
    for (int y=-1; y<=1; ++y) for (int x=-1; x<=1; ++x) {
        vec3 c = texture(sceneTex, TexCoord + vec2(x,y)*aa.metrics.xy).rgb;
        lo = min(lo,c); hi = max(hi,c);
    }
    // Motion is previous UV minus current UV (includes object corrections).
    vec2 motion = texture(motionTex, TexCoord).xy;
    vec2 previousUV = TexCoord + motion;
    vec3 history = texture(historyTex, previousUV).rgb;
    // Reject exposed surfaces by neighborhood disagreement; clamp remaining history.
    float disagreement = max(max(max(lo.r-history.r, history.r-hi.r), max(lo.g-history.g, history.g-hi.g)), max(lo.b-history.b, history.b-hi.b));
    bool valid = aa.metrics.w > 0.5 && all(greaterThanEqual(previousUV,vec2(0))) &&
                 all(lessThanEqual(previousUV,vec2(1))) && length(motion) < 0.1;
    float weight = valid ? 0.9 * (1.0-smoothstep(0.02,0.15,disagreement)) : 0.0;
    outColor = vec4(mix(current, clamp(history,lo,hi),weight),1);
}
