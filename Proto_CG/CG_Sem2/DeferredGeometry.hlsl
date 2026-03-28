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

void ComputePatchBasis(
    InputPatch<HSControlPoint, 3> patch,
    out float3 tangent,
    out float3 bitangent)
{
    float3 p0 = patch[0].Pos;
    float3 p1 = patch[1].Pos;
    float3 p2 = patch[2].Pos;
    float2 uv0 = patch[0].UV;
    float2 uv1 = patch[1].UV;
    float2 uv2 = patch[2].UV;

    float3 edge1 = p1 - p0;
    float3 edge2 = p2 - p0;
    float2 duv1 = uv1 - uv0;
    float2 duv2 = uv2 - uv0;
    float determinant = duv1.x * duv2.y - duv1.y * duv2.x;

    if (abs(determinant) < 1e-5f)
    {
        tangent = normalize(edge1);
        bitangent = normalize(edge2);
        return;
    }

    float invDet = 1.0f / determinant;
    tangent = normalize((edge1 * duv2.y - edge2 * duv1.y) * invDet);
    bitangent = normalize((edge2 * duv1.x - edge1 * duv2.x) * invDet);
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

    uint dispWidth = 0;
    uint dispHeight = 0;
    gDisplacementTex.GetDimensions(dispWidth, dispHeight);

    float2 texel = float2(1.0f / max((float)dispWidth, 1.0f), 1.0f / max((float)dispHeight, 1.0f));
    float heightU = (gDisplacementTex.SampleLevel(gSampler, uv + float2(texel.x, 0.0f), 0).r - 0.5f) * displacementScale;
    float heightV = (gDisplacementTex.SampleLevel(gSampler, uv + float2(0.0f, texel.y), 0).r - 0.5f) * displacementScale;

    float3 tangent;
    float3 bitangent;
    ComputePatchBasis(patch, tangent, bitangent);

    float4 posW = mul(float4(displacedPos, 1.0f), World);
    float4 posV = mul(posW, View);
    o.PosH = mul(posV, Proj);
    o.WorldPos = posW.xyz;
    o.ViewDepth = posV.z;

    float3x3 world3x3 = (float3x3)World;
    o.NormalW = normalize(mul(world3x3, normal));
    o.UV = baseUV;

    return o;
}

GBufferOutput PSMain(PSInput input)
{
    GBufferOutput o;

    float2 uv = input.UV * UVTransform.xy + UVTransform.zw;
    float4 albedo = SampleAntiAliased(gTex, uv);
    albedo.rgb = saturate(pow(albedo.rgb, 0.9f) * 1.22f);
    float roughness = SampleAntiAliased(gRoughnessTex, uv).r;
    float3 normalSample = SampleAntiAliased(gNormalTex, uv).xyz * 2.0f - 1.0f;
    normalSample.y *= -1.0f;
    float3x3 tbn = ComputeTBN(normalize(input.NormalW), input.WorldPos, uv);
    float3 normal = normalize(mul(normalSample, tbn));
    float depth = saturate(input.ViewDepth / 20000.0f);

    o.AlbedoSpec = float4(albedo.rgb, roughness);
    o.WorldPos = float4(input.WorldPos, 1.0f);
    o.Normal = float4(normal * 0.5f + 0.5f, 1.0f);
    o.Depth = float4(depth, depth, depth, 1.0f);

    return o;
}
