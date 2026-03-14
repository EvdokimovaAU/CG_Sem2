cbuffer PerObjectCB : register(b0)
{
    float4x4 World;
    float4x4 View;
    float4x4 Proj;
    float4 UVTransform;
    float4 TimeParams;
};

Texture2D gTex : register(t0);
SamplerState gSampler : register(s0);

struct VSInput
{
    float3 Pos : POSITION;
    float3 Norm : NORMAL;
    float2 UV : TEXCOORD0;
};

struct PSInput
{
    float4 PosH : SV_POSITION;
    float3 NormW : TEXCOORD1;
    float2 UV : TEXCOORD0;
};

PSInput VSMain(VSInput input)
{
    PSInput o;

    float4 posW = mul(float4(input.Pos, 1.0f), World);
    float4 posV = mul(posW, View);
    o.PosH = mul(posV, Proj);

    float3x3 W3 = (float3x3)World;
    o.NormW = normalize(mul(W3, input.Norm));
    o.UV = input.UV;

    return o;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float3 N = normalize(input.NormW);
    float3 Ldir = normalize(float3(-0.4f, -1.0f, -0.2f));
    float ndotl = saturate(dot(N, -Ldir));

    float2 uv = input.UV * UVTransform.xy + UVTransform.zw;
    float4 albedo = gTex.Sample(gSampler, uv);

    float3 ambient = float3(0.30f, 0.30f, 0.30f);
    float3 diffuse = float3(1.00f, 0.95f, 0.85f) * ndotl;
    float3 litLinear = (ambient + diffuse) * albedo.rgb;
    float3 litSRGB = pow(saturate(litLinear), 1.0f / 2.2f);

    return float4(litSRGB, albedo.a);
}
