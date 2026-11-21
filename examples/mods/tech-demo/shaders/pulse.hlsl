sampler2D Scene : register(s0);
float4 Pulse : register(c1);

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float4 scene = tex2D(Scene, uv);
    float2 centered = uv * 2.0 - 1.0;
    float edge = saturate(dot(centered, centered));
    float stripe = 1.0 - smoothstep(0.015, 0.075, abs(uv.y - Pulse.y));
    float intensity = saturate((edge * 0.45 + stripe * 0.65) * Pulse.x);
    return float4(lerp(scene.rgb, float3(0.05, 0.78, 1.0), intensity), scene.a);
}
