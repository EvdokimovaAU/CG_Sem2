struct FireParticle
{
    float3 Position;
    float Size;
    float3 Velocity;
    float Seed;
};

cbuffer FireSimulationCB : register(b0)
{
    float4 DeltaTimeTime;
    float4 BoundsMin;
    float4 BoundsMax;
    float4 FireOrigin;
};

cbuffer FireRenderCB : register(b1)
{
    float4x4 ViewProj;
    float4 CameraRight;
    float4 CameraUp;
    float4 FireColor;
    float4 EffectParams;
};

ConsumeStructuredBuffer<FireParticle> gFireInput : register(u0);
AppendStructuredBuffer<FireParticle> gFireOutput : register(u1);
StructuredBuffer<FireParticle> gFireParticles : register(t0);

struct VSOutput
{
    float3 Center : POSITION;
    float Size : SIZE;
    float Seed : SEED;
    float Height01 : HEIGHT;
};

struct GSOutput
{
    float4 PositionH : SV_POSITION;
    float2 UV : TEXCOORD0;
    float Height01 : TEXCOORD1;
    float Seed : TEXCOORD2;
};

float Hash1(float value)
{
    return frac(sin(value * 91.3458f) * 47453.5453f);
}

[numthreads(64, 1, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    if (dispatchThreadID.x >= (uint)DeltaTimeTime.z)
    {
        return;
    }

    FireParticle particle = gFireInput.Consume();
    float deltaTime = DeltaTimeTime.x;
    float time = DeltaTimeTime.y;
    float height01 = saturate((particle.Position.y - BoundsMin.y) / max(BoundsMax.y - BoundsMin.y, 0.001f));

    float driftX =
        sin(time * 6.3f + particle.Seed * 19.0f) * 4.5f +
        sin(time * 11.1f + particle.Seed * 37.0f) * 1.8f;
    float driftZ =
        cos(time * 5.4f + particle.Seed * 13.0f) * 4.2f +
        cos(time * 9.7f + particle.Seed * 31.0f) * 1.6f;
    float rise = particle.Velocity.y * lerp(1.15f, 0.72f, height01);
    particle.Position.x += (particle.Velocity.x + driftX * lerp(0.55f, 1.15f, height01)) * deltaTime;
    particle.Position.y += rise * deltaTime;
    particle.Position.z += (particle.Velocity.z + driftZ * lerp(0.55f, 1.15f, height01)) * deltaTime;
    particle.Size *= lerp(1.0f, 0.965f, deltaTime * 60.0f);

    if (particle.Position.y > BoundsMax.y)
    {
        float rx = Hash1(particle.Seed * 17.0f + time);
        float rz = Hash1(particle.Seed * 29.0f + time * 0.7f);
        float rs = Hash1(particle.Seed * 43.0f + time * 0.3f);
        float rv = Hash1(particle.Seed * 53.0f + time * 0.9f);

        particle.Position = float3(
            FireOrigin.x + (rx * 2.0f - 1.0f) * 12.0f,
            FireOrigin.y + rs * 8.0f,
            FireOrigin.z + (rz * 2.0f - 1.0f) * 12.0f);
        particle.Size = 4.2f + rs * 5.6f;
        particle.Velocity.x = (rx * 2.0f - 1.0f) * 5.0f;
        particle.Velocity.y = 22.0f + rv * 24.0f;
        particle.Velocity.z = (rz * 2.0f - 1.0f) * 5.0f;
    }

    gFireOutput.Append(particle);
}

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    FireParticle particle = gFireParticles[vertexId];

    VSOutput output;
    output.Center = particle.Position;
    output.Height01 = saturate((particle.Position.y - EffectParams.x) / max(EffectParams.y - EffectParams.x, 0.001f));
    output.Size = particle.Size * lerp(1.0f, 0.55f, output.Height01);
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
        output.Height01 = input[0].Height01;
        output.Seed = input[0].Seed;
        stream.Append(output);
    }
}

float4 PSMain(GSOutput input) : SV_TARGET
{
    float2 uv = input.UV * 2.0f - 1.0f;
    float radial = saturate(1.0f - dot(uv, uv));
    float vertical = saturate(1.0f - input.UV.y);
    float wobble = 0.85f + 0.15f * sin(input.Seed * 41.0f + uv.x * 7.0f + input.Height01 * 9.0f);
    float core = pow(radial, 0.70f) * pow(vertical, 0.42f) * wobble;
    clip(core - 0.24f);

    float3 baseColor = lerp(float3(1.0f, 0.95f, 0.45f), float3(1.0f, 0.24f, 0.02f), input.Height01);
    float flicker = 0.80f + 0.20f * frac(input.Seed * 13.0f + input.Height01 * 1.7f);
    float3 color = baseColor * FireColor.rgb * core * flicker;
    return float4(saturate(color), 1.0f);
}
