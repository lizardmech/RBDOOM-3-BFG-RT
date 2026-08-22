import upt03_reservoir_contract;
import upt04_initial_bindings;
import upt04_reservoir_image;
import upt04_primary_image;
import upt04_geometry_contract;
import upt04_geometry_bindings;
import upt04_geometry_decode;
import upt04_emissive_replay;
import upt04_material_set_live_probe;
import upt04_primary_surface_live_contract;
import upt04_hit_contract;
import upt04_light_contract;

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= gUpt04Control.renderWidth || pixel.y >= gUpt04Control.renderHeight)
        return;
    const uint index = pixel.y * gUpt04Control.renderWidth + pixel.x;
    if (index >= gUpt04Control.surfaceCount)
        return;

    const Upt04PrimarySurfaceRecord primary =
        Upt04LoadPrimarySurfaceRecord(index);
    Upt04HitFacts hit = Upt04MissFacts(0.0f);
    hit.status = kUpt04HitTriangle;
    hit.instanceId = primary.instancePrimitiveObject.x;
    hit.geometryId = 0u;
    hit.primitiveId = primary.instancePrimitiveObject.y;
    hit.barycentrics = float2(0.33333334f, 0.33333334f);
    hit.frontFace = 1u;
    const Upt04GeometryHitSurface decoded = Upt04DecodeGeometryHit(hit);

    Upt04EmissiveGeometrySample replay = (Upt04EmissiveGeometrySample)0;
    if (gUpt04Control.currentLightCount > 0u)
    {
        replay = Upt04ReplayEmissiveTriangleGeometry(
            gUpt04CurrentLights[0],
            primary.worldPositionAndViewDepth.xyz,
            float2(0.25f, 0.75f),
            (gUpt04Control.availabilityFlags
                & kUpt04EmissiveReplayTwoSided) != 0u);
    }
    const float3 textureProbe = Upt04MaterialBindingProbe(
        decoded.materialIndex,
        decoded.texCoord);

    Upt03PackedReservoir packed = (Upt03PackedReservoir)0;
    packed.word0 = decoded.valid;
    packed.word1 = decoded.materialIndex;
    packed.word2 = decoded.triangleClassAndFlags;
    packed.word3 = replay.status;
    packed.word4 = replay.replayIndex;
    packed.word5 = asuint(replay.solidAnglePdf);
    packed.word6 = asuint(textureProbe.x);
    Upt04StorePackedReservoir(index, packed);
}