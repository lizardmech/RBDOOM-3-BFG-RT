#include "RtxdiBridge/RAB_UnifiedLightRecord.hlsli"

struct ParticleLightingConstants
{
    uint taskCount;
    uint lightCount;
    uint candidateCount;
    uint traceVisibility;
    float ambientFloor;
    float rayTMin;
    float rayTMaxBias;
    float directClamp;
    float emissiveScale;
    float analyticScale;
    float temporalWeight;
    uint historyTaskCount;
};

struct ParticleCompositeLightingTask
{
    float3 centerWorld;
    uint stableParticleId;
    uint stablePrimitiveIndex;
    uint materialId;
    uint compatibility;
    uint historyIndex;
};

#ifdef SPIRV
[[vk::push_constant]] ConstantBuffer<ParticleLightingConstants> ParticleLightingParams;
#else
ConstantBuffer<ParticleLightingConstants> ParticleLightingParams : register(b0);
#endif

RaytracingAccelerationStructure ParticleScene : register(t0);
StructuredBuffer<ParticleCompositeLightingTask> ParticleLightingTasks : register(t1);
StructuredBuffer<PathTraceUnifiedLightRecord> ParticleLights : register(t2);
RWStructuredBuffer<float4> ParticleLightingOutput : register(u3);
StructuredBuffer<float4> ParticleLightingHistory : register(t4);

static const float PARTICLE_LIGHT_PI = 3.14159265358979323846;

uint ParticleHash(uint value)
{
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    value ^= value >> 16u;
    return value;
}

float ParticleRandom01(inout uint state)
{
    state = ParticleHash(state + 0x9e3779b9u);
    return float(state & 0x00ffffffu) * (1.0 / 16777216.0);
}

float ParticleLuminance(float3 value)
{
    return dot(max(value, float3(0.0, 0.0, 0.0)), float3(0.2126, 0.7152, 0.0722));
}

bool ParticleEvaluateLight(
    PathTraceUnifiedLightRecord light,
    float3 center,
    out float3 contribution,
    out float3 direction,
    out float traceDistance)
{
    contribution = float3(0.0, 0.0, 0.0);
    direction = float3(0.0, 0.0, 1.0);
    traceDistance = 0.0;
    if (light.sourceIndex == PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX || light.sourceWeight <= 0.0)
    {
        return false;
    }

    const float3 toLight = light.positionAndRadius.xyz - center;
    const float distanceSquared = max(dot(toLight, toLight), 1.0e-4);
    const float distance = sqrt(distanceSquared);
    direction = toLight / distance;
    float radianceScale = 0.0;
    if (light.type == PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC)
    {
        radianceScale = ParticleLightingParams.analyticScale;
    }
    else if (light.type == PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE)
    {
        radianceScale = ParticleLightingParams.emissiveScale;
    }
    const float3 radiance =
        max(light.radianceAndLuminance.rgb, float3(0.0, 0.0, 0.0)) * radianceScale;

    if (light.type == PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC)
    {
        const float influenceRadius = light.uvOrDoomParams.x;
        const float sphereRadius = min(max(light.positionAndRadius.w, 0.01), max(influenceRadius, 0.01));
        if (influenceRadius <= 0.0 || distance > influenceRadius || ParticleLuminance(radiance) <= 0.0)
        {
            return false;
        }
        const float radiusFraction = saturate(distance / influenceRadius);
        const float influence = saturate(1.0 - radiusFraction * radiusFraction);
        const float sinTheta = saturate(sphereRadius / distance);
        const float solidAngle = 2.0 * PARTICLE_LIGHT_PI * (1.0 - sqrt(max(0.0, 1.0 - sinTheta * sinTheta)));
        contribution = radiance * influence * (solidAngle / (4.0 * PARTICLE_LIGHT_PI));
        traceDistance = max(distance - sphereRadius - ParticleLightingParams.rayTMaxBias, ParticleLightingParams.rayTMin);
        return ParticleLuminance(contribution) > 0.0;
    }

    if (light.type == PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE)
    {
        const float area = light.normalAndArea.w;
        const float normalLengthSquared = dot(light.normalAndArea.xyz, light.normalAndArea.xyz);
        if (normalLengthSquared <= 1.0e-8)
        {
            return false;
        }
        const float3 normal = light.normalAndArea.xyz * rsqrt(normalLengthSquared);
        const float facing = saturate(dot(normal, -direction));
        if (area <= 1.0e-6 || facing <= 0.0 || ParticleLuminance(radiance) <= 0.0)
        {
            return false;
        }
        contribution = radiance * (area * facing / max(distanceSquared, 1.0e-4)) / (4.0 * PARTICLE_LIGHT_PI);
        traceDistance = max(distance - ParticleLightingParams.rayTMaxBias, ParticleLightingParams.rayTMin);
        return ParticleLuminance(contribution) > 0.0;
    }

    return false;
}

bool ParticleVisible(float3 center, float3 direction, float traceDistance)
{
    if (ParticleLightingParams.traceVisibility == 0u || traceDistance <= ParticleLightingParams.rayTMin)
    {
        return true;
    }
    RayDesc ray;
    ray.Origin = center + direction * ParticleLightingParams.rayTMin;
    ray.Direction = direction;
    ray.TMin = ParticleLightingParams.rayTMin;
    ray.TMax = traceDistance;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE> query;
    query.TraceRayInline(
        ParticleScene,
        RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE,
        0xff,
        ray);
    while (query.Proceed())
    {
    }
    return query.CommittedStatus() == COMMITTED_NOTHING;
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint taskIndex = dispatchThreadId.x;
    if (taskIndex >= ParticleLightingParams.taskCount)
    {
        return;
    }

    const ParticleCompositeLightingTask task = ParticleLightingTasks[taskIndex];
    float3 direct = float3(0.0, 0.0, 0.0);
    if (ParticleLightingParams.lightCount > 0u && ParticleLightingParams.candidateCount > 0u)
    {
        const uint sampleCount = min(ParticleLightingParams.candidateCount, ParticleLightingParams.lightCount);
        if (ParticleLightingParams.traceVisibility == 0u)
        {
            // Legacy smoke needs a stable, low-frequency estimate rather than
            // one stochastic visible light. The default visits the complete
            // active domain. Lower diagnostic caps use an evenly spaced,
            // compensated subset so task reordering cannot make cards flicker.
            const float subsetScale = float(ParticleLightingParams.lightCount) / float(sampleCount);
            for (uint sampleIndex = 0u; sampleIndex < sampleCount; ++sampleIndex)
            {
                const uint lightIndex = ParticleLightingParams.lightCount <= sampleCount
                    ? sampleIndex
                    : min(
                        uint((float(sampleIndex) + 0.5) * float(ParticleLightingParams.lightCount) / float(sampleCount)),
                        ParticleLightingParams.lightCount - 1u);
                float3 candidateContribution;
                float3 candidateDirection;
                float candidateDistance;
                if (ParticleEvaluateLight(
                    ParticleLights[lightIndex],
                    task.centerWorld,
                    candidateContribution,
                    candidateDirection,
                    candidateDistance))
                {
                    direct += candidateContribution * subsetScale;
                }
            }
        }
        else
        {
            uint randomState = ParticleHash(task.stableParticleId ^ (taskIndex * 0x85ebca6bu) ^ 0xc2b2ae35u);
            float weightSum = 0.0;
            float selectedTarget = 0.0;
            float3 selectedContribution = float3(0.0, 0.0, 0.0);
            float3 selectedDirection = float3(0.0, 0.0, 1.0);
            float selectedDistance = 0.0;

            for (uint sampleIndex = 0u; sampleIndex < sampleCount; ++sampleIndex)
            {
                const uint lightIndex = ParticleLightingParams.lightCount <= sampleCount
                    ? sampleIndex
                    : min(uint(ParticleRandom01(randomState) * ParticleLightingParams.lightCount), ParticleLightingParams.lightCount - 1u);
                float3 candidateContribution;
                float3 candidateDirection;
                float candidateDistance;
                if (!ParticleEvaluateLight(
                    ParticleLights[lightIndex],
                    task.centerWorld,
                    candidateContribution,
                    candidateDirection,
                    candidateDistance))
                {
                    continue;
                }
                const float target = ParticleLuminance(candidateContribution);
                const float weight = target * float(ParticleLightingParams.lightCount);
                weightSum += weight;
                if (ParticleRandom01(randomState) * weightSum <= weight)
                {
                    selectedTarget = target;
                    selectedContribution = candidateContribution;
                    selectedDirection = candidateDirection;
                    selectedDistance = candidateDistance;
                }
            }

            if (selectedTarget > 0.0 && weightSum > 0.0 &&
                ParticleVisible(task.centerWorld, selectedDirection, selectedDistance))
            {
                direct = (selectedContribution / selectedTarget) * (weightSum / float(sampleCount));
            }
        }
    }

    direct = min(max(direct, float3(0.0, 0.0, 0.0)), ParticleLightingParams.directClamp.xxx);
    // Legacy cards need broad local color, not HDR surface radiance. Compress
    // direct illumination so bright lamps cannot drive white smoke to the same
    // value as its background and make the card appear to dissolve.
    const float3 compressedDirect = 0.65 * direct / (direct + 0.5);
    float3 localFill = min(
        ParticleLightingParams.ambientFloor.xxx + compressedDirect,
        float3(0.85, 0.85, 0.85));
    if (ParticleLightingParams.temporalWeight > 0.0 &&
        task.stableParticleId != 0u &&
        task.historyIndex < ParticleLightingParams.historyTaskCount)
    {
        const float3 previousFill = max(
            ParticleLightingHistory[task.historyIndex].rgb,
            float3(0.0, 0.0, 0.0));
        const float currentLuminance = ParticleLuminance(localFill);
        const float previousLuminance = ParticleLuminance(previousFill);
        // Preserve temporal stability for ordinary candidate variation while
        // rejecting history when a light switches on/off or changes strongly.
        const bool energyCompatible =
            previousLuminance <= currentLuminance * 4.0 + 0.05 &&
            currentLuminance <= previousLuminance * 4.0 + 0.05;
        if (energyCompatible)
        {
            localFill = lerp(
                localFill,
                previousFill,
                saturate(ParticleLightingParams.temporalWeight));
        }
    }
    ParticleLightingOutput[taskIndex] = float4(localFill, 1.0);
}
