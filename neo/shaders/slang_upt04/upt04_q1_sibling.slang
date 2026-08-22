import upt04_backend_rayquery;
import upt04_hit_contract;
import upt04_geometry_contract;
import upt04_geometry_decode;
import upt04_initial_bindings;
import upt04_initial_control;
import upt04_initial_shared;
import upt04_initial_work_contract;
import upt03_reservoir_contract;
import upt04_transport_initial_bounded;
import upt04_initial_indirect;
import upt04_initial_multibounce;
import upt04_initial_reservoir_stream;
import upt04_light_contract;
import upt05_material_set_openpbr;
#define UPT_SELECTED_MATERIAL_SET Upt05OpenPbrMaterialSet
import upt04_x3_split;
import upt04_q1_split;

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint compactCount = Upt04Q1QueuedCount();
    if (dispatchThreadId.x >= compactCount)
        return;
    const Upt04Q1Token token = Upt04Q1QueuedToken(dispatchThreadId.x);
    Upt04InitialTraceWork work;
    Upt04HitFacts hit;
    Upt04LocalLightSelection selection;
    Upt04Q1Unpack(token, work, hit, selection);
    const uint surfaceIndex = work.surfaceIndex;
    if (surfaceIndex >= gUpt04Control.surfaceCount)
        return;
    const uint pixelY = surfaceIndex / gUpt04Control.renderWidth;
    const uint2 pixel = uint2(
        surfaceIndex - pixelY * gUpt04Control.renderWidth, pixelY);
    uint directVisibilityRays = 0u;
    Upt03LogicalReservoir reservoir = Upt04LoadIntermediateInitialReservoir(
        surfaceIndex, directVisibilityRays);
    const Upt04HitFacts continuationHit = hit;
    const Upt04IndirectHitTrialResult indirect =
        Upt04PrepareIndirectHitTrials<UPT_SELECTED_MATERIAL_SET>(
            work, continuationHit, pixel, selection);
    Upt04StreamInitialTraceWork(
        reservoir, indirect.endpointCandidate, pixel, 1u);
    Upt04InitialTraceWork secondaryNee = indirect.neeWork;
    hit = Upt04MissFacts(secondaryNee.ray.tMax);
    if (secondaryNee.valid != 0u && secondaryNee.traceRequired != 0u)
        hit = Upt04TraceIndirectVisibilityRayQuery(
            secondaryNee.ray,
            Upt04IndirectNeeVisibilityIgnore(secondaryNee));
    secondaryNee = Upt04AdvanceInitialTraceWork(secondaryNee, hit);
    Upt04StreamInitialTraceWork(reservoir, secondaryNee, pixel, 2u);

    Upt04SecondContinuationResult preparedX3 = (Upt04SecondContinuationResult)0;
    if (work.valid != 0u && continuationHit.status == kUpt04HitTriangle)
    {
        const Upt04GeometryHitSurface queuedSurface =
            Upt04DecodeGeometryHit(continuationHit);
        if (queuedSurface.valid != 0u)
        {
            Upt04InitialTraceWork secondSource = work;
            if (indirect.neeWork.valid != 0u)
            {
                secondSource.transport.budget.visibilityRays = max(
                    secondSource.transport.budget.visibilityRays,
                    indirect.neeWork.transport.budget.visibilityRays);
            }
            preparedX3 = Upt04PrepareSecondContinuation<UPT_SELECTED_MATERIAL_SET>(
                secondSource, queuedSurface);
            if (preparedX3.work.valid != 0u
                && preparedX3.work.traceRequired != 0u)
            {
                Upt04AppendX3Token(
                    surfaceIndex,
                    preparedX3,
                    queuedSurface.position,
                    secondSource.transport.budget.visibilityRays);
            }
        }
    }
    if (preparedX3.work.valid != 0u && preparedX3.work.traceRequired != 0u)
    {
        Upt04WriteIntermediateInitialReservoir(
            surfaceIndex, reservoir, directVisibilityRays);
        return;
    }
    Upt04WriteInitialReservoir(surfaceIndex, reservoir);
}