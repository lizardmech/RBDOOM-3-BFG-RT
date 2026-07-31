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
