Unified ReSTIR PT Slang Test Renderer
====================================

Status
------

UPT-00 through UPT-06 are complete through the accepted direct no-reuse and
two-page history contracts. P0/D0/R0 produced a functional textured 2560 x
1440 image at the user's 120 FPS cap. The compact32 receiver plus cold history
sidecar is accepted, authored vertex smoothing is preserved, and both attempted
D0 splits remain rejected because they serialized or duplicated traversal and
were slower than the roughly 0.9 ms monolithic result.

UPT-07 is a live, opt-in primary-direct temporal pass. It uses a dithered
nine-probe surface-only reprojection search, stable manager light identity,
host frame-serial page admission, current-domain sample replay, and at most one
winner-only visibility RayQuery. UPT-08 is implemented and defaults on inside
the opt-in route: U0/U1 publish a stable sample identity, count correlation in
a 17 x 17 neighborhood, and adapt the next temporal history-M cap without a
per-frame clear. UPT-09 is a live, opt-in direct spatial pass over the same two
64-byte pages. Its accepted sub-policy defaults are unique-neighbor mode 0,
direct proposal parity, RTXDI target-PDF parity, duplication control, and
history M/age 32. Temporal and spatial themselves remain default off.

The UPT-only portal/player-position emissive acquisition loss is closed. D0 now
uses the exact current emissive-CDF publication and an accepted legacy-shaped
previous-best reseed. Runtime testing reports legacy-equivalent behavior: the
one-frame portal-border hitch remains, but emitters no longer suffer persistent
or large-scale loss.

UPT-43.4's accepted temporal replay-compaction postmortem is recorded in
59_upt43_temporal_replay_compaction_postmortem.txt. It contains the exact old
monolithic continuation-replay source shape, the classifier/queue/indirect-
consumer replacement, the measured 10.348 to 3.043 ms result, and the general
GPU work-topology lesson behind the 70.6 percent reduction.

The first bounded one-continuation unified indirect/secondary-NEE estimator is
now implemented and Vulkan-build verified. It keeps one shared initial
reservoir, performs no temporal or spatial indirect reuse, adds no buffer/page
or per-frame clear, and remains behind the existing opt-in unified family.
Direct-only remains the accepted baseline and previous-best must remain off for
the first estimator proof. The first runtime image exposed a secondary-NEE
identity regression: it sampled the full manager emissive range rather than
the exact current power CDF, reproducing disappearing and phantom emitters.
That bypass is corrected and Vulkan-build verified. The first post-correction
diagnostic capture then proved the one-draw secondary estimator itself was far
too sparse. A bounded inner RIS over the existing D0 candidate schedule is now
deployed and still traces visibility only for one selected winner. The gi2.txt
retest proved that loop is live but produced no visible improvement: visibility
attempts rose from 4,846 to 106,334 while PDF/ray rejection became the dominant
3,217,063-sample bucket. A direct legacy comparison then found that UPT decoded
the committed front-face bit but left secondary geometric and shading normals
in raw mesh-winding orientation. Secondary-hit normals are now oriented to the
incoming continuation, matching the legacy surface adapter. The focused and
full Vulkan builds pass. The final shared-module rebuild produced and deployed
33 UPT-04, four UPT-07, and two UPT-09 Vulkan blobs. Runtime testing showed no
visible improvement, so normal orientation was valid but not the dominant
failure.

A mirror-material experiment then exposed false-color/placeholder textures on
secondary hits. The source mismatch was exact: UPT multiplied every diffuse
texture by material-ID debugAlbedo, skipped Doom YCoCg decoding, and multiplied
all surfaces by vertex color. Legacy uses debugAlbedo only as fallback/forced
debug color, decodes flagged YCoCg textures when r_pathTracingTextureDecode is
enabled, and applies vertex color only to GUI-screen translucent surfaces. UPT
now follows that convention. Focused and full Vulkan builds pass; the rebuilt
executable, exactly 23 affected production Vulkan UPT blobs, and final rebuilt
diagnostic blob are deployed. Runtime confirmed that the reflected textures
are fixed, but the mirror experiment also exposed missing world chunks, doors,
and rigid props. gi4.txt through gi7.txt prove that every committed
continuation hit decodes successfully. A portal-residency widening was rejected
by gi8 after it switched all static hits to bucket traversal, worsened holes,
duplicated reflected emissives, and caused a transient all-geometry loss; it is
rolled back. The direct legacy comparison found the UPT-only fault instead:
continuation rays used accept-first traversal rather than closest-hit traversal.
That flag is now removed only for surface continuations and retained for
visibility. The full Vulkan build passes; exactly 31 rebuilt UPT blobs and the
executable are deployed with hash parity. Runtime then accepted the geometry
fix, leaving only alpha-tested meshes blocking indirect light panels. The
production Vulkan RayQuery path now performs route-exact UV/material alpha
testing for continuation and secondary visibility candidates while preserving
the 48-byte compact material ABI and fail-closed geometry behavior. Focused and
full Vulkan builds pass and 26 rebuilt Vulkan UPT blobs are deployed with hash
parity. Runtime accepted the alpha-cutout result and reports all identified
basic indirect-lighting defects fixed. Multi-bounce, indirect reuse, and final
cost acceptance remain later work. See
13_upt04_corrections_and_paper_reference.txt and
23_handover_2026_08_08.txt for the exact checkpoint and test boundary.

Purpose
-------

Build a parallel, measurable test renderer for a unified direct/global ReSTIR
path-tracing reservoir. The first useful image must exist before temporal or
spatial reuse is added. This makes the initial path sampler, reservoir contract,
final estimator, ray count, shader size, and Vulkan execution cost observable
without reuse hiding defects.

This is not a refactor of the current clean DI or clean GI implementations. It
is a separate route with separate shaders, pipeline state, resources, markers,
and output. The current renderer remains the visual and runtime fallback.

Primary decisions
-----------------

1. Use Slang for the new shader lane, provisionally.

   The existing HLSL shaders remain on DXC. Slang and HLSL coexist in the tree.
   The new lane gets a Vulkan-only CMake target that invokes slangc directly and
   emits SPIR-V. Do not modify ShaderMake or translate unrelated HLSL.

   Slang is selected for its module system, explicit specialization, capability
   checking, direct SPIR-V output, ray-tracing support, and HLSL-like syntax.
   It is not accepted as faster by assertion. UPT-00 must compile the same small
   kernels with Slang and DXC and compare output, SPIR-V, cold pipeline build,
   and GPU timing before the renderer commits to Slang-only shaders.

2. Vulkan only.

   Do not add DXIL compilation, DX12 shader targets, or runtime DX12 branches.

3. Unified DI/GI reservoir from the start.

   One reservoir stream represents a selected light-transport contribution.
   Direct analytic/emissive/environment events and indirect continuation events
   use one candidate/status/weight contract. They do not write independent DI
   and GI reservoirs and get combined later.

4. Initial result before reuse.

   The first image consists of:

       existing primary-surface input
       one unified initial-sampling dispatch
       one trace-free final resolve dispatch

   No temporal, spatial, duplication map, boiling filter, denoiser, PSR,
   reflection, liquid-pool, particle, or cache pass is allowed in that timing.

5. No feature megakernel.

   A shader entry point owns one pass responsibility. Source modules may share
   typed math and contracts, but an entry point may not import every debug mode,
   material feature, reuse phase, and special effect. Every generated module is
   audited for reachable functions and SPIR-V size.

6. No unaccounted GPU work.

   Every buffer, texture, descriptor set, barrier, dispatch, and ray category
   needs an entry in 02_gpu_work_ledger.txt before implementation. "Might be
   useful later" is not sufficient justification for allocation or dispatch.

7. Invalid-sample and empty-pixel behavior are foundational.

   Invalid candidates never inflate reservoir M, weight sum, age, history, or
   sample identity. A valid receiver with an empty center reservoir remains
   eligible for temporal recovery and spatial neighbor rescue. Reuse must not
   assume the center already contains a sample.

8. Special Doom 3 features attach at explicit boundaries.

   Clear-window PSR, DLSS Ray Reconstruction guides, the liquid-pool modifier,
   screen-space particles, optional screen-space ReGIR/NEE cache proposals, and
   additional OpenPBR material types are planned now but are not compiled into
   the first kernel. The baseline opaque material provider is independently
   owned and selected by the composition root. See
   04_feature_extension_contract.txt.

9. Material implementations are not ReSTIR implementation details.

   Initial sampling, reservoir update, temporal reuse, spatial reuse, and
   resolve may consume a typed material-interaction contract. They may not
   contain Lambert, GGX, OpenPBR, glass, liquid, texture-decode, or material
   fallback formulas. Concrete material types live in independently owned
   modules and are selected by a thin composition root outside ReSTIR.

   Adding or replacing a material type must not require editing a ReSTIR pass,
   reservoir codec, proposal-mixture implementation, or reuse algorithm.

10. Reserve the random-sequence contract before writing stochastic passes.

   Blue noise itself is optional and is not a baseline binding. Pass/stream
   namespaces, random dimension ownership, replay keys, and the white-noise
   fallback are frozen early so a later blue-noise provider does not reorder
   decisions or alter reservoir semantics. See
   06_random_sequence_and_blue_noise.txt.

11. One bounce is an initial workload limit, not an architectural limit.

   The user-validated NVIDIA reference needed its three-bounce mode for good
   image quality and used aggressive Russian roulette to keep it affordable.
   The first renderer still traces only one continuation, but reservoir
   identity, path replay, random dimensions, material events, and transport
   state must be capable of representing at least the mapped equivalent of
   that three-bounce configuration without an ABI or ReSTIR rewrite.

   No extra bounce rays, queues, payload, buffers, or pipelines are admitted
   until the later measured task. See
   07_three_bounce_and_russian_roulette_readiness.txt.

Authority and reference boundary
--------------------------------

Algorithm authority:

    ReSTIR PT Enhanced public paper and project page
    https://research.nvidia.com/labs/rtr/publication/lin2026restirptenhanced/

Compiler authority:

    Slang official repository and documentation
    https://github.com/shader-slang/slang
    https://docs.shader-slang.org/en/latest/

Local behavioral/performance oracle:

    E:/prog/rtxdi_testing

Important local reference points:

    Samples/FullSample/Shaders/LightingPasses/PT/GenerateInitialSamples.hlsl
    Samples/FullSample/Shaders/LightingPasses/PT/TemporalResampling.hlsl
    Samples/FullSample/Shaders/LightingPasses/PT/SpatialResampling.hlsl
    Samples/FullSample/Shaders/LightingPasses/PT/FinalShading.hlsl
    Samples/FullSample/Shaders/LightingPasses/PT/FillSampleID.hlsl
    Samples/FullSample/Shaders/LightingPasses/PT/ComputeDuplicationMap.hlsl
    Libraries/Rtxdi/Include/Rtxdi/PT/Reservoir.hlsli
    Libraries/Rtxdi/Include/Rtxdi/PT/InitialSampling.hlsli
    Libraries/Rtxdi/Include/Rtxdi/PT/TemporalResampling.hlsli
    Libraries/Rtxdi/Include/Rtxdi/PT/SpatialResampling.hlsli

These NVIDIA files are behavioral and performance references. Do not copy their
shader bodies, comments, private layouts, or helper functions into a
GPL-distributable rbdoom implementation. Public paper equations, observed I/O
contracts, and rbdoom-owned implementations are the clean design authority.

Existing rbdoom knowledge to retain:

    docs/restir_pt_enhanced_research_review.md
    docs/restir_pt_agent_bridge_plan.txt
    docs/ReSTIR/current_restir_pt_state.txt
    docs/optimize/path_traced_performance_investigation_2026_07_31.md
    docs/modular_shaders/material_contract.txt
    docs/modular_shaders/openpbr_eon_ggx_vndf_steps.txt

Do not reuse current hot-path helpers without auditing their transitive shader
and host dependencies. Reusing a name or abstraction is not proof that its
workload is appropriate for the new renderer.

Initial non-goals
-----------------

    temporal reuse
    spatial reuse
    duplication map
    more than one indirect continuation bounce
    general transparent path transport
    glossy reflection reconstruction
    denoising or reconstruction
    dynamic shader compilation
    runtime Slang reflection
    importing the current DI/GI shader universe
    matching every current debug view

Packet index
------------

    01_slang_build_and_shader_architecture.txt
        Slang decision, coexistence, CMake boundary, ABI validation, and
        anti-megakernel rules.

    02_gpu_work_ledger.txt
        Mandatory admission form for GPU resources/work plus the planned
        baseline and future pass/resource ledger.

    03_unified_reservoir_contract.txt
        Logical reservoir, candidate validity, initial sampling, temporal and
        spatial recovery, page scheduling, and duplication-map planning.

    04_feature_extension_contract.txt
        OpenPBR, PSR, RR guides, liquid pools, particles, and optional proposal
        providers without polluting the core pass.

    05_tasks_and_validation.txt
        Ordered read-only, build, correctness, image, and performance gates.

    06_random_sequence_and_blue_noise.txt
        Stable stochastic-dimension ownership, provider isolation, blue-noise
        eligibility, and the pre-reuse A/B gate.

    07_three_bounce_and_russian_roulette_readiness.txt
        Three-bounce compatibility, roulette probability/replay semantics, and
        the later fixed-loop versus wavefront performance gate.

    08_upt00_toolchain_results.txt
        Implemented UPT-00 build boundary, ABI/readback proof, compiler/SPIR-V
        metrics, repeated GPU timings, and the first-pipeline order finding.

    09_upt01_live_input_ownership_audit.txt
        Completed UPT-01 live resource ledger, exact primary/scene/material/
        light ownership, missing generation publications, transport-neutral
        material boundary, lifecycle matrix, and host composition point.

    10_upt02_unified_reservoir_codec_results.txt
        Completed UPT-02 candidate-state derivation, 64-byte CPU codec, random
        dimension schedule, packing/memory budget, and three-bounce/RR vectors.

    11_upt03_gpu_abi_results.txt
        Completed UPT-03 deterministic Slang compute codec, exact CPU/GPU byte
        comparison, reflection/dependency gate, and bounded zero-ray ledger.

    12_upt04_initial_sampler_admission.txt
        UPT-04 GPU work admission and implementation record: corrected path-tree
        candidate streaming, exact three-ray ceiling, resource/module boundaries,
        mandatory host publications, fixed counter readback, offline shader
        closure, selected runtime wiring, exact current emissive-CDF count, and
        the bounded D0 previous-best emissive acquisition checkpoint.

    14_upt05_trace_free_resolve_results.txt
        UPT-05 resolve resource/dispatch admission, static SPIR-V closure,
        direct presentation path, and first live 1440p image result.

    15_upt05_openpbr_material_results.txt
        Static material-provider boundary, rbdoom-owned EON/GGX/VNDF baseline,
        Vulkan build and live image result, shader sizes, and disposition of
        the inherited Doom analytic-light range cutoff seen in resolve view 2.

    16_upt05_repeatability_probe.txt
        Fixed sample-index control and group-reduced 128-bit exact packed-
        reservoir signature using only the optional diagnostic buffer pair.

        SUPERSEDED IN PART by 13. Its initial-sampling design (one-draw
        technique mixture, M = 1, two-ray ceiling) does not match the cited
        paper. Read 13 before implementing or reviewing D0.

    17_upt05_nsight_timing_results.txt
        Four-capture 1440p P0/D0/R0 timing comparison, submit-scope stability,
        zero steady-frame Vulkan creation events, and remaining comparison
        gates.

    18_upt05_bandwidth_bottleneck_audit.txt
        Live-SPIR-V proof of the 176-byte receiver loads, unused UPT-only RR
        guide writes, 64-byte reservoir traffic, exact bytes/pixel budget, the
        implemented cleanup, and its runtime rejection as the root cause.

    19_upt05_payload_and_occupancy_audit.txt
        Exact early/current/reference payload sizes, unified D0 live-state and
        three-query megakernel evidence, and the deployed full-frame
        traversal-isolation proof mode 14, plus the UPT-only 336-byte versus
        172-byte compact-primary payload A/B.

    20_upt06_history_resource_admission.txt
        Two-page allocation/lifetime admission, host-owned page metadata,
        count-only zero-trial encoding, no-clear invalidation, and the exact
        temporal/spatial page-role schedules, including live acceptance and
        the compact binary16 Vulkan capability correction. D0 now advances and
        consumes the same two-page history even when T0/S0 are disabled.

    21_upt07_temporal_contract_slice.txt
        Nine-probe surface-only reprojection, finalized-weight oracle, stable
        light identity, frame-serial/page-role repair, one winner-only
        direct visibility ray, the opt-in traced one-continuation/secondary-NEE
        replay provider, and the live two-page T0 composition with no clears.

    22_upt09_spatial_rescue_slice.txt
        Bounded direct spatial reuse, non-recursive empty-center rescue,
        unique-neighbor and estimator proof modes, UPT-08 duplication control,
        accepted direct-reuse sub-policy defaults, and remaining limitations.

    23_handover_2026_08_08.txt
        Current branch/worktree state, known-good runtime controls, fixes that
        must not regress, the audited UPT-only emissive CDF-capacity failure,
        runtime-accepted D0 previous-best/CDF repair, the remaining legacy-like
        portal hitch, evidence locations, and the next implementation sequence.

    24_upt10_monolithic_first_optimization_plan.txt
        Measured monolithic-first performance sequence: prepared primary and
        secondary receiver closures, candidate live-range and specialization
        cleanup, independent T0/S0 migration gates, and an evidence-only split
        escalation while preserving one unified DI/GI reservoir.

    25_upt11_light_tile_presampling.txt
        Exact-PDF GPU light-tile A/B for the accepted split D0 path: separate
        emissive-CDF and ordinal-stratified analytic domains, coherent 8x8
        screen-tile selection, bounded resources, and total-cost gates.

    26_upt12_native_direct_light_view.txt
        Nsight warp/source evidence and the mechanical direct-side adoption of
        the existing native112 uint4 light view, with whole-load and ray-ceiling
        build gates plus the next indirect register/compaction boundary.

    27_upt13_indirect_endpoint_lifetime.txt
        Rejected UPT-12 runtime result, correction of the paper's replay-
        compaction scope, and the bounded D0b endpoint-before-secondary-NEE
        lifetime trial.

    28_upt14_secondary_nee_inverse_square.txt
        UPT-13 rollback and the paper-defined inverse-square NEE trial schedule:
        primary unchanged, first secondary vertex reduced from 33 to 9 default
        typed trials with matched D0/T0 PDF and replay contracts.

    29_upt15_temporal_forced_nee_reconnection.txt
        Section 6.2.3 forced selected-light reconnection for temporal secondary
        NEE, including the estimator-equivalent raw-target representation,
        stable remap/UV/PDF/visibility contract, and temporal performance gate.

    30_upt16_temporal_winner_only_indirect_visibility.txt
        Defers secondary-NEE shadow traversal until the temporal history sample
        wins, removes visibility from reciprocal target-only replay, preserves
        fail-closed D0 fallback, and reduces unified T0 from five to four static
        RayQuery sites.

    31_upt17_hybrid_shift_reconnection_audit.txt
        Confirms that T0 still replays every continuation, corrects the claim
        that secondary-NEE reservoirs retain the secondary vertex, rejects the
        current one-page 32-byte hit buffer as history storage, and defines the
        diagnostic and stable-identity gate for true early reconnection.

    32_upt17_static_hybrid_reconnection.txt
        Implements an ABI-neutral static x2 locator, paper-default footprint
        gate and BASIC-shift Jacobian, exact replay fallback, isolated Vulkan
        shader A/B, and aggregate route/visibility/admission diagnostics.

    33_upt18_temporal_route_state_diagnostics.txt
        Records the rejected UPT-17 timing and multi-ms temporal state
        variance, traces the event-dependent one/two indirect replay routes
        and light-payload remap effect, and adds isolated baseline counters and
        a controlled reset protocol.

    34_upt19_hardware_opaque_geometry_partition.txt
        Corrects the shared all-programmable BLAS/ray contract with conservative
        fixed-size opaque geometry chunks, exact hardware-to-source primitive
        remapping, Vulkan UPT/legacy ray-flag changes, rollback control and
        chunk-opacity diagnostics.

    35_upt20_temporal_indirect_replay_compaction.txt
        Uses the wall/orb split-continuation captures to isolate the remaining
        view-dependent cost to T0 indirect replay, then defines a bounded
        tracing-work compaction that preserves the unified reservoir, replay
        algebra, visibility rules, ray ceiling and no-clear contract.

    36_upt21_lambert_material_isolation.txt
        Adds a static constant-gray Lambert D0/T0 profiling specialization that
        omits OpenPBR receiver/secondary shading while preserving real emitter
        textures, alpha traversal, identities, reservoir algebra and ray count.

    37_upt22_temporal_bottleneck_ladder.txt
        Adds five cumulative static T0 probe shaders whose adjacent marker-time
        deltas isolate reprojection/history traffic, first replay, reciprocal
        cross evaluation, merge and final visibility under identical D0 input;
        modes 6--11 further split the measured reciprocal hotspot into
        bookkeeping, current/direct-cross targets, reciprocal traversal/decode
        and post-hit evaluation, including request-only and committed-hit-only
        boundaries around the second RayQuery.

    38_upt23_frozen_scene_isolation.txt
        Adds a persistent production-package freeze and a static-only flat
        geometry/analytic-light specialization so matched timing can separate
        scene/BVH work, GPU lookup/routing, traversal/reservoir work and
        OpenPBR shading without changing the production estimator.

    39_upt24_rigid_skinned_resolver.txt
        Uses the frozen/view evidence to narrow rigid/skinned current-hit route
        loads, removes the skinned header classification load, gives T0 alpha
        traversal the accepted material-first gate, and defines the measured
        escalation to a common instance/geometry resolver.

    40_upt25_emissive_temporal_pdf_lookup.txt
        Removes the redundant temporal emissive identity hash lookup after
        stable dense remapping by preserving the exact source PDF in compact64.

    41_upt26_alpha_clip_geometry_isolation.txt
        Adds a default-off destructive diagnostic that omits authored alpha-test
        surfaces from every RT geometry producer and performs a one-time cache
        invalidation, separating executed alpha traversal/material cost from
        the remaining UPT workload.

    42_upt27_executed_work_budgets.txt
        Adds a build-failing T0 structural budget and an atomics-free per-pixel
        executed-work capture for rays, programmable candidates, stable remaps,
        emissive hash loads, light/geometry/material loads and texture samples.

    43_upt28_diagnostic_configuration_parity.txt
        Decouples compact64 lights from compact geometry for split D0 and moves
        the temporal probe ladder onto production OpenPBR, allowing diagnostics
        to retain the measured-fast native-geometry/split-initial configuration.

    44_upt29_temporal_ray_site_workgroups.txt
        Records the temporal RayQuery site/workgroup investigation and the
        measured constraints on replay scheduling experiments.

    45_upt30_genuine_di_gi_reuse_unification.txt
        Owns the shared DI/GI temporal adapter, common GRIS merge and bounded
        disjoint reciprocal-pair spatial implementation and acceptance gates.

    46_upt31_shared_spatial_optimization_research.txt
        Audits shared-S0 emissive routes, paired scheduling, the stored source
        target invariant, instruction footprint and the controlled timing
        matrix for the first default-off UPT-31 experiment.

    47_upt32_full_occupancy_shared_spatial.txt
        Replaces interleaved half-lane reciprocal ownership with one mapping
        and one publication per lane through an 8x8 group-shared handoff,
        retaining the two-page ABI, exact GRIS estimator and four-ray ceiling.

    48_upt33_empty_center_rescue.txt
        Re-admits only explicitly marked empty centers through one reciprocal
        donor, preserves count-only confidence as a valid-zero technique, and
        stamps the result structurally non-donating for both direct and global
        paths.

    49_upt34_simultaneous_multi_neighbor_spatial.txt
        Defines the simultaneous canonical-plus-three-neighbor pairwise-GRIS
        estimator, distinct reciprocal workgroup maps, duplicate exclusion,
        one-partner rescue boundary and bounded no-new-buffer implementation;
        runtime-correct but rejected after a roughly 40 percent FPS loss.

    50_upt35_three_vertex_initial_transport.txt
        Maps the NVIDIA maxBounceDepth=3 preset to exactly two continuation
        rays after the primary receiver, adds fixed-q roulette and an x3
        endpoint to a separate D0-only artifact, and freezes the unbiased
        target/PDF, ABI, ray ceiling and reuse-admission boundaries.

    51_upt36_three_vertex_temporal_replay.txt
        Replays the accepted x1-x2-x3 endpoint exactly through persisted
        path-vertex randoms, q=0.8 survival, two closest-hit continuations and
        current-receiver endpoint/MIS evaluation in a separate bounded T0
        artifact while leaving spatial reuse disabled.

    52_upt37_three_vertex_termination_controls.txt
        Makes x2-to-x3 roulette probability runtime-configurable and adds the
        FullSample-shaped initial-only squared-L2 throughput cutoff before
        roulette/ray emission, with stored-q replay and history invalidation.

    53_upt38_three_vertex_spatial_replay.txt
        Extends only the accepted one-neighbor UPT-33 shared-spatial artifact
        with UPT-36 exact stored-q two-continuation replay, preserving the
        reservoir/binding ABI and rejecting the three-neighbor experiment.

    54_upt39_dlss_rr_bridge.txt
        Reuses the existing primary RR-guide producer and Streamline bridge for
        UPT beauty resolve without extra rays, guide resources or image copies,
        while deferring glass PSR until it can publish compact32 receivers.

    65_upt45_compact_clear_window_psr.txt
        Supersedes the rejected clean-DI adapter with a UPT-owned native Slang
        clear-window PSR: publish compact32/sidecar receivers before D0, keep
        late transparent composition distinct after R0, import no clean
        reservoir/wide-record ABI, and gate the feature on explicit
        register/occupancy and dependency-footprint evidence.

    66_upt45_native_psr_contract_audit.txt
        Completes S45.1: proves compact32 plus the sidecar replace the wide
        receiver, proves sparse stable material hashes must not become a GPU
        lookup, rejects the fat general geometry decoder, admits only one
        8-byte/pixel composition token, freezes the native producer/composer
        descriptor and register-profile gates, and checkpoints the native
        Vulkan implementation plus artifact hashes.

    55_upt40_rr_jitter_temporal_reprojection.txt
        Corrects D0 previous-best and T0 history addressing for the exact
        previous-frame RR projection jitter while leaving search dither,
        reservoir math, visibility and ray ceilings unchanged.

    56_upt41_shader_variant_reachability_pruning.txt
        Records the completed default-build pruning from a 144-variant
        D0/T0/S0 matrix to 17 production-reachable Vulkan inputs, while
        retaining the full matrices as explicit audit targets and preserving
        parity, diagnostics and optional multi-bounce/RR features.

    57_upt42_gaussian_reuse_texture_pairing.txt
        Retains the opt-in diagnostic that replaces S0's moving 8x8
        neighborhood with the paper's sigma-16, 254-pixel Gaussian reuse map.
        Its rejected 11 ms pair-list launch was replaced by screen-ordered
        leader election; corrected runtime acceptance is pending while UPT-33
        remains the production default.

    58_upt43_bounded_endpoint_reuse_architecture.txt
        Replaces universal reuse replay with the RTXDI/ReSTIR-PT-shaped bounded
        endpoint contract: cached emissive/NEE endpoints, trace-free x2
        reconnection, sparse later-prefix replay, paper-shaped two-stage paired
        spatial execution, work-budget diagnostics and staged rollback gates.
        S43.1 persists exact direct-emissive PDF state; S43.2 adds the exact
        current-frame emissive geometry sidecar in ABI 13 and removes general
        triangle routing from selected-emissive D0/T0/S0 replay.

    59_upt43_temporal_replay_compaction_postmortem.txt
        Explains the S43.4 70.6 percent temporal reduction with before/after
        code excerpts: full-screen exact replay was replaced by a cheap
        classifier plus a bounded 64x1 compact replay queue.

    60_upt43_temporal_boiling_filter.txt
        Adds the default-off, ray-free FullSample-shaped post-T0 boiling
        filter over finalized target-scaled UCW, including exact weight
        convention, 8x8 threshold math, runtime controls and acceptance gate.

    61_upt43_temporal_permutation_sampling.txt
        Adds an exact default-off NVIDIA PT first-history-tap shifted 4x4 XOR
        permutation A/B with no new specialization, storage, dispatch or rays,
        while deferring sparse disocclusion boost to a proper GRIS design.

    62_upt43_sparse_disocclusion_boost.txt
        Defines the remaining RTXDI PT enhanced layer as an rcLength-two,
        short-history-only compact spatial side lane that reuses S43.4 queue
        storage and never revives the rejected full-screen UPT-34 kernel.

    63_upt43_temporal_surface_validation.txt
        Audits the temporal disocclusion gate, restores compact sidecar
        surface-class separation and adopts the RTXDI PT/GI 0.6 geometric
        normal threshold without adding identity bandwidth or depth heuristics.

    64_upt44_production_preset.txt
        Consolidates the accepted compact D0, S43.4/S43.6 temporal and UPT-33
        spatial paths into one deterministic `exec upt` production preset that
        explicitly clears rejected and diagnostic console state.

    65_upt45_compact_clear_window_psr.txt
    66_upt45_native_psr_contract_audit.txt
        Track clear-window PSR ownership. S45.7 supersedes and rejects the
        secondary native mini-renderer described by the earlier rungs: clear
        panes now continue inside canonical lean P0, so the committed receiver
        inherits the ordinary alpha/material/skinned/indirect/RR contract.

    13_upt04_corrections_and_paper_reference.txt
        Correction of the initial sampler to the paper's path-tree formulation,
        plus a self-contained transcription of the ReSTIR PT Enhanced math
        (RIS/GRIS, reservoir tuple, technique index and MIS, hybrid shift and
        Jacobian, unified DI+GI initial sampling, RIS-based NEE, Russian
        roulette, measured costs) so the PDF does not have to be re-read.
        Also carries the live host-side blockers.

Historical reuse admission gate
-------------------------------

This gate admitted UPT-07 through UPT-09 and remains a regression checklist.
The no-reuse renderer had to:

    produces a correct unified direct/global image;
    reports exact ray and dispatch counts;
    has no invalid reservoir admissions;
    has stable output at fixed RNG input;
    has small, pass-specific SPIR-V modules;
    builds Vulkan pipelines in a normal bounded time;
    reaches the reference-comparison timing gate in UPT-05;
    eliminates the proven unused/duplicate no-reuse traffic in 18;
    resolves the payload/live-state occupancy gate in 19;
    and admits a bounded UPT-owned hot receiver ABI before neighbor reads.
