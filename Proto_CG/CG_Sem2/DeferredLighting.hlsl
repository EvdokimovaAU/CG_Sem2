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
    float4 CascadeSplits;
    float4 ShadowParams;
    float4x4 View;
    float4x4 LightViewProj[4];
    float4x4 InvView;
    float4x4 InvProj;
};

#define POST_PROCESS_DEBUG_MODE 0

Texture2D<float4> GAlbedoSpec : register(t0);
Texture2D<float4> GWorldPos : register(t1);
Texture2D<float4> GNormal : register(t2);
Texture2D<float4> GDepth : register(t3);
Texture2DArray<float> ShadowMap : register(t4); // массив каскадов
Texture2D<float4> ShadowMaskTex : register(t5);
SamplerState ShadowSampler : register(s0);
SamplerState ShadowMaskSampler : register(s1);

struct PSInput
{
    float4 Position : SV_POSITION;
    float2 UV : TEXCOORD0;
};

PSInput VSMain(uint vertexId : SV_VertexID) // full-sreen quad
{
    PSInput output;
    static const float2 kQuadUV[6] = // передаем вершинные в пискельный
    {
        float2(0.0f, 0.0f),
        float2(1.0f, 0.0f),
        float2(0.0f, 1.0f),
        float2(0.0f, 1.0f),
        float2(1.0f, 0.0f),
        float2(1.0f, 1.0f)
    };

    float2 uv = kQuadUV[vertexId]; 

    output.Position = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    output.UV = uv;
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

float ComputeShadow(float3 worldPos, float3 normal, float3 lightDir)
{
    const bool useStaticShadowSelection = ShadowParams.x > 0.5f;
    const float bias = useStaticShadowSelection
        ? ShadowParams.z
        : (ShadowParams.z + (1.0f - saturate(dot(normal, lightDir))) * ShadowParams.w);
    const float texelSize = ShadowParams.y;
    const float viewDepth = mul(float4(worldPos, 1.0f), View).z;
    uint startCascadeIndex = 0;

    if (viewDepth > CascadeSplits.x) startCascadeIndex = 1;
    if (viewDepth > CascadeSplits.y) startCascadeIndex = 2;
    if (viewDepth > CascadeSplits.z) startCascadeIndex = 3;
    if (!useStaticShadowSelection && viewDepth > CascadeSplits.w) return 1.0f;

    float bestVisibility = 1.0f;
    bool foundCascade = false;

    [unroll]
    for (uint cascadeIndex = (useStaticShadowSelection ? 0u : startCascadeIndex); cascadeIndex < 4u; ++cascadeIndex)
    {
        float4 lightClip = mul(float4(worldPos, 1.0f), LightViewProj[cascadeIndex]);
        float3 lightNdc = lightClip.xyz / max(lightClip.w, 0.0001f);
        float2 shadowUv = lightNdc.xy * float2(0.5f, -0.5f) + 0.5f.xx;
        float shadowDepth = lightNdc.z;
        const float borderMargin = texelSize * 2.5f;

        if (shadowUv.x <= -borderMargin || shadowUv.x >= 1.0f + borderMargin ||
            shadowUv.y <= -borderMargin || shadowUv.y >= 1.0f + borderMargin ||
            shadowDepth <= 0.0f || shadowDepth >= 1.0f)
        {
            continue;
        }

        shadowUv = clamp(shadowUv, texelSize.xx, 1.0f.xx - texelSize.xx);

        float visibility = 0.0f;
        [unroll]
        for (int y = -1; y <= 1; ++y)
        {
            [unroll]
            for (int x = -1; x <= 1; ++x)
            {
                const float2 offset = float2(x, y) * texelSize;
                const float sampledDepth = ShadowMap.SampleLevel(
                    ShadowSampler,
                    float3(shadowUv + offset, cascadeIndex),
                    0.0f);
                visibility += ((shadowDepth - bias) <= sampledDepth) ? 1.0f : 0.0f;
            }
        }

        visibility /= 9.0f;

        if (useStaticShadowSelection)
        {
            bestVisibility = min(bestVisibility, visibility);
            foundCascade = true;
            continue;
        }

        return visibility;
    }

    return foundCascade ? bestVisibility : 1.0f;
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
    float clampedRoughness = clamp(lerp(0.12f, 1.0f, roughness * 0.90f), 0.12f, 1.0f);
    float3 F0 = lerp(float3(0.026f, 0.026f, 0.026f), float3(0.045f, 0.045f, 0.045f), smoothness * 0.45f);
    float3 F = FresnelSchlick(HdotV, F0);
    F *= lerp(0.72f, 0.90f, roughness);
    float D = DistributionGGX(N, H, clampedRoughness);
    float G = GeometrySmith(N, V, L, clampedRoughness);

    float3 numerator = D * G * F;
    float denominator = max(4.0f * NdotV * NdotL, 0.0001f);
    float3 specular = numerator / denominator;

    float3 kd = (1.0f.xxx - F) * (1.0f - smoothness * 0.05f);
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

#if POST_PROCESS_DEBUG_MODE == 1
    return float4(albedo, 1.0f);
#elif POST_PROCESS_DEBUG_MODE == 2
    return float4(frac(worldPos * 0.05f), 1.0f);
#elif POST_PROCESS_DEBUG_MODE == 3
    return float4(normal * 0.5f + 0.5f, 1.0f);
#elif POST_PROCESS_DEBUG_MODE == 4
    float depthVis = saturate(pow(depth, 0.35f));
    return float4(depthVis, depthVis, depthVis, 1.0f);
#endif

    float3 cameraPos = GetCameraPositionWS();
    float3 V = normalize(cameraPos - worldPos);

    float upFactor = saturate(normal.y * 0.5f + 0.5f);
    const float sceneColorScale = AmbientColor.a;
    float3 skyAmbient = AmbientColor.rgb * lerp(0.72f, 1.18f, upFactor);
    float3 lit = skyAmbient * albedo * (1.0f - roughness * 0.18f);
    float3 ambientF0 = lerp(float3(0.026f, 0.026f, 0.026f), float3(0.045f, 0.045f, 0.045f), smoothness * 0.45f);
    float viewFacing = saturate(dot(normal, V));
    float3 ambientSpecular = FresnelSchlick(viewFacing, ambientF0) * lerp(0.006f, 0.032f, smoothness) * lerp(0.50f, 0.92f, upFactor);
    ambientSpecular *= lerp(0.55f, 1.0f, viewFacing);
    lit += ambientSpecular;
    lit += albedo * 0.030f;

    float3 directionalL = normalize(-LightDirection.xyz);
    float3 directionalRadiance = LightColor.rgb * LightColor.a;
    const float directionalShadow = ComputeShadow(worldPos, normal, directionalL);
    lit += EvaluateLight(albedo, roughness, normal, V, directionalL, directionalRadiance) * directionalShadow;

    const int pointCount = (int)LightCounts.x;
    const int spotCount = (int)LightCounts.y;

    [loop]
    for (int i = 0; i < pointCount; ++i)
    {
        float3 toLight = PointLightPositionRange[i].xyz - worldPos;
        float dist = length(toLight);
        float range = max(PointLightPositionRange[i].w, 0.0001f);
        float normalizedDist = saturate(dist / range);
        float falloff = 1.0f - normalizedDist * normalizedDist;
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

        float normalizedDist = saturate(dist / range);
        float falloff = 1.0f - normalizedDist * normalizedDist;
        float attenuation = falloff * falloff;

        float3 spotDir = normalize(SpotLightDirectionCosine[i].xyz);
        float outerConeCos = SpotLightDirectionCosine[i].w;
        float innerConeCos = lerp(outerConeCos, 1.0f, 0.14f);
        float coneFactor = dot(-L, spotDir);
        float spotAmount = smoothstep(outerConeCos, innerConeCos, coneFactor);
        spotAmount = lerp(0.35f, 1.0f, spotAmount);
        spotAmount *= spotAmount;

        float3 radiance = SpotLightColorIntensity[i].rgb * (SpotLightColorIntensity[i].a * attenuation * spotAmount);
        lit += EvaluateLight(albedo, roughness, normal, V, L, radiance);
    }

    const float shadowAmount = saturate(1.0f - directionalShadow);
    const float2 maskUv = worldPos.xz * float2(0.01f, 0.03f);
    const float maskSample = ShadowMaskTex.Sample(ShadowMaskSampler, maskUv).a;
    const float shadowPattern = shadowAmount * saturate(maskSample * 1.35f);

    lit *= lerp(0.28f, 1.0f, directionalShadow);
    lit *= 1.0f - shadowPattern * 0.65f;
    lit *= sceneColorScale;

    return float4(max(lit, 0.0f.xxx), 1.0f);
}
