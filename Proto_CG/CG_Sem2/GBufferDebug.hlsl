Texture2D<float4> GAlbedoSpec : register(t0);
Texture2D<float4> GWorldPos : register(t1);
Texture2D<float4> GNormal : register(t2);
Texture2D<float4> GDepth : register(t3);

cbuffer DebugOverlayCB : register(b0)
{
    float4 OverlayRect;
    float4 SceneCenter;
    float4 SceneExtents;
    uint DebugMode;
    float3 Padding;
};

struct PSInput
{
    float4 Position : SV_POSITION;
    float2 UV : TEXCOORD0;
};

PSInput VSMain(uint vertexId : SV_VertexID)
{
    PSInput output;
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.Position = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    output.UV = uv;
    return output;
}

float3 VisualizeWorldPosition(float3 worldPos)
{
    float3 normalized = (worldPos - SceneCenter.xyz) / max(SceneExtents.xyz, float3(0.001f, 0.001f, 0.001f));
    return saturate(normalized * 0.5f + 0.5f);
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float2 uv = saturate(input.UV);
    float2 borderDistance = min(uv, 1.0f - uv);

    if (borderDistance.x < 0.015f || borderDistance.y < 0.015f)
    {
        return float4(0.95f, 0.95f, 0.95f, 1.0f);
    }

    uint texWidth = 0;
    uint texHeight = 0;
    GAlbedoSpec.GetDimensions(texWidth, texHeight);
    int2 texel = int2(min(uv * float2(texWidth, texHeight), float2(texWidth - 1, texHeight - 1)));

    if (DebugMode == 0)
    {
        return float4(GAlbedoSpec.Load(int3(texel, 0)).rgb, 1.0f);
    }

    if (DebugMode == 1)
    {
        float3 worldPos = GWorldPos.Load(int3(texel, 0)).xyz;
        return float4(VisualizeWorldPosition(worldPos), 1.0f);
    }

    if (DebugMode == 2)
    {
        return float4(GNormal.Load(int3(texel, 0)).rgb, 1.0f);
    }

    float depth = saturate(GDepth.Load(int3(texel, 0)).x);
    float depthVis = saturate(pow(depth, 0.35f));
    return float4(depthVis, depthVis, depthVis, 1.0f);
}
