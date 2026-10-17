// Test fixture only (not a shipped shader). Set 0: b0 UBO, b1 texture, b2 sampler.
struct Params { float4 tint; };
[[vk::binding(0, 0)]] ConstantBuffer<Params> params;
[[vk::binding(1, 0)]] Texture2D tex;
[[vk::binding(2, 0)]] SamplerState samp;

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

[shader("fragment")]
float4 main(VSOut i) : SV_Target0 { return tex.Sample(samp, i.uv) * params.tint; }
