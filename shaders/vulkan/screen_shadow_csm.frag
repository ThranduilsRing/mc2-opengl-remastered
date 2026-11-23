// Unified object receiver: one static D16 gather and the moverVisibility seam.
#version 450
#extension GL_GOOGLE_include_directive : require
#include "../include/axis.hglsl"
#include "include/mover_shadow.glsl"
layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;
layout(set=0,binding=0) uniform sampler2D sceneDepthTex;
layout(set=0,binding=1) uniform sampler2D sceneNormalTex;
layout(set=0,binding=2) uniform sampler2DShadow shadowMap;
layout(std140,set=0,binding=3) uniform ScreenShadowParams {
    mat4 inverseViewProj;
    mat4 lightSpaceMatrix;
    int enableShadows;
    int enableDynamicShadows;
    float mechSoft;
    int debugMode;
} u;
float objectStaticVisibility(vec3 worldPos) {
    vec4 p=u.lightSpaceMatrix*vec4(worldPos,1.0);
    vec3 q=p.xyz/p.w; q.xy=q.xy*0.5+0.5;
    if (q!=clamp(q,0.0,1.0)) return 1.0;
    vec4 taps=textureGather(shadowMap,q.xy,q.z-3.0/65535.0);
    vec2 f=fract(q.xy*vec2(textureSize(shadowMap,0))-0.5);
    float shadow = mix(mix(taps.w,taps.z,f.x),mix(taps.x,taps.y,f.x),f.y);
    return mix(sunShadowFloor(0.4), 1.0, shadow);
}
bool rc_pixelHandlesOwnShadow(vec4 gbuf1) { return gbuf1.a>0.5; }
bool rc_isStaticPropNoSelfShadow(vec4 gbuf1) { return gbuf1.a>0.1 && gbuf1.a<=0.5; }
void main() {
    vec4 normalData=texture(sceneNormalTex,TexCoord);
    float depth=texture(sceneDepthTex,TexCoord).r;
    if (depth<=0.0001 || rc_pixelHandlesOwnShadow(normalData)) { FragColor=vec4(1.0); return; }
    vec4 p=u.inverseViewProj*vec4(TexCoord*2.0-1.0,depth,1.0);
    vec3 worldPos=p.xyz/p.w;
    bool prop=rc_isStaticPropNoSelfShadow(normalData);
    // Props store Stuff-space normals; skinned mechs already write MC2 normals.
    // Converting a mech twice suppresses body reception through mechSoft.
    vec3 decodedNormal=normalize(normalData.rgb*2.0-1.0);
    vec3 N=prop?mc2ToRender(decodedNormal):decodedNormal;
    float shadow=u.enableShadows!=0 && !prop?objectStaticVisibility(worldPos):1.0;
    if (u.enableDynamicShadows!=0) {
        float v=moverVisibilityEx(worldPos,N,prop?1.0:0.0);
        if (!prop && u.mechSoft>0.0)
            v=mix(1.0,v,smoothstep(0.0,0.15+0.30*u.mechSoft,max(dot(N,moverFrame.sun.xyz),0.0)));
        float moverShadow = v;
        shadow = min(shadow, mix(moverShadowFloor(prop ? 0.4 : 0.55), 1.0, moverShadow));
    }
    FragColor=vec4(vec3(shadow),1.0);
}
