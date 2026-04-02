cbuffer DeferredLightCB : register(b0)
{
    float4 LightDirection;
    float4 LightColor;
    float4 AmbientColor;
    float4 LightCounts;

    float4 PointLightPositionRange[6];
    float4 PointLightColorIntensity[6];
    float4 SpotLightPositionRange[4];
    float4 SpotLightDirectionCosine[4];
    float4 SpotLightColorIntensity[4];
    float4 ScreenSize;
    float4x4 InvView;
    float4x4 InvProj;
};

Texture2D<float4> GAlbedoSpec : register(t0);
Texture2D<float4> GWorldPos : register(t1);
Texture2D<float4> GNormal : register(t2);
Texture2D<float4> GDepth : register(t3);

struct PSInput
{
    float4 Position : SV_POSITION;
};

PSInput VSMain(uint vertexId : SV_VertexID)
{
    PSInput output;
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.Position = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    int2 pixelPos = int2(input.Position.xy);

    float4 albedo = GAlbedoSpec.Load(int3(pixelPos, 0));
    float3 worldPos = GWorldPos.Load(int3(pixelPos, 0)).xyz;
    float3 normalEncoded = GNormal.Load(int3(pixelPos, 0)).xyz;
    float depth = GDepth.Load(int3(pixelPos, 0)).x;

    if (depth >= 1.0f)
    {
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    }

    float3 normal = normalize(normalEncoded * 2.0f - 1.0f);
    float3 ambient = AmbientColor.rgb * albedo.rgb;
    float3 lit = ambient;

    float3 directionalL = normalize(-LightDirection.xyz);
    float directionalNdotL = saturate(dot(normal, directionalL));
    lit += LightColor.rgb * directionalNdotL * albedo.rgb * LightColor.a;

    const int pointCount = (int)LightCounts.x;
    const int spotCount = (int)LightCounts.y;

    [loop]
    for (int i = 0; i < pointCount; ++i)
    {
        float3 toLight = PointLightPositionRange[i].xyz - worldPos;
        float dist = length(toLight);
        float range = max(PointLightPositionRange[i].w, 0.0001f);
        float falloff = saturate(1.0f - dist / range);
        float attenuation = falloff * falloff;

        float3 L = toLight / max(dist, 0.0001f);
        float ndotl = saturate(dot(normal, L));
        float intensity = PointLightColorIntensity[i].a;
        lit += PointLightColorIntensity[i].rgb * ndotl * attenuation * intensity * albedo.rgb;

    }

    [loop]
    for (int i = 0; i < spotCount; ++i)
    {
        float3 toLight = SpotLightPositionRange[i].xyz - worldPos;
        float dist = length(toLight);
        float range = max(SpotLightPositionRange[i].w, 0.0001f);
        float3 L = toLight / max(dist, 0.0001f);

        float falloff = saturate(1.0f - dist / range);
        float attenuation = falloff * falloff;

        float3 spotDir = normalize(SpotLightDirectionCosine[i].xyz);
        float coneCos = SpotLightDirectionCosine[i].w;
        float spotAmount = saturate((dot(-L, spotDir) - coneCos) / max(1.0f - coneCos, 0.0001f));
        spotAmount = spotAmount * spotAmount * spotAmount;

        float ndotl = saturate(dot(normal, L));
        float intensity = SpotLightColorIntensity[i].a;
        lit += SpotLightColorIntensity[i].rgb * ndotl * attenuation * spotAmount * intensity * albedo.rgb;

    }

    float3 litSRGB = pow(saturate(lit), 1.0f / 2.2f);
    return float4(litSRGB, albedo.a);
}
