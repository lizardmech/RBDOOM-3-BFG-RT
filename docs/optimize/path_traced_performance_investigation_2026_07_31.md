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

Modes 6 and 7 are now reused for the next boundary while mode 1 remains the
immediate invalid-candidate baseline:

- mode 6 loads the normal primary surface, initializes RNG, constructs the
  material surface, and samples the normal first-indirect ray, then invalidates
  the candidate before `TraceRay`;
- mode 7 performs the same setup and launches the ray into the same TLAS with
  the normal large payload, but forces opaque traversal and suppresses both
  any-hit and closest-hit execution;
- mode 0 remains the complete normal first-indirect trace, hit handling,
  secondary-surface reconstruction, and candidate packing.

The candidate remains invalid in modes 1, 6, and 7, keeping downstream shade
work equivalent. Compare `FirstIndirect.0a Trace DispatchRays`: `6 - 1` bounds
primary reconstruction, RNG, and BSDF ray sampling; `7 - 6` isolates TLAS
traversal, miss handling, and the large payload without material hit shaders;
`0 - 7` attributes the remaining cost to forced-non-opaque any-hit,
closest-hit, dry-surface/material reconstruction, liquid-capable payload
handling, and packing the secondary candidate.

The rebuilt production Vulkan trace library is 429,044 bytes and passes
`spirv-val` for Vulkan 1.2. Its disassembly has a distinct mode-7
`OpTraceRayKHR` with constant ray flags `13`
(`FORCE_OPAQUE | ACCEPT_FIRST_HIT_AND_END_SEARCH |
SKIP_CLOSEST_HIT_SHADER`); mode 6 branches around that instruction. The normal
mode-0 `OpTraceRayKHR` and material-hit path remain present.

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
- mode 6: in the trace pass, perform normal primary reconstruction, RNG setup,
  and first-indirect ray sampling, then invalidate before `TraceRay`;
- mode 7: perform mode 6 plus force-opaque TLAS traversal using the normal
  payload, with any-hit and closest-hit execution suppressed.

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
