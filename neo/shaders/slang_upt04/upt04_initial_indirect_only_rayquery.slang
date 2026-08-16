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
import upt04_initial_reservoir_stream;
import upt05_material_set_openpbr;

#ifndef UPT04_MULTI_BOUNCE_INITIAL
#define UPT04_MULTI_BOUNCE_INITIAL 0
#endif
#if UPT04_MULTI_BOUNCE_INITIAL
import upt04_initial_multibounce;
#endif

#ifndef UPT04_SPLIT_INITIAL_INDIRECT
#define UPT04_SPLIT_INITIAL_INDIRECT 0
#endif

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= gUpt04Control.renderWidth || pixel.y >= gUpt04Control.renderHeight)
        return;
    const uint surfaceIndex = pixel.y * gUpt04Control.renderWidth + pixel.x;
    if (surfaceIndex >= gUpt04Control.surfaceCount)
        return;

    uint directVisibilityRays = 0u;
#if UPT04_SPLIT_INITIAL_INDIRECT
    Upt03LogicalReservoir reservoir = Upt04LoadIntermediateInitialReservoir(
        surfaceIndex,
        directVisibilityRays);
#else
    Upt03LogicalReservoir reservoir = (Upt03LogicalReservoir)0;
#endif
    Upt04BoundedTransportState transport = Upt04BeginBoundedTransport();
#if UPT04_SPLIT_INITIAL_INDIRECT
    transport.budget.visibilityRays = directVisibilityRays;
#endif
    if (!Upt04CompletePrimaryNeeStage(transport))
    {
        Upt04WriteInitialReservoir(surfaceIndex, reservoir);
        return;
    }
    Upt04InitialTraceWork continuation =
        Upt04PrepareIndirectPathTreeWork<Upt05OpenPbrMaterialSet>(pixel, transport);
    Upt04HitFacts hit = Upt04MissFacts(continuation.ray.tMax);
    if (continuation.valid != 0u && continuation.traceRequired != 0u)
        hit = Upt04TraceRayQuery(continuation.ray);
    const Upt04IndirectHitTrialResult indirect =
        Upt04PrepareIndirectHitTrials<Upt05OpenPbrMaterialSet>(
            continuation, hit, pixel);
    Upt04StreamInitialTraceWork(
        reservoir,
        indirect.endpointCandidate,
        pixel,
#if UPT04_SPLIT_INITIAL_INDIRECT
        1u);
#else
        0u);
#endif

    Upt04InitialTraceWork secondaryNee = indirect.neeWork;
    hit = Upt04MissFacts(secondaryNee.ray.tMax);
    if (secondaryNee.valid != 0u && secondaryNee.traceRequired != 0u)
        hit = Upt04TraceIndirectVisibilityRayQuery(
            secondaryNee.ray,
            Upt04IndirectNeeVisibilityIgnore(secondaryNee));
    secondaryNee = Upt04AdvanceInitialTraceWork(secondaryNee, hit);
    Upt04StreamInitialTraceWork(
        reservoir,
        secondaryNee,
        pixel,
#if UPT04_SPLIT_INITIAL_INDIRECT
        2u);
#else
        1u);
#endif

#if UPT04_MULTI_BOUNCE_INITIAL
    const uint threeVertexProbe = Upt04ThreeVertexBottleneckProbe(
        gUpt04Control.indirectPolicyFlags);
    // Probe 1 retains the exact production x1/x2 path in this same x3-capable
    // shader but stops before any x2-to-x3 work. Modes 2-4 are cumulative so
    // their dispatch-time deltas locate continuation, proposal, and visibility
    // cost without comparing different PSOs.
    if (threeVertexProbe == 1u)
    {
        Upt04WriteInitialReservoir(surfaceIndex, reservoir);
        return;
    }

    // The x2 NEE visibility and x2->x3 continuation are sibling techniques.
    // Carry the visibility count into the second-continuation copy so its
    // bounded state accounts for all rays actually emitted by this pixel.
    Upt04InitialTraceWork secondSource = continuation;
    if (indirect.neeWork.valid != 0u)
    {
        secondSource.transport.budget.visibilityRays = max(
            secondSource.transport.budget.visibilityRays,
            indirect.neeWork.transport.budget.visibilityRays);
    }
    const Upt04SecondContinuationResult secondContinuation =
        Upt04PrepareSecondContinuation<Upt05OpenPbrMaterialSet>(
            secondSource, indirect.secondarySurface);
    hit = Upt04MissFacts(secondContinuation.work.ray.tMax);
    if (secondContinuation.work.valid != 0u
        && secondContinuation.work.traceRequired != 0u)
        hit = Upt04TraceRayQuery(secondContinuation.work.ray);
    const Upt04InitialTraceWork secondEndpoint =
        Upt04EvaluateSecondEndpoint<Upt05OpenPbrMaterialSet>(
            secondContinuation, indirect.secondarySurface, hit);
    Upt04StreamInitialTraceWork(
        reservoir,
        secondEndpoint,
        pixel,
#if UPT04_SPLIT_INITIAL_INDIRECT
        3u);
#else
        2u);
#endif

    if (threeVertexProbe == 2u)
    {
        Upt04WriteInitialReservoir(surfaceIndex, reservoir);
        return;
    }

    Upt04InitialTraceWork thirdNee = (Upt04InitialTraceWork)0;
    if (hit.status == kUpt04HitTriangle)
    {
        const Upt04GeometryHitSurface thirdSurface =
            Upt04DecodeGeometryHit(hit);
        thirdNee = Upt04PrepareThirdVertexNee<Upt05OpenPbrMaterialSet>(
            secondContinuation, thirdSurface, pixel);
    }
    hit = Upt04MissFacts(thirdNee.ray.tMax);
    const bool forceKnownVisible = threeVertexProbe == 3u
        || threeVertexProbe == 5u
        || threeVertexProbe == 7u
        || threeVertexProbe == 9u
        || threeVertexProbe == 11u
        || threeVertexProbe == 12u
        || threeVertexProbe == 13u
        || threeVertexProbe == 14u
        || threeVertexProbe == 15u;
    if (forceKnownVisible
        && thirdNee.valid != 0u
        && thirdNee.traceRequired != 0u)
    {
        // Retain the selected x3 NEE candidate so the compiler cannot erase
        // proposal work, but deliberately replace only its final visibility
        // query with a known-visible result for this diagnostic mode.
        thirdNee.traceRequired = 0u;
        thirdNee.candidateReady = 1u;
        thirdNee.candidateFlags |= kUpt04CandidateFlagVisibilityKnown
            | kUpt04CandidateFlagVisibilityPassed;
    }
    else if (thirdNee.valid != 0u && thirdNee.traceRequired != 0u)
        hit = Upt04TraceIndirectVisibilityRayQuery(
            thirdNee.ray,
            Upt04IndirectNeeVisibilityIgnore(thirdNee));
    if (!forceKnownVisible)
        thirdNee = Upt04CompleteIndirectNeeVisibility(thirdNee, hit);
    Upt04StreamInitialTraceWork(
        reservoir,
        thirdNee,
        pixel,
#if UPT04_SPLIT_INITIAL_INDIRECT
        4u);
#else
        3u);
#endif
#endif
    Upt04WriteInitialReservoir(surfaceIndex, reservoir);
}
