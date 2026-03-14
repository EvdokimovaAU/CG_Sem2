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
    float3 WorldPos : TEXCOORD0;
    float3 NormalW : TEXCOORD1;
    float2 UV : TEXCOORD2;
    float ViewDepth : TEXCOORD3;
};

struct GBufferOutput
{
    float4 AlbedoSpec : SV_Target0;
    float4 WorldPos : SV_Target1;
    float4 Normal : SV_Target2;
    float4 Depth : SV_Target3;
};

PSInput VSMain(VSInput input)
{
    PSInput o;

    float4 posW = mul(float4(input.Pos, 1.0f), World);
    float4 posV = mul(posW, View);
    o.PosH = mul(posV, Proj);
    o.WorldPos = posW.xyz;
    o.ViewDepth = posV.z;

    float3x3 W3 = (float3x3)World;
    o.NormalW = normalize(mul(W3, input.Norm));
    o.UV = input.UV;

    return o;
}

GBufferOutput PSMain(PSInput input)
{
    GBufferOutput o;

    float2 uv = input.UV * UVTransform.xy + UVTransform.zw;
    float4 albedo = gTex.Sample(gSampler, uv);
    float3 normal = normalize(input.NormalW);
    float depth = saturate(input.ViewDepth / 20000.0f);

    o.AlbedoSpec = albedo;
    o.WorldPos = float4(input.WorldPos, 1.0f);
    o.Normal = float4(normalize(normal) * 0.5f + 0.5f, 1.0f);
    o.Depth = float4(depth, depth, depth, 1.0f);

    return o;
}
