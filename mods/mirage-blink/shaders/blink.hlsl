// Blink's lens pass (render/postfx.h): the view gathers while aiming, tears toward the aim point in travel and settles
// through the cooldown. The aim point stays sharp; everything grows toward the edges.
//
//   c1: warm-up (aiming, 0-1), travel (0-1), cooldown (1 to 0), wobble (a decaying shake after arriving)
sampler2D Scene : register(s0);
float4 Frame : register(c0);        // seconds, width, height, width / height
float4 Blink : register(c1);

float Hash(float n) { return frac(sin(n * 12.9898) * 43758.5453); }

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    const float t = Frame.x;
    const float warm = Blink.x, travel = Blink.y, cool = Blink.z, wobble = Blink.w;
    const float2 d = uv - 0.5;
    const float r = length(float2(d.x * Frame.w, d.y));
    const float edge = smoothstep(0.1, 0.7, r);             // 0 at the aim point
    const float angle = atan2(d.y, d.x);
    const float2 radial = d / max(length(d), 1e-4);

    // The edges ripple: slow rings that drift outward, stronger in travel and in the wobble after it
    const float ripple = sin(angle * 6.0 + t * 7.0) * sin(r * 22.0 - t * 6.0);
    const float2 bent = uv + radial * ripple * (0.005 * warm + 0.018 * travel + 0.012 * wobble) * edge;

    // A radial blur toward the aim point, with red and blue pulled apart along the radius
    const float blur = (0.02 * warm + 0.16 * travel + 0.04 * cool) * edge;
    const float split = (0.003 * warm + 0.01 * travel + 0.002 * wobble) * edge;
    float3 c = 0;
    [unroll] for (int i = 0; i < 12; i++) {
        const float2 s = 0.5 + (bent - 0.5) * (1.0 - blur * (i / 11.0));
        c.r += tex2D(Scene, s + radial * split).r;
        c.g += tex2D(Scene, s).g;
        c.b += tex2D(Scene, s - radial * split).b;
    }
    c /= 12.0;

    // A dark tunnel closing around the aim, a cold cast at the edges while gathering
    c *= 1.0 - smoothstep(0.22, 0.85, r) * saturate(0.4 * warm + 0.8 * travel + 0.3 * cool);
    c = lerp(c, c * float3(0.78, 0.94, 1.18), warm * edge * 0.55);

    // Through the cooldown: thin streaks rushing outward past the view
    const float lane = floor((angle + 3.14159) * 48.0 / 6.28318);
    const float bright = pow(Hash(lane), 18.0);
    const float rush = frac(r * 2.5 - t * 2.2 + Hash(lane + 7.0));
    c += bright * smoothstep(0.9, 1.0, rush) * smoothstep(0.25, 0.6, r) * cool * 1.2;
    return float4(c, 1.0);
}
