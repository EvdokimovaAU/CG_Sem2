Texture2D<float4> GAlbedoSpec : register(t0);
Texture2D<float4> GWorldPos   : register(t1);
Texture2D<float4> GNormal     : register(t2);
Texture2D<float4> GDepth      : register(t3);

SamplerState PointSampler  : register(s0);
SamplerState LinearSampler : register(s1);

struct PSInput
{
    float4 Position : SV_POSITION;
    float2 UV       : TEXCOORD0;
};

PSInput VSMain(uint vertexId : SV_VertexID)
{
    static const float2 kQuadUV[6] =
    {
        float2(0.0f, 0.0f),
        float2(1.0f, 0.0f),
        float2(0.0f, 1.0f),
        float2(0.0f, 1.0f),
        float2(1.0f, 0.0f),
        float2(1.0f, 1.0f)
    };

    PSInput output;
    float2 uv = kQuadUV[vertexId];
    output.Position = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    output.UV = uv;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float2 uv = saturate(input.UV);

    // Read source data from the G-buffer.
    float4 albedoSpec = GAlbedoSpec.Sample(PointSampler, uv);
    float3 worldPos = GWorldPos.Sample(PointSampler, uv).xyz;
    float3 normalEncoded = GNormal.Sample(PointSampler, uv).xyz;
    float depth = GDepth.Sample(PointSampler, uv).x;

    // Unpack the values used by the current renderer.
    float3 albedo = albedoSpec.rgb;
    float roughness = albedoSpec.a;
    float3 normal = normalize(normalEncoded * 2.0f - 1.0f);

    // Example early out for background pixels.
    if (depth >= 1.0f)
    {
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    }

    // Temporary debug output:
    // return float4(albedo, 1.0f);
    // return float4(normal * 0.5f + 0.5f, 1.0f);
    // return float4(depth.xxx, 1.0f);
    // return float4(frac(worldPos * 0.05f), 1.0f);

    // Replace this block with your post-processing logic.
    float3 color = albedo;
    color *= 0.25f + 0.75f * saturate(normal.y);
    color = lerp(color, float3(1.0f, 0.85f, 0.35f), roughness * 0.15f);

    return float4(color, 1.0f);
}
