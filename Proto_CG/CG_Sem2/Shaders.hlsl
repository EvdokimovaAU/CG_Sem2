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
    float2 du = ddx(uv);
    float2 dv = ddy(uv);
    float footprint = max(length(du), length(dv));
    float filterAmount = saturate(footprint * 512.0f);
    float2 offset = (abs(du) + abs(dv)) * 0.35f;

    float4 baseSample = tex.Sample(gSampler, uv);
    float4 blurred =
        tex.Sample(gSampler, uv + float2( offset.x,  offset.y)) +
        tex.Sample(gSampler, uv + float2(-offset.x,  offset.y)) +
        tex.Sample(gSampler, uv + float2( offset.x, -offset.y)) +
        tex.Sample(gSampler, uv + float2(-offset.x, -offset.y));
    blurred *= 0.25f;

    return lerp(baseSample, blurred, filterAmount);
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
    float roughness = SampleAntiAliased(gRoughnessTex, uv).r;
    float3x3 tbn = ComputeTBN(normalize(input.NormW), input.WorldPos, uv);
    float3 N = normalize(mul(normalSample, tbn));
    float3 Ldir = normalize(float3(-0.4f, -1.0f, -0.2f));
    float ndotl = saturate(dot(N, -Ldir));

    float4 albedo = SampleAntiAliased(gTex, uv);
    albedo.rgb = saturate(pow(albedo.rgb, 1.08f) * 0.82f);

    float upFactor = saturate(N.y * 0.5f + 0.5f);
    float smoothness = 1.0f - roughness;
    float3 ambient = lerp(float3(0.12f, 0.13f, 0.14f), float3(0.24f, 0.25f, 0.28f), upFactor) * (1.0f - roughness * 0.24f);
    float specPower = lerp(110.0f, 12.0f, roughness * 0.82f);
    float3 V = normalize(float3(0.0f, 0.0f, 1.0f));
    float3 H = normalize(V + (-Ldir));
    float fresnel = pow(1.0f - saturate(dot(N, V)), 5.0f);
    float specular = pow(saturate(dot(N, H)), specPower) * lerp(0.16f, 0.03f, roughness) * (1.0f + fresnel * 1.35f);
    float ambientSpecular = lerp(0.015f, 0.10f, smoothness) * (0.35f + upFactor * 0.65f) * (0.3f + fresnel * 0.7f);
    float3 diffuse = float3(1.00f, 0.95f, 0.85f) * ndotl;
    float3 litLinear = (ambient + diffuse) * albedo.rgb + (specular + ambientSpecular).xxx;
    float3 litSRGB = pow(saturate(litLinear), 1.0f / 2.2f);

    return float4(litSRGB, albedo.a);
}
