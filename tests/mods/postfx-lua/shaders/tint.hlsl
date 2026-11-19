// The frame blended toward Tint.rgb by Tint.a
sampler2D Scene : register(s0);
float4 Tint : register(c1);

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float4 c = tex2D(Scene, uv);
    return float4(lerp(c.rgb, Tint.rgb, Tint.a), 1);
}
