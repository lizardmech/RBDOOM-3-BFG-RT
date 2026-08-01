import upt04_geometry_contract;
import upt04_geometry_bindings;
import upt04_geometry_decode;
import upt04_initial_bindings;
import upt04_light_contract;
import upt04_hit_contract;

public Upt04EmissiveGeometrySample Upt04ReplayEmissiveTriangleGeometry(
    Upt04UnifiedLightRecord light,
    float3 receiverPosition,
    float2 random)
{
    Upt04EmissiveGeometrySample sample = (Upt04EmissiveGeometrySample)0;
    if (light.type != kUpt04LightEmissiveTriangle
        || light.sourceIndex >= gUpt04Control.emissiveReplayCount
        || !all(isfinite(receiverPosition))
        || !all(isfinite(random)) || any(random < 0.0f) || any(random >= 1.0f))
        return sample;

    const Upt04EmissiveReplayRecord replay = gUpt04EmissiveReplay[light.sourceIndex];
    if (replay.instanceId != light.instanceId
        || replay.primitiveIndex != light.primitiveIndex
        || replay.materialIndex != light.materialOrLightId
        || replay.identityHashLo != light.identityA
        || replay.identityHashHi != light.identityB)
        return sample;

    const float squareRootU = sqrt(random.x);
    const float barycentric1 = random.y * squareRootU;
    const float barycentric2 = squareRootU - barycentric1;
    Upt04HitFacts replayHit = Upt04MissFacts(0.0f);
    replayHit.status = kUpt04HitTriangle;
    replayHit.instanceId = replay.instanceId;
    replayHit.geometryId = 0u;
    replayHit.primitiveId = replay.primitiveIndex;
    replayHit.barycentrics = float2(barycentric1, barycentric2);
    replayHit.frontFace = 1u;

    Upt04ResolvedTriangle triangle;
    if (!Upt04ResolveTriangle(replayHit, triangle)
        || triangle.materialIndex != replay.materialIndex)
        return sample;
    const Upt04GeometryHitSurface surface = Upt04BuildGeometryHitSurface(
        triangle,
        replayHit.barycentrics,
        1u);
    if (surface.valid == 0u)
        return sample;

    const float3 edge0 = triangle.vertex1.position.xyz - triangle.vertex0.position.xyz;
    const float3 edge1 = triangle.vertex2.position.xyz - triangle.vertex0.position.xyz;
    const float3 areaVector = cross(edge0, edge1);
    const float doubleAreaSquared = dot(areaVector, areaVector);
    if (!isfinite(doubleAreaSquared) || doubleAreaSquared <= 1.0e-12f)
        return sample;
    const float doubleArea = sqrt(doubleAreaSquared);
    const float area = 0.5f * doubleArea;
    float3 normal = areaVector / doubleArea;
    const float3 publishedNormal = Upt04GeometrySafeNormalize(
        light.normalAndArea.xyz,
        normal);
    if (dot(normal, publishedNormal) < 0.0f)
        normal = -normal;

    const float3 toLight = surface.position - receiverPosition;
    const float distanceSquared = dot(toLight, toLight);
    if (!isfinite(distanceSquared) || distanceSquared <= 1.0e-8f)
        return sample;
    const float distance = sqrt(distanceSquared);
    const float3 direction = toLight / distance;
    const float lightFacing = saturate(dot(normal, -direction));
    const float boundedFacing = max(lightFacing, 1.0e-8f);
    const float solidAnglePdf = distanceSquared / max(area * boundedFacing, 1.0e-8f);
    if (!Upt04FinitePositive(solidAnglePdf))
        return sample;

    sample.status = lightFacing > 0.0f
        ? kUpt04SampleValidPositive
        : kUpt04SampleValidZero;
    sample.materialIndex = triangle.materialIndex;
    sample.triangleClassAndFlags = triangle.triangleClassAndFlags;
    sample.replayIndex = light.sourceIndex;
    sample.position = surface.position;
    sample.normal = normal;
    sample.direction = direction;
    sample.distance = distance;
    sample.solidAnglePdf = solidAnglePdf;
    sample.texCoord = surface.texCoord;
    return sample;
}
