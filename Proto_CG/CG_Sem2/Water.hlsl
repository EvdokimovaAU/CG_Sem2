cbuffer WaterCB : register(b0)
{
    float4x4 View;
    float4x4 Proj;
    float4 CameraPos;
    float4 WaterOrigin;
    float4 WaterSize;
    float4 WaterColor;
    float4 WaveA;
    float4 WaveB;
};

struct HSControlPoint
{
    float2 LocalXZ : TEXCOORD0;
    float2 UV : TEXCOORD1;
};

struct HSConstants
{
    float Edges[3] : SV_TessFactor;
    float Inside : SV_InsideTessFactor;
};

struct PSInput
{
    float4 Position : SV_POSITION;
    float3 WorldPos : TEXCOORD0;
    float3 NormalW : TEXCOORD1;
    float2 UV : TEXCOORD2;
};

HSControlPoint VSMain(uint vertexId : SV_VertexID)
{
    static const float2 kPositions[6] =
    {
        float2(-1.0f, -1.0f),
        float2( 1.0f, -1.0f),
        float2( 1.0f,  1.0f),
        float2(-1.0f, -1.0f),
        float2( 1.0f,  1.0f),
        float2(-1.0f,  1.0f)
    };

    HSControlPoint output;
    output.LocalXZ = kPositions[vertexId];
    output.UV = kPositions[vertexId] * 0.5f + 0.5f;
    return output;
}

float ComputeTessFactor(float3 worldPos)
{
    float distanceToCamera = distance(worldPos, CameraPos.xyz);
    float fade = saturate(distanceToCamera / 1400.0f);
    return lerp(18.0f, 6.0f, fade);
}

[patchconstantfunc("PatchConstantFunction")]
[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[maxtessfactor(32.0)]
HSControlPoint HSMain(
    InputPatch<HSControlPoint, 3> patch,
    uint controlPointId : SV_OutputControlPointID)
{
    return patch[controlPointId];
}

HSConstants PatchConstantFunction(InputPatch<HSControlPoint, 3> patch)
{
    HSConstants output;

    float2 localXZ =
        patch[0].LocalXZ * (1.0f / 3.0f) +
        patch[1].LocalXZ * (1.0f / 3.0f) +
        patch[2].LocalXZ * (1.0f / 3.0f);

    float3 worldPos = float3(
        WaterOrigin.x + localXZ.x * WaterSize.x,
        WaterOrigin.y,
        WaterOrigin.z + localXZ.y * WaterSize.y);

    float tess = ComputeTessFactor(worldPos);
    output.Edges[0] = tess;
    output.Edges[1] = tess;
    output.Edges[2] = tess;
    output.Inside = tess;
    return output;
}

float EvaluateWaveHeight(float2 worldXZ)
{
    float time = WaterOrigin.w;
    float wave1 = sin(worldXZ.x * WaveA.x + time * WaveA.z) * WaveA.y;
    float wave2 = cos(worldXZ.y * WaveB.x + time * WaveB.z) * WaveB.y;
    float wave3 = sin((worldXZ.x + worldXZ.y) * WaveB.w + time * WaveA.w) * (WaveA.y * 0.45f);
    return wave1 + wave2 + wave3;
}

[domain("tri")]
PSInput DSMain(
    HSConstants patchConstants,
    const OutputPatch<HSControlPoint, 3> patch,
    float3 bary : SV_DomainLocation)
{
    PSInput output;

    float2 localXZ =
        patch[0].LocalXZ * bary.x +
        patch[1].LocalXZ * bary.y +
        patch[2].LocalXZ * bary.z;

    float2 uv =
        patch[0].UV * bary.x +
        patch[1].UV * bary.y +
        patch[2].UV * bary.z;

    float2 worldXZ = float2(
        WaterOrigin.x + localXZ.x * WaterSize.x,
        WaterOrigin.z + localXZ.y * WaterSize.y);

    float height = EvaluateWaveHeight(worldXZ);
    float stepX = 0.75f;
    float stepZ = 0.75f;
    float heightX = EvaluateWaveHeight(worldXZ + float2(stepX, 0.0f));
    float heightZ = EvaluateWaveHeight(worldXZ + float2(0.0f, stepZ));

    float3 worldPos = float3(worldXZ.x, WaterOrigin.y + height, worldXZ.y);
    float3 dx = float3(stepX, heightX - height, 0.0f);
    float3 dz = float3(0.0f, heightZ - height, stepZ);
    float3 normalW = normalize(cross(dz, dx));

    float4 positionV = mul(float4(worldPos, 1.0f), View);
    output.Position = mul(positionV, Proj);
    output.WorldPos = worldPos;
    output.NormalW = normalW;
    output.UV = uv;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float3 viewDir = normalize(CameraPos.xyz - input.WorldPos);
    float fresnel = pow(1.0f - saturate(dot(input.NormalW, viewDir)), 4.0f);
    float3 lightDir = normalize(float3(-0.38f, -1.0f, -0.22f));
    float3 halfVec = normalize(viewDir - lightDir);
    float specular = pow(saturate(dot(input.NormalW, halfVec)), 96.0f);

    float2 waveUv = input.UV * 10.0f;
    float foam =
        sin(waveUv.x + WaterOrigin.w * 1.8f) *
        cos(waveUv.y * 1.2f - WaterOrigin.w * 1.5f);
    foam = saturate(foam * 0.5f + 0.5f);
    foam = pow(foam, 5.0f);

    float3 shallow = WaterColor.rgb;
    float3 deep = shallow * float3(0.42f, 0.60f, 0.78f);
    float depthTint = saturate(0.5f + input.NormalW.y * 0.5f);
    float3 color = lerp(deep, shallow, depthTint);
    color += fresnel * float3(0.58f, 0.74f, 0.88f);
    color += specular * (0.95f + fresnel * 1.25f);
    color += foam * 0.16f;

    float alpha = saturate(WaterColor.a + fresnel * 0.18f + foam * 0.06f);
    return float4(color, alpha);
}
