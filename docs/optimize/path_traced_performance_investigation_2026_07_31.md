# Path-traced performance investigation — 2026-07-31

## Scope

- Branch: `restir-development`
- Runtime API: Vulkan only
- Comparison target: modified NVIDIA ReSTIR PT sample in `E:\prog\rtxdi_testing`
- Reference captures: base DI, DI + GI, and DI + GI + DLSS Ray Reconstruction
- Marker switch: `r_pathTracingNsightGpuMarkers 1`
- Primary question: why do rbdoom's nominal temporal and spatial passes take roughly
  two to three times as long as the transplanted reference implementations?

This pass intentionally starts below optional glass, multibounce, reflection, and
denoiser features. Those features have measurable costs, but they do not explain
the base DI/temporal/spatial discrepancy.

## Correct production GI baseline

The minimal-room performance baseline must use DI view 4, GI view 0, and
`r_pathTracingCleanRestirGiResolve 1`. GI debug views are not a substitute for
displaying production GI: any nonzero GI view selects `debugRayTracing`, and
views 1 and 2 deliberately retain the full diagnostic producer shade path.

With the corrected route, the minimal room measures approximately 48 FPS.
The only tested GI control with a material frame-level effect is
`r_pathTracingCleanRestirGiMaxBounces`, at roughly 20 percent. All earlier
33--38 FPS GI-view-1 numbers and their proof-mode decomposition remain useful
only for understanding the debug/full-shade shader. They are not the production
view-0 baseline and must not drive root-cause attribution.

The max-bounce A/B itself contains two changes. At one bounce, view 0 can select
`FirstIndirectShadeFastRayGen`; at two bounces, `defaultOneSampleShade` becomes
false and the host selects the larger `FirstIndirectShadeRayGen`, then submits
the continuation trace and continuation shade dispatches. The observed 20
percent therefore combines a shader-permutation change with the actual second
bounce. Nsight marker durations, or a host-only permutation override, are
needed to split those costs.

`r_pathTracingCleanRestirGiForceFullShade` supplies that override. With view 0,
one bounce, one direct sample, direct probability 1, and secondary NEE cache
off, its only effect is choosing the full shade shader table instead of
ShadeFast. The controlled matrix is:

- one bounce, force-full 0: ShadeFast, no continuation;
- one bounce, force-full 1: full Shade, no continuation;
- two bounces: full Shade plus continuation trace/shade.

The second row minus the first measures shader-permutation shape. The third row
minus the second measures continuation work without conflating the ShadeFast
transition.

The compiled entrypoint shapes are substantially different despite the
controlled one-sample settings:

| Production entry | SPIR-V operations | Loads | Stores | Branches | Static `TraceRay` sites |
|---|---:|---:|---:|---:|---:|
| `FirstIndirectShadeFastRayGen` | 26,344 | 616 | 31 | 4,831 | 5 |
| `FirstIndirectShadeRayGen` | 63,322 | 1,492 | 70 | 11,391 | 12 |

The full entry is about 2.4 times the compiled operation count. The force-full
runtime comparison tests whether disabled dynamic branches from that larger
entry still impose register-pressure or occupancy cost at the original basic
one-sample workload.

Runtime result: forcing the full shade permutation at the otherwise identical
one-bounce production settings had no measurable FPS effect. The 2.4-times
larger static entrypoint is therefore not by itself imposing the observed
steady-state cost when its optional branches are disabled. Of the tested live
controls on the corrected production route, only max bounces and analytic-light
trials produced measurable changes.

### Delayed DI-view response with GI enabled

An observed DI view 16 to view 4 transition took more than 15 seconds before
the displayed frame rate rose when production GI resolve was enabled. With GI
disabled, switching between those DI views changed FPS immediately.

One-time Vulkan GI pipeline warmup was the first hypothesis. The renderer builds
16 separate GI RT pipelines, at most one per frame, with a 15-frame cooldown
between builds. Runtime testing disproved this explanation: the delay recurred
after the game had been running for a long time and after more than five
view-16/view-4 transitions.

Queued GPU work is also implausible. Vulkan presentation waits on the preceding
frame's graphics-queue event in the explicit triple-buffered frame cycle, so it
cannot accumulate a 15-second command backlog. The displayed FPS is averaged
over only six frames.

The apparent DI-buffer lifetime mismatch is not the cause either. View 4
dispatches only the initial DI raygen, while the GI host binding selects the
temporal DI buffer whenever the temporal CVar is enabled. However, the initial
raygen's `PathTraceCleanRoomStoreInitialReservoir` writes the newly generated
sample into both current and temporal reservoir storage. GI therefore receives
fresh view-4 DI data despite the misleading host-side selection expression.

GI temporal state is bounded much more tightly than the observed delay. The
production defaults cap confidence history at 4 and reservoir age at 12 frames,
with history rejected at the age limit. At the measured 30--50 FPS, legitimate
GI reservoir persistence should disappear in well under one second. The next
runtime discriminator is `r_pathTracingCleanRestirGiMaxHistoryLength 0` during
the same repeated view-16 -> view-4 transition. If the long delay remains, the
cause lies outside GI reservoir history and needs per-pass GPU timestamps at
the start and end of the recovery interval.

Runtime behavior is more generally unstable than a single delayed transition.
Changing a control in the nominally more expensive direction can temporarily
raise performance: one observation moved from roughly 48 to 56 FPS after
increasing light candidates, then drifted down again over approximately one
minute. This invalidates short post-CVar FPS comparisons. Each A/B must now be
held long enough to expose drift, and it needs GPU-pass timings or a continuous
frame-time trace rather than one settled-looking FPS value.

The behavior also reproduces without a CVar transition. In the minimal room at
one bounce and DI view 4, a fresh load can remain near 48 FPS for roughly 30
seconds, then rise to about 58 FPS, occasionally return to 48 FPS, and sometimes
remain at the lower plateau. The corresponding frame times are 20.83 and 17.24
ms, a discrete difference of approximately 3.59 ms. Reducing resolution far
enough reaches the 120-FPS cap, so the normal-resolution plateau is GPU-bound,
not a render-thread ceiling. This pattern is not compatible with six-frame FPS
smoothing or legitimate 12-frame GI history. Capture or timestamp the same
labeled GPU passes at both plateaus; whichever interval changes by about 3.6 ms
owns the state transition.

### GI history invalidation across scene resets

DI view 4 can acquire a blue tint after a same-resolution map reload and, less
consistently, after changing resolution. Source tracing found a concrete stale
history path: `ResetRayTracingSmokeSceneResources` resets the scene and DI-owned
state, but clean GI reservoirs were only marked for clearing when the GI-owned
reservoir buffer itself changed dimensions. A map reload at the same dimensions
could therefore consume reservoirs belonging to the previous scene.

Clean GI now invalidates reservoir history and cached bindings on the existing
frame reset reasons for scene resources, output resize, and backbuffer resize.
The reset is applied before GI dispatch, so the first GI execution after any of
those events clears both reservoir ping-pong regions before reuse. This is a
correctness fix for the blue-tint/reset contamination. It may remove a source of
bad post-reload timing, but it does not by itself explain a stable 48/58-FPS
plateau that can recur without a reset.

### Per-frame Vulkan descriptor-pool churn

A low-level lifetime mismatch exists between rbdoom and the NVIDIA sample.
NVRHI Vulkan's `Device::createBindingSet` creates a dedicated
`VkDescriptorPool` with `maxSets=1`, allocates one descriptor set, and makes the
binding set own that pool. Command-buffer liveness retains the binding set until
GPU retirement; its destructor then destroys the pool. The clean production
path was creating the large DI set and as many as five GI sets inside every
frame dispatch. At 50 FPS this can create and destroy approximately 18,000
Vulkan descriptor pools per minute. The NVIDIA sample instead retains binding
sets and recreates them only when render targets or RTXDI resources change.

The first bounded repair caches identical GI binding descriptions, separated by
layout, with a 32-entry cap. The cache is cleared when GI-owned buffers or
textures are recreated and in `ReleaseResources`. CVar
`r_pathTracingCleanRestirGiBindingSetCache` defaults to 1; value 0 restores the
original per-frame pool churn. This is host-only and does not change shader
work, descriptors, dispatch count, or reservoir history. DI's per-frame binding
set remains unchanged for this first A/B.

### Analytic trial overrun regression

`r_pathTracingRestirPTAnalyticLightTrials` defaults to 32 and supplies the
production RLU Doom-analytic sample count. The nominal
`r_pathTracingCleanRtxdiDiCandidateCount` does not control the active typed-RLU
initial producer. Runtime testing identifies analytic trials as one of only two
GI/DI controls with a measurable steady-state effect.

The sampler contained a concrete overrun. Commit `e877e931d` removed both the
manager-side and shader-side `min(sampleCount, rangeCount)` while adding
repeated emissive-triangle replay. Repeated UV proposals can be meaningful for
an emissive triangle, but the removal also affected Doom analytic records. The
shader then computes `stride = max(1, rangeCount / sampleCount)` and clamps the
resulting index to the end of the range. With six analytic lights and 32
trials, it evaluates lights 0--4 once and the final light 27 times.

The narrow repair caps only Doom analytic attempts to their range size.
Emissive triangles retain repeated UV replay. This restores the original
analytic bound without reverting the emissive feature and gives the minimal
room a controlled 32-requested/6-executed A/B.

The production DI initial Vulkan library compiled directly after deleting its
old blob and passed `spirv-val --target-env vulkan1.2`:

- size: 971,708 bytes;
- SHA-256: `A5753C375EFC25AC6DC19971A0B619336869CCF103BE49C624823C8C73A01519`;
- output mtime: 2026-07-31 13:27:03 UTC.

Runtime acceptance in the six-light room is that requested trials 32 and 6
have the same cost after restart, while 6 versus 1 retains only the legitimate
distinct-light proposal difference.

## Main conclusion

The slowdown is visible in the compiled production shaders. It is not merely an
Nsight-labeling anomaly or a BVH-only problem.

The production DI temporal `RayGen` contains 2.83 times as many SPIR-V operations
as the demo's custom DI temporal compute entrypoint before the first fix. The
production DI spatial `RayGen` contains 3.66 times as many operations as the
demo's custom DI spatial compute entrypoint. These ratios closely reproduce the
reported GPU-time ratios.

The source comparison previously looked deceptively similar because the shared
resampling math is similar. The surrounding callbacks, material reconstruction,
pass fusion, visibility resolve, history movement, and production output work are
not similar.

## Compiled artifact comparison

Counts below cover the hot entrypoint body from `OpFunction` through
`OpFunctionEnd`, not every entrypoint in the shader library.

| Entry | Blob bytes | Hot ops | Conditional branches | Loads | Extended ops | TraceRay |
|---|---:|---:|---:|---:|---:|---:|
| Demo DI temporal compute | 97,500 | 3,635 | 101 | — | — | 0 |
| rbdoom DI temporal, before | 217,612 | 10,278 | 877 | 293 | 391 | 1 |
| rbdoom DI temporal, packed-material fix | 209,156 | 9,718 | 827 | 267 | 387 | 1 |
| Demo DI spatial compute | 114,700 | 4,503 | 98 | — | — | 0 |
| rbdoom DI spatial, before | 326,156 | 16,474 | 1,332 | 554 | 693 | 1 |
| rbdoom DI spatial, packed-material fix | 301,380 | 14,936 | 1,200 | 473 | 651 | 1 |
| Demo GI temporal compute | 67,600 | 2,292 | 104 | — | — | 0 |
| rbdoom GI temporal compute | 130,780 | 7,119 | 583 | 49 | 397 | 0 |
| Demo GI spatial compute | 48,436 | 1,325 | 66 | — | — | 0 |
| rbdoom GI reuse `RayGen` | 473,752 | 16,031 | 1,307 | 121 | 952 | 3 |

The GI spatial rows are deliberately not presented as an apples-to-apples
algorithm comparison. rbdoom's reuse entrypoint also performs final shading and
resolve work.

Disassemblies used for this table are under:

`C:\Users\lizard\.codex\visualizations\2026\07\31\019fb6f4-eb62-74a3-9b28-a810412ebc0d\ptperf`

## Proven low-level causes

### 1. Production DI re-resolved already-resolved material state

The primary-surface producer samples textures, applies the material classifier,
applies full-metal and roughness overrides, and packs the resulting BSDF into a
176-byte `PathTracePrimarySurfaceRecord`.

Production temporal and spatial reuse then:

1. resolved a live material index from geometry again;
2. loaded live material/classifier state again;
3. re-applied the classifier to packed albedo, F0, and roughness;
4. repeated that work for center, previous, selected-source, and neighbor
   target-PDF evaluations.

This is multiplicative work inside resampling loops. It is also the wrong history
contract: a previous-frame surface should retain its previous-frame resolved
attributes rather than being restyled with current live material state.

The production-only `CLEAN_DI_VIEW_STATIC` path now consumes the packed resolved
material directly. Diagnostic libraries retain live classifier re-evaluation.

Measured compiled effect:

- temporal hot ops: 10,278 -> 9,718 (-5.4%);
- temporal loads: 293 -> 267 (-8.9%);
- spatial hot ops: 16,474 -> 14,936 (-9.3%);
- spatial loads: 554 -> 473 (-14.6%);
- temporal blob: 217,612 -> 209,156 bytes (-3.9%);
- spatial blob: 326,156 -> 301,380 bytes (-7.6%).

This is a proven contributor, not a complete explanation. The post-fix production
entries remain approximately 2.67 times and 3.32 times the demo entries.

### 2. DI temporal-to-spatial history was copied instead of consumed directly

The temporal pass writes u70. Before spatial, C++ transitioned u70 to copy source,
transitioned u71 to copy destination, copied the complete full-resolution
reservoir, and made spatial read u71.

Spatial now reads u70 directly. After spatial has recorded its work, C++ rotates
the temporal and previous buffer handles so the just-produced temporal page is
u71 on the next frame.

Avoided transfer size for the 24-byte packed DI reservoir:

| Resolution | Avoided bytes/frame | Avoided MiB/frame |
|---|---:|---:|
| 2560 x 1440 | 88,473,600 | 84.4 |
| 3840 x 2160 | 199,065,600 | 189.8 |

This also removes the associated CopySource/CopyDest transitions and barriers.

### 3. rbdoom DI spatial is not the demo's spatial pass shape

The demo custom spatial shader:

1. loads a compact G-buffer surface;
2. loads an input reservoir;
3. runs spatial resampling;
4. stores the output reservoir.

rbdoom `CleanDI.2 Spatial DispatchRays` additionally performs final DI resolve,
BRDF weighting, output/RR writes, and a selected-light visibility ray. Comparing
that whole marker against the demo's custom spatial marker attributes resolve
work to “spatial”.

This explains part of the marker ratio but does not prove that the extra work is
free or optimal. A useful next experiment must separate reuse and resolve into
independently timed production entrypoints while preserving total output.

### 4. rbdoom GI temporal is fused with initial-reservoir preparation

The demo custom GI temporal wrapper loads an already-produced initial reservoir
and performs temporal reuse.

rbdoom's `CleanGI.1 TemporalReuse` contract constructs/accepts the initial
reservoir, stores the INIT page, performs temporal reuse, validates it, and stores
the temporal page. The marker therefore includes producer/initial work that is
outside the demo temporal marker.

The next GI experiment should move the real initial-reservoir preparation into
the seed/initial pass and make temporal load INIT. Merely adding phase branches
inside the same giant entrypoint is not sufficient; a prior phase-entry split
regressed GPU time.

## Other high-value findings

### Primary-surface history copies 176 bytes per pixel every frame

After all clean DI/GI consumers, the clean route copies the complete current
primary-surface page to the previous page.

| Resolution | Copy bytes/frame | Copy MiB/frame |
|---|---:|---:|
| 2560 x 1440 | 648,806,400 | 618.8 |
| 3840 x 2160 | 1,459,814,400 | 1,392.2 |

The demo rotates current/previous G-buffer handles. rbdoom cannot safely apply a
late blind swap because the same handles are captured by the primary producer's
binding set, subview promotion, and post-dispatch diagnostics. The correct fix is
producer-boundary page rotation plus binding-set ownership changes. Treat this as
a dedicated experiment, not a two-line end-of-frame swap.

### Per-frame clean DI binding-set construction is CPU-side overhead

The clean route builds a binding set with roughly seventy resources every frame.
The demo keeps stable binding sets and rotates resources. This can contribute to
render-thread/driver overhead but cannot explain a single GPU dispatch taking
three times as long. Measure it separately from shader event time.

### Existing GI findings remain valid but secondary to the base mismatch

Prior work already removed a duplicate full-screen specular GI producer and
reduced inherited DI candidate counts in GI shading. Those were real costs. They
do not account for the compiled DI temporal/spatial ratios above.

## Build and deployment result

Validated on 2026-07-31:

- production Vulkan temporal and spatial shaders compiled directly with the same
  DXC arguments generated by CMake;
- both blobs are newer than their edited source/includes;
- spatial disassembly binds the input reservoir only at u70; u71 is absent;
- `RBDoom3BFG.vcxproj`, Release x64, compiled and linked successfully with
  project references disabled;
- executable and only the affected Vulkan blobs were copied to
  `E:\prog\rbdoom-3-BFG-prebuilt`.

The complete preset currently fails while compiling the all-views diagnostic DI
shader with DXC `failed to legalize SPIR-V: ID overflow`. The production view-16
libraries compile successfully. The diagnostic path retains its original live
material behavior, so the ID-overflow failure must be resolved independently
before claiming a fully green preset build.

## Required runtime A/B

Use the same test-map save, camera, internal/output resolution, and warm-up for
both builds. Capture at least three steady frames per mode.

1. Base DI:
   - clean DI production view 16;
   - clean GI disabled;
   - DLSS Ray Reconstruction disabled.
2. DI + GI:
   - same DI state;
   - clean GI enabled in production view;
   - DLSS Ray Reconstruction disabled.
3. DI + GI + DLSSRR:
   - same DI/GI state;
   - DLSS Ray Reconstruction enabled.

Record:

- full GPU frame time;
- `CleanDI.1 Temporal DispatchRays`;
- `CleanDI.2 Spatial DispatchRays`;
- the copy/barrier region between those events;
- `CleanGI.1 TemporalReuse`;
- `CleanGI.2 SpatialReuse DispatchRays`;
- primary-surface current-to-previous copy;
- CPU submit/render-thread time separately.

Acceptance for the current slice:

- no temporal instability or material-state regression;
- no DI temporal-to-spatial reservoir copy in GPU Trace;
- temporal/spatial GPU time does not regress;
- any gain is reported as median and range, not a single-frame FPS value.

## Ranked next experiments

1. Capture the deployed packed-material/direct-u70 slice.
2. Split DI spatial reuse from final visibility/resolve so algorithm and output
   costs are independently timed and comparable to the demo.
3. Split real GI initial-reservoir preparation from temporal reuse.
4. Rework primary-surface page ownership so history rotates at the producer
   boundary and removes the 176-byte-per-pixel copy.
5. Cache/rebuild clean DI binding sets only when resource identities change;
   measure CPU submission separately.
6. Return to optional glass, extra bounce, reflection, and denoiser features only
   after the base pass-shape and history costs are accounted for.

## Producer follow-up after runtime A/B

The packed-material/direct-u70 slice improved the DI temporal and spatial events
by about 10 percent in the test map. This confirms that the reuse cleanup was
real, but also confirms that reuse was not the main frame-time limiter. Nsight
shows `DispatchRays` dominated by:

- `CleanDI.0 Initial`;
- `FirstIndirect.0a Trace`;
- `FirstIndirect.0b ShadeFast` (or the full shade fallback).

The follow-up therefore compares those producer passes with the matching Vulkan
ray-generation permutations in `E:\prog\rtxdi_testing`.

### Compiled producer comparison

The NVIDIA rows were compiled from the modified local sample with DXC, SPIR-V,
ray-query disabled, and ReGIR disabled. Counts cover the hot entrypoint or
hit-shader function body.

| Function | SPIR-V operations | Loads | Branches | Image operations | Static TraceRay sites |
|---|---:|---:|---:|---:|---:|
| NVIDIA DI initial raygen | 7,798 | 78 | 542 | 23 | 2 |
| rbdoom clean DI initial raygen | 54,048 | 1,561 | 10,033 | 271 | 9 |
| NVIDIA BRDF/first-indirect trace raygen | 1,992 | 97 | 241 | 33 | 1 |
| rbdoom first-indirect trace raygen | 12,650 | 428 | 2,170 | 214 | 1 |
| NVIDIA material any-hit | 348 | 35 | 51 | 3 | 0 |
| rbdoom first-indirect material any-hit, before | 6,395 | 384 | 1,038 | 168 | 0 |
| NVIDIA secondary-surface shade raygen | 17,948 | 182 | 1,297 | 63 | 4 |
| rbdoom `ShadeFast` raygen | 26,237 | 613 | 4,805 | 167 | 5 |
| NVIDIA shadow any-hit | 260 | 20 | 36 | 2 | 0 |
| rbdoom `ShadeFast` shadow any-hit, before | 3,686 | 214 | 615 | 96 | 0 |

Static TraceRay sites are not rays per pixel; they are separately inlined
control-flow routes in the compiled function. The first-indirect trace has one
actual material ray for each valid launched receiver. `ShadeFast` selects one
direct proposal by default and traces its shadow ray, but its compiled pipeline
contains five possible inlined visibility routes.

### DI final-visibility reuse hypothesis: disproven at frame level

NVIDIA's default DI parameters enable initial visibility and enable final
visibility reuse. rbdoom also enables initial visibility, but previously
defaulted `r_pathTracingCleanRtxdiDiResolveVisibilityReuse` to 0. A surviving
sample could therefore pay one shadow ray in initial sampling and another in
final resolve.

The experiment changed the default to 1: reuse packed reservoir visibility when
valid, otherwise fall back to the final visibility trace. Runtime A/B showed no
measurable frame-rate change. The default has therefore been restored to 0.
This policy mismatch is real, but it is not the missing producer performance.

There is a separate diagnostic-control bug in the initial shader: setting
`r_pathTracingCleanRtxdiDiInitialVisibility 0` disables storing visibility but
does not skip the trace. Fixing that gate in the current all-entry sentinel tips
DXC over its existing SPIR-V ID limit. It must be fixed together with a split of
the monolithic diagnostic library; the production default remains initial
visibility on.

### GI opaque-candidate any-hit hypothesis: disproven at frame level

The rbdoom GI material and shadow rays use `RAY_FLAG_FORCE_NON_OPAQUE`. Every
candidate, including ordinary opaque walls, therefore executes any-hit.

`CleanGiMaterialRejectsHit` does the following before
checking whether the material had any alpha-driven behavior:

1. resolved the triangle material route a second time;
2. loaded and applied dynamic emissive material state;
3. rebuilt all three vertices with normals, two UV sets, and two color sets;
4. sampled diffuse/alpha coverage.

For a normal opaque material, all of that work is discarded and the hit is
accepted. An experiment changed the path to:

- passes the already-resolved material index into rejection;
- loads static visibility fields without applying unrelated dynamic emissive
  state;
- classifies alpha-driven and GUI materials before geometry reconstruction;
- immediately accepts ordinary opaque candidates.

Alpha test, keyed alpha, additive/filter decals, GUI vertex alpha, glass
transmission, and liquid-pool collection retained their existing paths.

Compiled any-hit size moves only modestly because those uncommon paths still
exist:

| Function | Operations before | Operations after | Loads before | Loads after | Branches before | Branches after |
|---|---:|---:|---:|---:|---:|---:|
| material any-hit | 6,395 | 6,152 | 384 | 373 | 1,038 | 984 |
| shadow any-hit | 3,686 | 3,443 | 214 | 203 | 615 | 561 |

Despite the static reduction, the deployed A/B showed no measurable frame-rate
change. The fast path has been rolled back. This eliminates ordinary-candidate
any-hit material work as the explanation for the current producer gap in the
reference scene.

### GI trace-to-shade record is three times the reference size

rbdoom writes and rereads a 144-byte
`PathTraceFirstIndirectCandidateSurface` at full internal resolution between
trace and shade. NVIDIA's `SecondaryGBufferData` is 48 bytes and packs normals,
albedo, F0/roughness, throughput, and flags.

At 3840x2160, rbdoom's record represents roughly 1.11 GiB for one full write
plus one full read, versus about 0.37 GiB for the reference-shaped record. This
is the next architectural producer experiment, after measuring the any-hit and
visibility changes. It requires a packed trace/shade ABI and should not be mixed
into the current checkpoint.

### Leading historically valid candidate: the primary-surface ABI

The 144-byte GI trace-to-shade record is a current cost, but it was introduced
after the original basic DI/GI implementation. It cannot by itself explain the
long-standing two-to-three-times gap.

The primary-surface contract can. The first primary-history implementation,
commit `b4d550a48` on May 8, used a 128-byte
`PathTracePrimarySurfaceHistory`: eight complete 16-byte vectors. Commit
`b8074836f`, the next day's path-trace core refactor baseline, expanded that
design to the current 176-byte `PathTracePrimarySurfaceRecord`. The current
record contains eleven 16-byte vectors: validity/status, world position/depth,
two normals, view direction, resolved albedo, F0, emissive, previous
position/motion, material/surface identity, and instance/primitive identity.
DI initial, DI temporal, DI spatial, GI trace, and GI shade all consume this
structured buffer. The exact 176-byte layout is not the historical invariant;
the invariant is a large, unpacked, composite structured surface record from
the start of temporal history.

The NVIDIA sample instead reads five packed G-buffer values for the equivalent
primary surface:

- one 32-bit depth value;
- two 32-bit octahedral normals;
- one 32-bit packed diffuse value;
- one 32-bit packed specular/roughness value.

It reconstructs world position and view direction from depth and view
constants. The nominal primary input is therefore 20 bytes per pixel rather
than a 176-byte structured record, before considering that individual textures
can be loaded selectively.

One complete 176-byte sweep is about 348 MiB at 1920x1080 and 1.36 GiB at
3840x2160. The compiled Vulkan shaders contain composite structured-buffer
loads of this record in every hot family examined: two static sites in DI
temporal, four in DI spatial, one in first-indirect trace, and one primary plus
one 144-byte secondary load in first-indirect ShadeFast. Static load sites are
not a dynamic byte count, and the NVIDIA driver may scalarize them, so this is
a leading candidate rather than proof of physical memory traffic.

Modes 1, 6, and 7 initially tested that question in
`FirstIndirect.0a Trace DispatchRays`: no primary read, one scalar primary
read, and forced consumption of all 176 bytes. All three modes produced the
same roughly 50-percent FPS improvement over normal mode 0.

That runtime result disproves primary-surface transport as the large cost
inside the first-indirect trace dispatch. It does not prove that the primary
ABI is free across DI temporal/spatial neighbor loops, but it removes the
record load from the leading explanation for this GI dispatch.

The second probe reused modes 6 and 7 for normal primary/RNG/ray setup and then
force-opaque traversal with any-hit and closest-hit suppressed. Modes 1, 6,
and 7 again produced the same result: 26 FPS in normal mode 0 versus 35 FPS in
all three proof modes. That is approximately 38.5 ms versus 28.6 ms per frame,
placing roughly 9.9 ms exclusively after the bare traversal boundary in this
scene. Primary reconstruction, RNG/ray sampling, TLAS traversal, the large
payload, and miss handling are all below FPS resolution in that test.

The third probe measured 35 FPS in mode 1, 35 FPS in force-opaque
closest-hit-only mode 6, 33 FPS in non-opaque any-hit mode 7, and 26 FPS in
normal mode 0. In frame time, any-hit accounts for about 1.7 ms
(28.6 -> 30.3 ms), while the post-trace reconstruction block accounts for
about 8.2 ms (30.3 -> 38.5 ms). The all-geometry-any-hit policy is real but is
not the dominant current cost.

The next modes split raw geometry transport from the rest of reconstruction:

- mode 6 performs the normal non-opaque trace, any-hit, closest-hit, and full
  payload fingerprint, then stops;
- mode 7 performs mode 6 plus `CleanGiLoadTriangleGeometryFull` and consumes
  every returned position, normal, UV, normal UV, vertex color, and secondary
  color value, but performs no normal/material texture sampling, classifier
  work, liquid resolution, or 144-byte candidate packing;
- mode 0 retains the complete dry-surface reconstruction and packing.

`PathTraceSmokeVertex` was already five `float4` values, 80 bytes, before the
first NVIDIA-reference comparison. It is now seven `float4` values, 112 bytes.
The hit loader fetches three composite vertex records, nominally 240 bytes in
the early design and 336 bytes now, even though later stages consume only
subsets. The same vertex ABI and route-dependent loaders feed DI and GI.

The production geometry-proof SPIR-V is 444,452 bytes and passes
`spirv-val` for Vulkan 1.2. It contains 72 static composite
`OpLoad %PathTraceSmokeVertex` sites because the three-vertex load is duplicated
across static, dynamic, bucket, and rigid route branches in both the normal and
proof call graphs. These are static sites rather than a per-lane execution
count, but they demonstrate the route-megakernel shape surrounding a logically
simple three-vertex fetch.

Compare `FirstIndirect.0a Trace DispatchRays`: `7 - 6` isolates route/index
resolution and three full vertex loads; `0 - 7` isolates interpolation/tangent
math, normal/diffuse/specular/alpha/emissive texture work, classifier and
override work, liquid resolution, and candidate packing.

Runtime measured both modes 6 and 7 at about 33--34 FPS. Full secondary
vertex-record transport is therefore also below FPS measurement resolution in
this dispatch. The remaining roughly 8.2 ms mode-0 gap starts after the vertex
loads: interpolation and tangent construction, normal/diffuse/specular/alpha/
emissive texture sampling, material/classifier overrides, liquid resolution,
and candidate packing.

### Debug-view workload cliffs

The DI and GI debug-view numbers are not a monotonic shader-feature bisect.
Several view transitions silently change the number of candidates or activate
additional full-screen dispatches.

Observed DI behavior was effectively frame-rate-limited through view 7, then
dropped by about 40 FPS at view 8. The host/shader routing explains a real
workload boundary:

- views 1--3 are sentinel/status presentations and do not run the initial DI
  producer;
- view 4 runs initial DI, but the host supplies one local-light candidate;
- views 5 and 6 run initial plus temporal, also with one candidate;
- view 7 runs one-candidate initial DI and presents reservoir identity/history;
- view 8 changes `CleanRtxdiDiCandidateCount` from one to the CVar default of
  eight and, when temporal is enabled, runs both initial and temporal;
- the default stacked view-8 diagnostic also replays selected-sample
  visibility in band 8, covering one sixteenth of the screen.

Changing `r_pathTracingCleanRtxdiDiCandidateCount` from 1 through 20 produced
no FPS change. Source tracing showed that this CVar does not control the active
producer when the Remix light-universe route is enabled:
`PathTraceCleanRoomRunInitialProducer` diverts to
`PathTraceCleanRoomRunTypedRluInitialProducer`, which streams the RLU's
independent typed sample counts instead. Those counts are populated from
`r_pathTracingReservoirCandidateTrials` for emissive triangles (default 1) and
`r_pathTracingRestirPTAnalyticLightTrials` for Doom analytic lights (default
32). A valid candidate-count test on this route must change those Cvars, not
`r_pathTracingCleanRtxdiDiCandidateCount`.

The other clean separation is `r_pathTracingCleanRtxdiDiView8Band 0`, which
retains the initial and temporal dispatches but replaces the stacked 16-band
presentation with the cheapest temporal-gate output. If that restores the
missing FPS, the cliff is in view 8's diagnostic replay/presentation rather
than reservoir production or temporal reuse.

The valid RLU proposal-count test reduced
`r_pathTracingRestirPTAnalyticLightTrials` from its default 32 to 1. DI view 8
recovered about 10 FPS, but the complete level with DI and GI active recovered
barely 1 FPS. The 32-trial default is unnecessarily expensive in the isolated
DI view, but it is only a secondary cost once the full frame is dominated by
GI and cannot explain the renderer-wide 2--3x gap.

Disabling manual bilinear texture filtering while retaining the safe
`Texture.Load` method also produced no measurable FPS change. The four-load
manual filter is therefore not the dominant fixed cost.

The strongest scene-complexity control is an extremely simple closed box with
about six lights and one stationary low-poly Doom 3 monster. It contains no
reflective surfaces. DI view 4 remains at 120 FPS, while enabling GI view 1
immediately drops to about 33 FPS. That is the same roughly 8.3-to-30.3 ms
transition seen in materially busier content.

This invariance rules out geometry count, BVH traversal complexity, reflective
materials, multibounce behavior, and later optional features as explanations
for the dominant GI cost. The remaining culprit is fixed per dispatched pixel:
dispatch extent, compiled shader/occupancy shape, trace-to-shade dependency and
record transport, or unconditional per-pixel reconstruction.

An attempted modes 2/5/3 comparison under GI debug views 1 and 6 initially
showed no response. That was a deployment error rather than a shader result:
the host selects `debugRayTracing` for every nonzero GI view, while the proof
work had only been rebuilt and deployed in the `split/production` libraries
used by view 0. The three `split/debug` blobs still predated the proof source by
one day.

The debug first-indirect trace, ShadeFast, and full-shade libraries were rebuilt
directly with the generated Vulkan DXC definitions, passed `spirv-val` for
Vulkan 1.2, and were deployed to the prebuilt tree. Their deployed sizes and
SHA-256 values are:

- trace: 444,452 bytes,
  `F65DAF4268FBFD88C34556DE18A20C34A33A8CFCCC2AE8DD88B146D47EC1FC08`;
- ShadeFast: 660,316 bytes,
  `479CAB90CF5B207E9958CADEEABFE52B4F0F91F6C626FC7F77C507540EFAB0FB`;
- full shade: 1,612,632 bytes,
  `6ADD56E600FC3EFBDE5818226A30713A6632972BD796C29327A687C22221ADEB`.

The modes 2/5/3 comparison must be repeated after a renderer restart. Mode 2
is immediate output, mode 5 adds one scalar trace-to-shade buffer read, and
mode 3 materializes the complete producer record and executes the proof store.

### Simple-room Nsight capture

An Nsight GPU Trace of DI view 4 plus GI view 1 in the six-light box measured a
35.27 ms frame. The dominant labeled dispatches were:

- `FirstIndirect.0a Trace DispatchRays`: 6.02 ms;
- `FirstIndirect.0b Shade DispatchRays`: 11.76 ms;
- `FirstIndirect.0c ContinuationTrace Dispatch`: 4.77 ms.

The trace and shade producer alone therefore consume 17.78 ms in the nearly
empty scene. This directly confirms that the dominant cost is inside the
full-screen GI producer passes rather than scene geometry or optional
reflection materials.

The capture is not a one-bounce baseline: the continuation marker proves
`r_pathTracingCleanRestirGiMaxBounces` was 2 (also the current default). That
optional trace visibly costs 4.77 ms, but removing it still leaves the two
basic producer passes at 17.78 ms. The clean modes 2/5/3 capture should use
`r_pathTracingCleanRestirGiMaxBounces 1` so continuation work does not obscure
the trace-to-shade comparison.

Observed GI behavior, while DI view 4 remained near 120 FPS, was about 33 FPS
for GI views 1 and 2 and about 28 FPS for view 3. These are approximately
8.3 ms, 30.3 ms, and 35.7 ms per frame respectively: the GI producer adds
about 22.0 ms, then the view-3 consumer chain adds about 5.4 ms.

GI views 1 and 2 are untextured only in presentation. The host still dispatches
the complete first-indirect trace and shade producer over the full image, and
the shade raygen performs the normal producer work before writing the simple
radiance or hit-geometry debug color. Seed and reuse dispatches are also
submitted, but their shaders return immediately for these views.

View 3 stops satisfying `CleanGiSeedPassSkipsView`. It therefore adds the INIT
seed pass and temporal reuse; when spatial reuse is enabled it also adds the
separate spatial dispatch. The 33-to-28 FPS transition is consequently a
producer-to-producer-plus-consumers boundary, not the cost of displaying an
initial-reservoir color.

The installed Nsight 2026.1 replay CLI accepts frame captures but rejects
`.ngfx-gputrace` files as an invalid replay header. Existing GPU Trace
bandwidth/cache/scoreboard counters therefore need to be read in the Nsight UI
or exported from it; they cannot be recovered through `ngfx-replay`.

### Disproven producer-slice verification

- All three experimental production GI Vulkan libraries compiled and passed
  `spirv-val`.
- `first_indirect_trace`: 427,148 -> 419,572 bytes.
- `first_indirect_shade_fast`: 658,568 -> 650,992 bytes.
- `first_indirect_shade`: 1,291,724 -> 1,284,148 bytes.
- The targeted Release executable build succeeds.
- The complete preset still stops at the pre-existing all-entry DI sentinel
  `ID overflow`; production Vulkan permutations compile successfully.

Runtime result: the combined change produced no measurable improvement. Both
behavior changes were rolled back.

### Active-shader proof and next isolation boundary

`r_pathTracingCleanRestirGiProducerConsumeProof 2` returns immediately from the
production shade raygen after writing magenta. It improved FPS by roughly 50
percent in the reference scene. This proves that the rebuilt production shader
is consumed. It does not identify an internal culprit: mode 2 bypasses the
144-byte surface read/unpack, RNG, proposal selection, BSDF/target math, shadow
`TraceRay`, hit shaders, and the normal result store.

The next modes isolate that skipped work inside the same dispatch:

- mode 0: normal production workload;
- mode 2: dispatch launch plus immediate magenta UAV write;
- mode 3: producer-surface load, unpack, emissive candidate construction, and
  normal radiance store; no RNG, proposal selection, BSDF, or shadow ray;
- mode 4: normal proposal selection, BSDF/target/contribution math, and result
  store, but return visible after the normal geometric visibility gates and
  before the shadow `TraceRay`.
- mode 5: read only the packed surface's scalar `valid` field, then return the
  same magenta marker as mode 2 with a visually negligible loaded-data delta.
- mode 6: trace force-non-opaque with normal any-hit and closest-hit, fingerprint
  the complete payload, and stop before reconstruction;
- mode 7: perform mode 6 plus all three full vertex-record loads, fingerprint
  every returned geometry attribute, and skip material evaluation and packing.

Capture the `FirstIndirect.0b ShadeFast DispatchRays` GPU duration for each
mode, rather than comparing FPS alone. Mode 3 minus mode 2 bounds surface
transport and unpack/store cost. Mode 0 minus mode 4 isolates the selected
shadow traversal and hit-shader cost. Mode 4 minus mode 3 bounds proposal,
random-sampler, BSDF/target, and contribution work.

Initial FPS A/B showed modes 3 and 4 both about 4 FPS faster than mode 0,
whereas mode 2 was previously about 50 percent faster. The tie between 3 and 4
puts proposal/RNG/BSDF work below FPS measurement resolution and attributes
only the roughly 4 FPS delta to the selected shadow ray. The large mode-2 to
mode-3 gap moves the primary suspicion to the trace-to-shade record crossing.
Mode 5 distinguishes the cost of one scalar buffer access from materializing
the complete record; use marker GPU duration because FPS differences are
non-linear.

The renderer was already comparably slow when only basic DI and GI existed.
Treat that history as a hard scope constraint: later glass, multibounce, RLU,
DLSS-RR, special-reflection, and material features may add incremental cost but
cannot be the root cause. The primary comparison must remain at the original
architecture layer: dispatch extent and active-lane count, primary/secondary
ray count, trace-to-shade record size and access shape, ray payload and
attribute size, SBT/hit-group routing, forced any-hit policy, and the compiled
production entry-point shape versus the NVIDIA sample.

### Foundational any-hit candidate: every BLAS geometry is non-opaque

The BLAS creation contract matches the historical constraint and is now a
leading shared DI/GI candidate.

NVRHI initializes `nvrhi::rt::GeometryDesc::flags` to
`GeometryFlags::None`; opacity requires explicitly setting
`GeometryFlags::Opaque`. rbdoom's original `CreateSmokeBlas` path and the
current legacy, canonical-rigid, static-bucket, and skinned BLAS builders call
`geometry.setTriangles(...)` but do not set the opaque geometry flag. This
policy is present in the extracted acceleration helper from commit
`ab82fcf8a` on May 2, before the NVIDIA sample comparison work.

Consequently, the early bounce rays' `RAY_FLAG_NONE` did not mean that ordinary
opaque triangles took an opaque fast path. It honored the BLAS geometry's
non-opaque classification and allowed any-hit execution. Later clean DI/GI
shaders made the same policy explicit with `RAY_FLAG_FORCE_NON_OPAQUE`.

This cannot be fixed by marking every existing mixed geometry range opaque:
alpha-tested cards and other rejecting materials still require any-hit. A
shipping repair would need opacity-aware geometry partitioning or an equivalent
per-instance/per-range contract. The current mode-6 versus mode-7 proof is safe
as a diagnostic because mode 6 force-bypasses any-hit only for the isolated
invalid-candidate trace.

Modes 6 and 7 measured equally at about 33--34 FPS. Together with the earlier
35-FPS closest-hit-only result, this leaves roughly 1.7 ms attributable to
any-hit and moves the remaining mode-0 gap into post-trace secondary material
reconstruction rather than BLAS opacity/any-hit routing or raw vertex
transport.

The deployed Vulkan probe build was verified as follows:

- production trace: 427,448 bytes;
- production ShadeFast: 659,936 bytes;
- production full shade: 1,293,616 bytes;
- all three output mtimes postdate the HLSL edit and pass `spirv-val` for
  Vulkan 1.2;
- ShadeFast disassembly contains the mode-3/mode-4 branches, a composite load
  of the producer surface record, and the retained shadow `OpTraceRayKHR`;
- the targeted Release executable build succeeds;
- the complete preset still stops later at the pre-existing monolithic clean
  DI sentinel `ID overflow`.

### One-bounce simple-room shade decomposition

The proof modes were repeated after the debug shader deployment was corrected,
in the minimal six-light room with DI view 4, GI view 1, and one GI bounce:

- two-bounce baseline: 33 FPS, or about 30.30 ms;
- one-bounce mode 0: 38 FPS, or about 26.32 ms;
- one-bounce mode 2: 70 FPS, or about 14.29 ms;
- one-bounce mode 3: 67 FPS, or about 14.93 ms;
- one-bounce mode 4: 50 FPS, or about 20.00 ms.

These frame-level differences are large enough to overturn the earlier
full-level FPS inference:

- mode 3 minus mode 2 is only about 0.64 ms. Materializing and consuming the
  complete 144-byte trace-to-shade record is therefore not the dominant shade
  cost in this scene.
- mode 4 minus mode 3 is about 5.07 ms. Proposal selection, sampler/BSDF/target
  work, contribution construction, and the pre-trace visibility gates are a
  major cost even with approximately six lights.
- mode 0 minus mode 4 is about 6.32 ms. The selected shadow `TraceRay`, its hit
  path, and any compiler/register-pressure consequence of retaining that trace
  account for the other major half of the shade gap.
- enabling the second bounce adds about 3.99 ms by frame time. The earlier
  Nsight capture measured its continuation trace dispatch directly at 4.77 ms.

Mode 2 is now a proven active-shader lower bound, not merely a marker test.
The roughly 12.0 ms difference from mode 2 to normal one-bounce shading agrees
closely with the 11.76 ms Nsight shade dispatch.

There is one important debug-route qualification. GI view 1 deliberately
selects the full `FirstIndirectShadeRayGen` library. The host selects the
smaller `FirstIndirectShadeFastRayGen` only when view is 0, NEE-cache secondary
sampling is disabled, max bounces is at most 1, the direct sample count is 1,
and direct probability is 1. The view-1 proof cleanly decomposes the full shade
path, but its absolute 11.76 ms must not be attributed automatically to the
shipping one-bounce fast permutation. A view-0 Nsight capture should compare
the `ShadeFast` marker directly; frame FPS alone includes seed/reuse/final work.

### Shadow-payload contract experiment

The current bounce payload is 152 bytes: 40 bytes of the original hit/control
state plus a 112-byte four-entry liquid-pool candidate set. Both GI pipeline
descriptors still declared `maxPayloadSize = 64`. The 64-byte declaration was
valid for the original 40-byte payload and became stale when the liquid
candidate set was appended. This descriptor field is not a Vulkan performance
control: NVRHI's Vulkan backend derives the interface from SPIR-V and does not
read `maxPayloadSize`; its explicit use is in the D3D12 shader-config object.
Correcting it is an ABI repair, not a proposed explanation for Vulkan timing.

More importantly, every visibility shadow ray reused this complete bounce
payload even though shadow traversal communicates only one blocked/unblocked
`uint`. The shadow any-hit shader reads no payload state; the shadow closest-hit
and miss shaders only set `value`. This design also predates the liquid
extension: the original visibility ray still carried a 40-byte bounce payload
where four bytes sufficed. The later 112-byte extension can amplify a current
cost but cannot by itself explain the renderer's historical slowdown.

A narrow A/B build now:

- gives visibility rays a separate four-byte payload;
- preserves static and skinned alpha/liquid any-hit behavior;
- leaves the 152-byte bounce payload and all lighting math unchanged;
- corrects the pipeline's largest-payload declaration from 64 to 152 bytes.

The changed common skinned-hit, debug shade, debug ShadeFast, production shade,
and production ShadeFast Vulkan libraries all pass `spirv-val` for Vulkan 1.2.
The Release executable builds successfully. The full preset continues to stop
only at the known unrelated clean-DI sentinel `ID overflow`.

Runtime acceptance is deliberately simple: with one bounce and GI view 1,
mode 4 should remain near 50 FPS because it never traces the visibility ray.
If mode 0 rises materially from the prior 38 FPS toward mode 4, oversized
shadow payload liveness was part of the 6.32 ms boundary. If mode 0 remains
near 38 FPS, revert the experiment and continue inside traversal/hit routing
and the five-millisecond proposal/setup slice.
