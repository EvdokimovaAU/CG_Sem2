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

float3 GetCameraPositionWS()
{
    return mul(float4(0.0f, 0.0f, 0.0f, 1.0f), InvView).xyz;
}

float DistributionGGX(float3 N, float3 H, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = saturate(dot(N, H));
    float NdotH2 = NdotH * NdotH;
    float denom = NdotH2 * (a2 - 1.0f) + 1.0f;
    return a2 / max(3.14159265f * denom * denom, 0.0001f);
}

float GeometrySchlickGGX(float NdotV, float roughness)
{
    float r = roughness + 1.0f;
    float k = (r * r) / 8.0f;
    return NdotV / max(NdotV * (1.0f - k) + k, 0.0001f);
}

float GeometrySmith(float3 N, float3 V, float3 L, float roughness)
{
    float NdotV = saturate(dot(N, V));
    float NdotL = saturate(dot(N, L));
    float ggxV = GeometrySchlickGGX(NdotV, roughness);
    float ggxL = GeometrySchlickGGX(NdotL, roughness);
    return ggxV * ggxL;
}

float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0f - F0) * pow(1.0f - cosTheta, 5.0f);
}

float3 EvaluateLight(
    float3 albedo,
    float roughness,
    float3 N,
    float3 V,
    float3 L,
    float3 radiance)
{
    float3 H = normalize(V + L);
    float NdotL = saturate(dot(N, L));
    float NdotV = saturate(dot(N, V));
    float HdotV = saturate(dot(H, V));

    if (NdotL <= 0.0f || NdotV <= 0.0f)
    {
        return 0.0f.xxx;
    }

    float smoothness = 1.0f - roughness;
    float clampedRoughness = clamp(lerp(0.045f, 1.0f, roughness * 0.82f), 0.045f, 1.0f);
    float3 F0 = lerp(float3(0.035f, 0.035f, 0.035f), float3(0.065f, 0.065f, 0.065f), smoothness * 0.65f);
    float3 F = FresnelSchlick(HdotV, F0);
    float D = DistributionGGX(N, H, clampedRoughness);
    float G = GeometrySmith(N, V, L, clampedRoughness);

    float3 numerator = D * G * F;
    float denominator = max(4.0f * NdotV * NdotL, 0.0001f);
    float3 specular = numerator / denominator;

    float3 kd = (1.0f.xxx - F) * (1.0f - smoothness * 0.08f);
    float3 diffuse = kd * albedo / 3.14159265f;
    return (diffuse + specular) * radiance * NdotL;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    int2 pixelPos = int2(input.Position.xy);

    float4 albedoSpec = GAlbedoSpec.Load(int3(pixelPos, 0));
    float3 worldPos = GWorldPos.Load(int3(pixelPos, 0)).xyz;
    float3 normalEncoded = GNormal.Load(int3(pixelPos, 0)).xyz;
    float depth = GDepth.Load(int3(pixelPos, 0)).x;

    if (depth >= 1.0f)
    {
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    }

    float3 albedo = albedoSpec.rgb;
    float roughness = saturate(albedoSpec.a);
    float smoothness = 1.0f - roughness;
    float3 normal = normalize(normalEncoded * 2.0f - 1.0f);
    float3 cameraPos = GetCameraPositionWS();
    float3 V = normalize(cameraPos - worldPos);

    float upFactor = saturate(normal.y * 0.5f + 0.5f);
    float3 skyAmbient = AmbientColor.rgb * lerp(0.55f, 1.15f, upFactor);
    float3 lit = skyAmbient * albedo * (1.0f - roughness * 0.22f);
    float3 ambientF0 = lerp(float3(0.035f, 0.035f, 0.035f), float3(0.065f, 0.065f, 0.065f), smoothness * 0.65f);
    float3 ambientSpecular = FresnelSchlick(saturate(dot(normal, V)), ambientF0) * lerp(0.015f, 0.11f, smoothness) * lerp(0.55f, 1.15f, upFactor);
    lit += ambientSpecular;

    float3 directionalL = normalize(-LightDirection.xyz);
    float3 directionalRadiance = LightColor.rgb * LightColor.a;
    lit += EvaluateLight(albedo, roughness, normal, V, directionalL, directionalRadiance);

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
        float3 radiance = PointLightColorIntensity[i].rgb * (PointLightColorIntensity[i].a * attenuation);
        lit += EvaluateLight(albedo, roughness, normal, V, L, radiance);
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

        float3 radiance = SpotLightColorIntensity[i].rgb * (SpotLightColorIntensity[i].a * attenuation * spotAmount);
        lit += EvaluateLight(albedo, roughness, normal, V, L, radiance);
    }

    float3 litSRGB = pow(saturate(lit), 1.0f / 2.2f);
    return float4(litSRGB, 1.0f);
}
