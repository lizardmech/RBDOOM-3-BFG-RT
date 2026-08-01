Unified ReSTIR PT Slang Test Renderer
====================================

Status
------

Preimplementation architecture packet. No renderer or shader implementation is
authorized by this packet alone.

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

Stop condition
--------------

Do not add temporal or spatial reuse unless the no-reuse renderer:

    produces a correct unified direct/global image;
    reports exact ray and dispatch counts;
    has no invalid reservoir admissions;
    has stable output at fixed RNG input;
    has small, pass-specific SPIR-V modules;
    builds Vulkan pipelines in a normal bounded time;
    and reaches the reference-comparison timing gate in UPT-05.
