cbuffer PerObjectCB : register(b0)
{
    float4x4 World;
    float4x4 View;
    float4x4 Proj;
    float4 UVTransform;
    float4 TimeParams;
    float4 TessellationParams;
};

Texture2D gDisplacementTex : register(t0);
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

struct DSOutput
{
    float4 PosH : SV_POSITION;
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
    float maxTess = max(TessellationParams.y, 1.0f);
    // Keep shadow-caster displacement stable regardless of camera distance.
    // Using camera-dependent tessellation here causes the visible surface and
    // shadow map to diverge as the camera moves closer or farther away.
    return maxTess;
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

[domain("tri")]
DSOutput DSMain(
    HSConstants patchConstants,
    const OutputPatch<HSControlPoint, 3> patch,
    float3 bary : SV_DomainLocation)
{
    DSOutput o;

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
    return o;
}
