Texture2D<float4> HDRColorTex : register(t0);
SamplerState LinearSampler : register(s0);

struct PSInput
{
    float4 Position : SV_POSITION;
    float2 UV : TEXCOORD0;
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
    const float2 uv = kQuadUV[vertexId];
    output.Position = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    output.UV = uv;
    return output;
}

float3 ApplyExposureToneMapping(float3 hdrColor, float exposure)
{
    return 1.0f.xxx - exp(-hdrColor * exposure);
}

float ComputeLuminance(float3 color)
{
    return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}

float EstimateAverageLuminance()
{
    uint width = 0;
    uint height = 0;
    HDRColorTex.GetDimensions(width, height);

    const int2 samplePositions[5] =
    {
        int2(width / 2, height / 2),
        int2(width / 4, height / 4),
        int2((width * 3) / 4, height / 4),
        int2(width / 4, (height * 3) / 4),
        int2((width * 3) / 4, (height * 3) / 4)
    };

    float luminanceSum = 0.0f;
    [unroll]
    for (int i = 0; i < 5; ++i)
    {
        const int2 pixelPos = min(samplePositions[i], int2(width - 1, height - 1));
        const float3 hdrSample = HDRColorTex.Load(int3(pixelPos, 0)).rgb;
        luminanceSum += log2(max(ComputeLuminance(max(hdrSample, 0.0f.xxx)), 0.0001f));
    }

    return exp2(luminanceSum / 5.0f);
}

float3 ComputeBloom(float2 uv, float avgLuminance)
{
    uint width = 0;
    uint height = 0;
    HDRColorTex.GetDimensions(width, height);
    const float2 texelSize = 1.0f / float2(width, height);

    const float bloomThreshold = max(avgLuminance * 1.6f, 0.9f);
    const float2 sampleOffsets[5] =
    {
        float2(-2.0f,  0.0f),
        float2( 0.0f,  0.0f),
        float2( 2.0f,  0.0f),
        float2( 0.0f, -2.0f),
        float2( 0.0f,  2.0f)
    };
    const float sampleWeights[5] =
    {
        0.16f,
        0.36f,
        0.16f,
        0.16f,
        0.16f
    };

    float3 bloom = 0.0f.xxx;
    [unroll]
    for (int i = 0; i < 5; ++i)
    {
        const float2 sampleUv = saturate(uv + sampleOffsets[i] * texelSize * 2.5f);
        const float3 hdrSample = HDRColorTex.SampleLevel(LinearSampler, sampleUv, 0.0f).rgb;
        const float luminance = ComputeLuminance(max(hdrSample, 0.0f.xxx));
        const float contribution = saturate((luminance - bloomThreshold) / max(bloomThreshold, 0.001f));
        bloom += hdrSample * contribution * sampleWeights[i];
    }

    return bloom;
}

// затемнение по бокам
float ComputeVignette(float2 uv, float strength, float roundness)
{
    float2 centeredUv = uv * 2.0f - 1.0f;
    centeredUv.x *= lerp(1.0f, 1.12f, roundness);
    const float radius = length(centeredUv);
    const float vignette = 1.0f - smoothstep(0.45f, 1.05f - strength * 0.35f, radius);
    return lerp(1.0f, vignette, strength);
}

float4 PSMain(PSInput input) : SV_TARGET
{
    const float3 hdrColor = HDRColorTex.Sample(LinearSampler, saturate(input.UV)).rgb;
    const float averageLuminance = EstimateAverageLuminance();

    // Eye Adaptation
    const float middleGray = 0.72f;
    const float exposure = saturate(middleGray / max(averageLuminance, 0.0001f)) * 1.8f;

    // Bloom
    const float3 bloom = ComputeBloom(input.UV, averageLuminance);

    float3 toneMapped = ApplyExposureToneMapping(max(hdrColor, 0.0f.xxx) + bloom * 0.45f, exposure);

    const float vignetteStrength = 0.88f;
    const float vignetteRoundness = 1.0f;
    toneMapped *= ComputeVignette(input.UV, vignetteStrength, vignetteRoundness);

    const float3 monitorColor = pow(saturate(toneMapped), 1.0f / 2.2f);
    return float4(monitorColor, 1.0f);
}
