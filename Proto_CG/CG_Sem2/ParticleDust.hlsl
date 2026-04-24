struct DustParticle
{
    float3 Position;
    float Size;
    float3 Velocity;
    float Seed;
};

cbuffer ParticleSimulationCB : register(b0)
{
    float4 DeltaTimeTime;
    float4 BoundsMin;
    float4 BoundsMax;
    float4 NoiseParams;
};

cbuffer ParticleRenderCB : register(b1)
{
    float4x4 ViewProj;
    float4 CameraRight;
    float4 CameraUp;
    float4 DustColor;
    float4 EffectParams;
};

ConsumeStructuredBuffer<DustParticle> gParticleInput : register(u0);
AppendStructuredBuffer<DustParticle> gParticleOutput : register(u1);
StructuredBuffer<DustParticle> gParticles : register(t0);
Texture2D gDustTexture : register(t1);
SamplerState gDustSampler : register(s0);

struct VSOutput
{
    float3 Center : POSITION;
    float Size : SIZE;
    float Seed : SEED;
};

struct GSOutput
{
    float4 PositionH : SV_POSITION;
    float2 UV : TEXCOORD0;
    float Seed : TEXCOORD1;
};

float WrapAxis(float value, float minValue, float maxValue)
{
    if (value < minValue)
    {
        return maxValue;
    }

    if (value > maxValue)
    {
        return minValue;
    }

    return value;
}

[numthreads(64, 1, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    if (dispatchThreadID.x >= (uint)DeltaTimeTime.z)
    {
        return;
    }

    DustParticle particle = gParticleInput.Consume();
    float deltaTime = DeltaTimeTime.x;
    float time = DeltaTimeTime.y;

    float phase = particle.Seed + time;
    float3 noiseVelocity = float3(
        sin(phase * 0.73f) + sin(phase * 1.91f + particle.Position.z * 0.01f) * 0.8f,
        sin(phase * 0.41f + 1.7f),
        cos(phase * 0.67f + 0.9f) + cos(phase * 1.57f + particle.Position.x * 0.01f) * 0.8f);

    float3 velocity = particle.Velocity;
    velocity += noiseVelocity * float3(NoiseParams.x, NoiseParams.z, NoiseParams.y);
    velocity.x += sin(time * 0.85f + particle.Seed * 9.0f) * 1.8f;
    velocity.z += cos(time * 0.95f + particle.Seed * 7.0f) * 1.8f;

    particle.Position += velocity * deltaTime;

    particle.Position.x = WrapAxis(particle.Position.x, BoundsMin.x, BoundsMax.x);
    particle.Position.y = WrapAxis(particle.Position.y, BoundsMin.y, BoundsMax.y);
    particle.Position.z = WrapAxis(particle.Position.z, BoundsMin.z, BoundsMax.z);

    gParticleOutput.Append(particle);
}

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    DustParticle particle = gParticles[vertexId];

    VSOutput output;
    output.Center = particle.Position;
    output.Size = particle.Size;
    output.Seed = particle.Seed;
    return output;
}

[maxvertexcount(4)]
void GSMain(point VSOutput input[1], inout TriangleStream<GSOutput> stream)
{
    static const float2 corners[4] =
    {
        float2(-1.0f, -1.0f),
        float2(-1.0f,  1.0f),
        float2( 1.0f, -1.0f),
        float2( 1.0f,  1.0f)
    };

    static const float2 uvs[4] =
    {
        float2(0.0f, 1.0f),
        float2(0.0f, 0.0f),
        float2(1.0f, 1.0f),
        float2(1.0f, 0.0f)
    };

    float3 right = CameraRight.xyz;
    float3 up = CameraUp.xyz;

    [unroll]
    for (uint i = 0; i < 4; ++i)
    {
        GSOutput output;
        float2 corner = corners[i] * input[0].Size;
        float3 worldPos = input[0].Center + right * corner.x + up * corner.y;
        output.PositionH = mul(float4(worldPos, 1.0f), ViewProj);
        output.UV = uvs[i];
        output.Seed = input[0].Seed;
        stream.Append(output);
    }
}

float4 PSMain(GSOutput input) : SV_TARGET
{
    float2 centeredUv = input.UV * 2.0f - 1.0f;
    float radial = saturate(1.0f - dot(centeredUv, centeredUv) * 0.35f);
    float tint = 0.82f + 0.18f * frac(input.Seed * 1.6180339f);
    float4 tex = gDustTexture.Sample(gDustSampler, input.UV);
    clip(tex.a - 0.08f);
    float3 color = DustColor.rgb * tex.rgb * radial * tint;
    return float4(color, 1.0f);
}
