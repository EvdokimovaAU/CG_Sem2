struct DustParticle
{
    float3 Position;
    float Size;
    float3 Velocity;
    float Seed;
    float Age;
    float Lifetime;
    float Kind;
};

cbuffer ParticleSimulationCB : register(b0)
{
    float4 DeltaTimeTime;
    float4 BoundsMin;
    float4 BoundsMax;
    float4 NoiseParams;
    float4 EmitterPosition;
    float4 SphereData;
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
    float Life01 : LIFE;
    float Kind : KIND;
};

struct GSOutput
{
    float4 PositionH : SV_POSITION;
    float2 UV : TEXCOORD0;
    float Seed : TEXCOORD1;
    float Life01 : TEXCOORD2;
    float Kind : TEXCOORD3;
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

float Hash1(float value)
{
    return frac(sin(value * 91.3458f) * 47453.5453f);
}

float3 RandomUnitVector(float seed)
{
    float z = Hash1(seed * 1.37f) * 2.0f - 1.0f;
    float angle = Hash1(seed * 2.11f) * 6.2831853f;
    float radius = sqrt(saturate(1.0f - z * z));
    return float3(cos(angle) * radius, z, sin(angle) * radius);
}

void RespawnParticle(inout DustParticle particle, float time)
{
    float3 emitter = EmitterPosition.xyz;
    float emitterRadius = max(EmitterPosition.w, 0.01f);
    float baseSeed = particle.Seed + time * 0.37f;

    float3 shellDirection = RandomUnitVector(baseSeed + 13.0f);
    float3 tangentNoise = RandomUnitVector(baseSeed + 23.0f);
    float3 spawnOffset = shellDirection * lerp(emitterRadius * 0.75f, emitterRadius, Hash1(baseSeed + 7.0f));
    float3 launchDirection = normalize(shellDirection + tangentNoise * 0.35f);
    float speed = lerp(18.0f, 34.0f, Hash1(baseSeed + 31.0f));

    particle.Position = emitter + spawnOffset;
    particle.Velocity = launchDirection * speed;
    particle.Size = lerp(3.8f, 7.2f, Hash1(baseSeed + 43.0f));
    particle.Age = 0.0f;
    particle.Lifetime = lerp(2.4f, 4.1f, Hash1(baseSeed + 59.0f));
    particle.Kind = 0.0f;
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
    float3 sphereCenter = SphereData.xyz;
    float sphereRadius = SphereData.w;

    if (particle.Kind > 0.5f)
    {
        float basePhase = particle.Seed * 3.1f + time * 0.55f;
        float3 fromCenter = particle.Position - sphereCenter;
        float len = max(length(fromCenter), 0.001f);
        float3 normal = fromCenter / len;
        float3 tangentA = normalize(cross(float3(0.0f, 1.0f, 0.0f), normal));
        if (dot(tangentA, tangentA) < 0.001f)
        {
            tangentA = normalize(cross(float3(1.0f, 0.0f, 0.0f), normal));
        }
        float3 tangentB = normalize(cross(normal, tangentA));

        float tangentSpeedA = sin(basePhase) * 2.8f;
        float tangentSpeedB = cos(basePhase * 1.37f) * 2.2f;
        particle.Position += (tangentA * tangentSpeedA + tangentB * tangentSpeedB) * deltaTime;
        particle.Position = sphereCenter + normalize(particle.Position - sphereCenter) * sphereRadius;
        particle.Age += deltaTime;
        gParticleOutput.Append(particle);
        return;
    }

    float phase = particle.Seed + time;
    float3 noiseVelocity = float3(
        sin(phase * 0.73f) + sin(phase * 1.91f + particle.Position.z * 0.01f) * 0.35f,
        sin(phase * 0.41f + 1.7f) * 0.2f,
        cos(phase * 0.67f + 0.9f) + cos(phase * 1.57f + particle.Position.x * 0.01f) * 0.35f);

    float3 velocity = particle.Velocity;
    velocity += noiseVelocity * NoiseParams.xxx;
    velocity.y += NoiseParams.y * deltaTime;
    velocity *= (1.0f - NoiseParams.z * deltaTime);

    particle.Position += velocity * deltaTime;
    particle.Velocity = velocity;
    particle.Age += deltaTime;

    float collisionRadius = sphereRadius + particle.Size * 0.28f;
    float3 offsetFromSphere = particle.Position - sphereCenter;
    float distanceToSphere = length(offsetFromSphere);
    if (distanceToSphere < collisionRadius)
    {
        float3 normal = distanceToSphere > 0.0001f
            ? offsetFromSphere / distanceToSphere
            : float3(0.0f, 1.0f, 0.0f);
        particle.Position = sphereCenter + normal * collisionRadius;
        particle.Velocity = reflect(particle.Velocity, normal) * NoiseParams.w;
    }

    bool outOfBounds =
        particle.Position.x < BoundsMin.x || particle.Position.x > BoundsMax.x ||
        particle.Position.y < BoundsMin.y || particle.Position.y > BoundsMax.y ||
        particle.Position.z < BoundsMin.z || particle.Position.z > BoundsMax.z;

    if (particle.Age >= particle.Lifetime || outOfBounds)
    {
        RespawnParticle(particle, time);
    }

    gParticleOutput.Append(particle);
}

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    DustParticle particle = gParticles[vertexId];

    VSOutput output;
    output.Center = particle.Position;
    float life01 = saturate(particle.Age / max(particle.Lifetime, 0.001f));
    output.Size = particle.Size * lerp(0.55f, 1.0f, saturate(life01 * 5.0f)) * lerp(1.0f, 0.65f, life01);
    output.Seed = particle.Seed;
    output.Life01 = life01;
    output.Kind = particle.Kind;
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
        output.Life01 = input[0].Life01;
        output.Kind = input[0].Kind;
        stream.Append(output);
    }
}

float4 PSMain(GSOutput input) : SV_TARGET
{
    float2 centeredUv = input.UV * 2.0f - 1.0f;
    float radial = saturate(1.0f - dot(centeredUv, centeredUv) * 0.55f);
    float tint = 0.82f + 0.18f * frac(input.Seed * 1.6180339f);
    float4 tex = gDustTexture.Sample(gDustSampler, input.UV);
    if (input.Kind > 0.5f)
    {
        float sphereAlpha = tex.a * radial * 0.95f;
        clip(sphereAlpha - 0.04f);
        float sparkle = 0.85f + 0.15f * sin(input.Seed * 23.0f);
        float3 sphereColor = lerp(float3(0.14f, 0.64f, 1.0f), float3(0.74f, 0.92f, 1.0f), radial) * sparkle;
        return float4(sphereColor, sphereAlpha);
    }

    float fadeIn = saturate(input.Life01 * 6.0f);
    float fadeOut = saturate((1.0f - input.Life01) * 2.4f);
    float alpha = tex.a * radial * fadeIn * fadeOut;
    clip(alpha - 0.03f);
    float3 youngColor = DustColor.rgb;
    float3 oldColor = lerp(DustColor.rgb, float3(0.72f, 0.90f, 1.0f), 0.55f);
    float3 color = lerp(youngColor, oldColor, input.Life01) * tex.rgb * tint;
    return float4(color, alpha);
}
