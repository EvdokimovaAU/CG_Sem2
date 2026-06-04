cbuffer PerObjectCB : register(b0)
{
    float4x4 World;
    float4x4 View;
    float4x4 Proj;
    float4 UVTransform;
    float4 TimeParams;
    float4 TessellationParams;
};

Texture2D gTex : register(t0);
Texture2D gDisplacementTex : register(t1);
Texture2D gNormalTex : register(t2);
Texture2D gRoughnessTex : register(t3);
SamplerState gSampler : register(s0);

struct VSInput
{
    float3 Pos : POSITION;
    float3 Norm : NORMAL;
    float2 UV : TEXCOORD0;
};

struct HSControlPoint
{
    float3 Pos : POSITION;
    float3 Norm : NORMAL;
    float2 UV : TEXCOORD0;
};

struct HSConstants
{
    float Edges[3] : SV_TessFactor;
    float Inside : SV_InsideTessFactor;
};

struct PSInput
{
    float4 PosH : SV_POSITION;
    float3 WorldPos : TEXCOORD4;
    float3 NormW : TEXCOORD1;
    float2 UV : TEXCOORD0;
    float3 ViewPos : TEXCOORD5;
};

HSControlPoint VSMain(VSInput input)
{
    HSControlPoint o;
    o.Pos = input.Pos;
    o.Norm = input.Norm;
    o.UV = input.UV;
    return o;
}

float ComputeTessFactor(float3 p0, float3 p1, float3 p2)
{
    float3 center = (p0 + p1 + p2) / 3.0f;
    float3 centerW = mul(float4(center, 1.0f), World).xyz;
    float3 centerV = mul(float4(centerW, 1.0f), View).xyz;

    float maxTess = max(TessellationParams.y, 1.0f);
    float minTess = max(min(TessellationParams.z, maxTess), 1.0f);
    float maxDistance = max(TessellationParams.w, 1.0f);
    float distanceFade = saturate(abs(centerV.z) / maxDistance);

    return lerp(maxTess, minTess, distanceFade);
}

[patchconstantfunc("PatchConstantFunction")]
[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[maxtessfactor(64.0)]
HSControlPoint HSMain(
    InputPatch<HSControlPoint, 3> patch,
    uint controlPointId : SV_OutputControlPointID)
{
    return patch[controlPointId];
}

HSConstants PatchConstantFunction(InputPatch<HSControlPoint, 3> patch)
{
    HSConstants output;
    float tess = ComputeTessFactor(patch[0].Pos, patch[1].Pos, patch[2].Pos);
    output.Edges[0] = tess;
    output.Edges[1] = tess;
    output.Edges[2] = tess;
    output.Inside = tess;
    return output;
}

float3x3 ComputeTBN(float3 normalW, float3 worldPos, float2 uv)
{
    float3 dp1 = ddx(worldPos);
    float3 dp2 = ddy(worldPos);
    float2 duv1 = ddx(uv);
    float2 duv2 = ddy(uv);

    float3 dp2Perp = cross(dp2, normalW);
    float3 dp1Perp = cross(normalW, dp1);
    float3 tangent = dp2Perp * duv1.x + dp1Perp * duv2.x;
    float3 bitangent = dp2Perp * duv1.y + dp1Perp * duv2.y;

    float invMax = rsqrt(max(dot(tangent, tangent), dot(bitangent, bitangent)));
    return float3x3(tangent * invMax, bitangent * invMax, normalW);
}

float4 SampleAntiAliased(Texture2D tex, float2 uv)
{
    float filterStrength = saturate(TimeParams.y);
    if (filterStrength <= 0.001f)
    {
        return tex.SampleLevel(gSampler, uv, 0.0f);
    }

    float2 du = ddx(uv);
    float2 dv = ddy(uv);
    float2 offset = (abs(du) + abs(dv)) * lerp(0.0f, 1.35f, filterStrength);

    float4 baseSample = tex.SampleLevel(gSampler, uv, 0.0f);
    float4 blurred =
        tex.Sample(gSampler, uv + float2( offset.x,  offset.y)) +
        tex.Sample(gSampler, uv + float2(-offset.x,  offset.y)) +
        tex.Sample(gSampler, uv + float2( offset.x, -offset.y)) +
        tex.Sample(gSampler, uv + float2(-offset.x, -offset.y));
    blurred *= 0.25f;

    return lerp(baseSample, blurred, saturate(filterStrength * 1.15f));
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

float3 SampleSkyColor(float3 dir)
{
    float up = saturate(dir.y * 0.5f + 0.5f);
    float horizon = 1.0f - abs(dir.y);
    float3 groundColor = float3(0.05f, 0.045f, 0.04f);
    float3 horizonColor = float3(0.78f, 0.82f, 0.88f);
    float3 zenithColor = float3(0.16f, 0.30f, 0.52f);
    float3 sky = lerp(horizonColor, zenithColor, saturate(pow(up, 1.35f)));
    float3 env = lerp(groundColor, sky, up);
    env += horizonColor * pow(horizon, 6.0f) * 0.08f;
    return env;
}

float3 SampleEnvironment(float3 dir, float roughness)
{
    float3 sharp = SampleSkyColor(dir);
    float3 blurred = SampleSkyColor(normalize(float3(dir.x, abs(dir.y) * 0.35f + 0.65f, dir.z)));
    return lerp(sharp, blurred, saturate(roughness * roughness));
}

float3 SampleIrradiance(float3 normal)
{
    float up = saturate(normal.y * 0.5f + 0.5f);
    float3 skyIrradiance = lerp(float3(0.24f, 0.26f, 0.30f), float3(0.58f, 0.68f, 0.82f), up);
    float3 groundBounce = float3(0.06f, 0.05f, 0.045f) * saturate(-normal.y * 0.5f + 0.5f);
    return skyIrradiance + groundBounce;
}

[domain("tri")]
PSInput DSMain(
    HSConstants patchConstants,
    const OutputPatch<HSControlPoint, 3> patch,
    float3 bary : SV_DomainLocation)
{
    PSInput o;

    float3 pos = patch[0].Pos * bary.x + patch[1].Pos * bary.y + patch[2].Pos * bary.z;
    float3 normal = normalize(patch[0].Norm * bary.x + patch[1].Norm * bary.y + patch[2].Norm * bary.z);
    float2 baseUV = patch[0].UV * bary.x + patch[1].UV * bary.y + patch[2].UV * bary.z;
    float2 uv = baseUV * UVTransform.xy + UVTransform.zw;

    float displacementScale = TessellationParams.x;
    float height = (gDisplacementTex.SampleLevel(gSampler, uv, 0).r - 0.5f) * displacementScale;
    float3 displacedPos = pos + normal * height;

    float4 posW = mul(float4(displacedPos, 1.0f), World);
    float4 posV = mul(posW, View);
    o.PosH = mul(posV, Proj);
    o.WorldPos = posW.xyz;
    o.ViewPos = posV.xyz;

    float3x3 world3x3 = (float3x3)World;
    o.NormW = normalize(mul(world3x3, normal));
    o.UV = baseUV;

    return o;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float2 uv = input.UV * UVTransform.xy + UVTransform.zw;
    float3 normalSample = SampleAntiAliased(gNormalTex, uv).xyz * 2.0f - 1.0f;
    normalSample.y *= -1.0f;
    float roughness = clamp(SampleAntiAliased(gRoughnessTex, uv).r, 0.045f, 1.0f);
    float3x3 tbn = ComputeTBN(normalize(input.NormW), input.WorldPos, uv);
    float3 N = normalize(mul(normalSample, tbn));
    float3 Ldir = normalize(float3(-0.4f, -1.0f, -0.2f));
    float3 L = -Ldir;
    float ndotl = saturate(dot(N, L));

    float4 albedo = SampleAntiAliased(gTex, uv);
    albedo.rgb = pow(saturate(albedo.rgb), 2.2f);

    const float metallic = 0.0f;
    float3 V = normalize(-input.ViewPos);
    float3 H = normalize(V + L);
    float NdotV = saturate(dot(N, V));
    float VdotH = saturate(dot(V, H));
    float3 F0 = lerp(0.04f.xxx, albedo.rgb, metallic);
    float3 F = FresnelSchlick(VdotH, F0);
    float D = DistributionGGX(N, H, roughness);
    float G = GeometrySmith(N, V, L, roughness);
    float3 specular = (D * G * F) / max(4.0f * NdotV * ndotl, 0.0001f);
    float3 kD = (1.0f.xxx - F) * (1.0f - metallic);
    float3 diffuse = kD * albedo.rgb / 3.14159265f;
    float3 radiance = 3.5f.xxx;
    float3 ambientDiffuse = SampleIrradiance(N) * albedo.rgb * kD * 0.35f;
    float3 R = reflect(-V, N);
    float3 ambientSpecular = SampleEnvironment(R, roughness) * FresnelSchlick(NdotV, F0) * lerp(1.0f, 0.35f, roughness);
    float sunReflection = pow(saturate(dot(R, L)), lerp(96.0f, 8.0f, roughness));
    ambientSpecular += radiance * sunReflection * FresnelSchlick(NdotV, F0) * (1.0f - roughness * 0.5f);
    float3 litLinear = ambientDiffuse + ambientSpecular + (diffuse + specular) * radiance * ndotl;
    float3 toneMapped = litLinear / (litLinear + 1.0f.xxx);
    float3 litSRGB = pow(saturate(toneMapped), 1.0f / 2.2f);

    return float4(litSRGB, albedo.a);
}
