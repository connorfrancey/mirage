// Does not compile, on purpose: the self-test reads the error
float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    return undefined_thing(uv);
}
