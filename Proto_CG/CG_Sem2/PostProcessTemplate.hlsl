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

    // чтение ресурсов из GBuffer
    float4 albedoSpec = GAlbedoSpec.Sample(PointSampler, uv);
    float3 worldPos = GWorldPos.Sample(PointSampler, uv).xyz;
    float3 normalEncoded = GNormal.Sample(PointSampler, uv).xyz;
    float depth = GDepth.Sample(PointSampler, uv).x;

    // распаковка 
    float3 albedo = albedoSpec.rgb;
    float roughness = albedoSpec.a;
    float3 normal = normalize(normalEncoded * 2.0f - 1.0f);

    // отсечение фона
    if (depth >= 1.0f)
    {
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    }


    // загтовка использования
    float3 color = albedo;
    color *= 0.25f + 0.75f * saturate(normal.y);
    color = lerp(color, float3(1.0f, 0.85f, 0.35f), roughness * 0.15f);

    return float4(color, 1.0f);
}
