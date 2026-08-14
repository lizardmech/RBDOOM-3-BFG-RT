# PT Performance Findings — 2026-08-11

Reference for future sessions. Records measured facts, falsified hypotheses, the
underlying failure class, reference-implementation comparisons, and the
measurement protocol. Read before proposing any optimization.

---

## 1. Measurement protocol (do this first, everything else is worthless without it)

| Tool | Purpose |
|---|---|
| `r_pathTracingUnifiedPtTemporalGpuTiming 1` | T0 dispatch-only: 16 warm-up frames, 64 samples, prints median/mean/min/p90/max |
| `r_pathTracingUnifiedPtFixedSampleIndex 0` | Freezes RNG streams |
| `r_pathTracingUnifiedPtFrozenScene 1` → auto 2 | Immutable committed scene, no uploads/AS work |
| `r_pathTracingUnifiedPtTemporalBottleneckProbe 0..11` | T0 stage ladder (see §4) |

**Within-batch noise: ~2%** (±0.2 ms at 11–14 ms). Excellent.

**Warm-up bias: up to 33% across batches after ANY state change.** Monotonic,
settles in ~3 batches. Measured with zero config change:

```
14.592 → 13.881 → 10.983 → 10.982 (settled, reproduces to 1 µs)
```

**Rule: only record a value once two consecutive batches agree.** A first-batch
measurement after a cvar change is unusable and reads ~30% pessimistic. This is
almost certainly what the "walk around and it gets faster" drift was, and it may
have corrupted several results below (notably the reconnection regression).

---

## 2. Measured timings (2560×1440 unless noted)

| Config | T0 dispatch |
|---|---|
| Family 1 (direct-only), TemporalIndirect 0 | **2.462 ms** |
| Family 0 + TemporalIndirect 1 + SplitInitial 1, settled | **10.982 ms** |
| → indirect temporal delta | **+8.5 ms** |
| FrozenScene 0 (production) | 18–20 ms |
| FrozenScene 2 (no uploads / no AS work) | **~14 ms** (−4 to −6) |
| FrozenScene 3 (+static-only TLAS, flat-static, analytic-only) | **~6 ms** (−8 more) |

Direct-only temporal at 2.46 ms is already at the paper's 2.14 ms for *unified*
DI+GI. **The entire problem is the indirect path.**

Other:
- BLAS+TLAS build: ~0.1 ms each. Negligible. Not the cost.
- Resolution scaling: 720p→1080p **linear** (+125% px, +120% cost); 1080p→1440p
  **superlinear** (+78% px, +100% cost) → mild working-set/cache effect at 1440p.
  Rules out all fixed per-frame costs.
- Orb test scene: wall 2.55 ms / orbs in view 6.0 ms / mode 3 2.7 ms.
  Camera-direction-only comparison — the cleanest controlled experiment available.

---

## 3. Falsified — do not retry without new evidence

All measured against the ~2% noise floor.

| Change | Result |
|---|---|
| Light tiles (UPT-11) | neutral |
| Native uint4 light view (UPT-12) | 0.1 ms |
| Register reduction 108 → 96 | **negative** — compiler spilled, +50% warp latency, smem up |
| `r_pathTracingUnifiedPtAnalyticPortalDomain 0` | neutral |
| Opacity partition; `r_pathTracingHardwareOpaqueGeometry 0` vs `1` | neutral |
| Reconnection shift (`TemporalEarlyReconnect 1`) | −30% (⚠ possibly warm-up artifact, unretested) |
| Narrow rigid/skinned resolve + alpha-clip reorder | neutral |
| `r_useLightGrid 0` | neutral |
| Exclusive fullscreen, DLSS-RR off | neutral |
| §6.2.4 Russian roulette | N/A — needs 3+ bounces, renderer has one continuation |

**Also ruled out as causes:** BVH quality, buffer residency/heap placement, the
engine data feed, D3D12/DXGI interop (driver presentation path, 0.03 ms), WDDM
paging, cross-API contention, occupancy.

**Occupancy is NOT the lever.** Measured 37% / atpw 19.6 / coherence 61.4% —
already better than the paper's fully-optimized 34.9% / 20.6.

---

## 4. Confirmed causes

**Emissive surface sampling — the majority of it.** (Found by scene ablation, not
by profiling.) Mechanism: per-candidate CDF binary search + 16-probe reverse hash.

**T0 stage ladder** (`TemporalBottleneckProbe`, Lambert+compact64+duplication):

| Stage | ms | % |
|---|---|---|
| Receiver/history shell | 1.293 | 9.6 |
| First indirect replay | 0.968 | 7.2 |
| **Reciprocal/cross** | **6.488** | **48.1** |
| Reservoir merge | 0.343 | 2.5 |
| Final visibility | 2.454 | 18.2 |
| Production feedback difference | 1.955 | 14.5 |
| Total | 13.501 | 100 |

**Reciprocal subsplit** (3.556 ms total — ⚠ unexplained 2.9 ms gap vs 6.488):

| Sub-stage | ms | % |
|---|---|---|
| Identity resolution + setup | 0.756 | 21 |
| Second RayQuery traversal | 0.813 | 23 |
| Committed-hit surface decode | 0.767 | 22 |
| Post-hit target evaluation | 1.220 | 34 |

**No hotspot** — four sequential dependent memory steps. Ceiling on any single
sub-fix is ~1 ms. The reciprocal path is the MIS weight; `pairwiseMis` defaults
to **0**, so this cost is the *non-pairwise* path (`!basicSelectsHistory` branch).

---

## 5. The failure class (the important part)

**Materializing a relationship at consumption time that could be materialized at
production time.** One mistake, repeatedly:

| Site | Consumption-time work | Should be |
|---|---|---|
| Emissive selection | CDF binary search per candidate | pre-sampled / alias table |
| Emissive reverse lookup | 16-probe hash (instance,prim)→light index | index carried in hit/instance data |
| Rigid geometry | route record → indices → vertices (4 levels) | offsets from `InstanceID` arithmetic |
| MIS cross-eval | full `Upt07ReplayIndirectSample` + ray for one scalar | target from stored geometry |
| Reservoir | stores identity + seed | store position/normal/radiance |
| Alpha clip | full triangle + material resolve, then check 1 bit | check flag first (fixed) |

**Why systemic here:** the contracts-and-parity discipline. Reusing the
authoritative resolver guarantees identical results *and* guarantees the cost.
Every "call the real function so parity holds" is a candidate instance.

**GPU mechanism:** dependent scattered loads. Load N+1's *address* depends on load
N's *result*, so they cannot overlap — serialized latency. At 37% occupancy all
warps stall on the same chain simultaneously; there is nothing to switch to.

**Cost is TRANSACTIONS (distinct cache lines touched), not BYTES.** 32 lanes at
32 different addresses issue ~32 transactions whether the record is 112 B or
80 B. This is why narrowing records, removing a dependent level, and trimming
instructions all measured zero.

**Hardware signature:** heavy memory stalls with **LOW** DRAM/L2 throughput —
i.e. *"nothing is stressed."* Bandwidth-bound code moves data; this doesn't.

---

## 6. Detection

### CSV filters (Nsight shader-source export, needs `Active Threads Per Warp`)

| Filter | Meaning | Fix direction |
|---|---|---|
| `OpLoad`, warp latency > 1000, **atpw ≥ 24** | scattered addresses | data layout: flatten, pre-resolve, index |
| warp latency > 1000, **atpw ≤ 16** | control-flow divergence | compaction, sorting, reordering |
| `Instruction Mix ≥ 20`, atpw ≤ 8 or low samples | cold + huge | delete/gate; icache tax |

Cross with `Dependency-Attributed Samples` to separate **causes** from
**consumers** (consumers show high latency, dep≈0 — e.g. `OpIsNan` after a load).

Script: `scatter.py` (delivered 2026-08-11).

### Nsight Compute
`l1tex__average_t_sectors_per_request_pipe_lsu_mem_global_op_ld` —
**~4 = coalesced, ~32 = scatter.** Direct measurement of the transaction problem.

### Source audit greps
```
grep -rn 'while (low < high)\|for (uint probe' neo/shaders/slang_upt*/*.slang
grep -rn -oE 'g[A-Z][A-Za-z0-9_]+\[[a-z][A-Za-z0-9_]*\.[A-Za-z0-9_]+' neo/shaders/slang_upt*/*.slang
```
**Key refinement — it is frequency, not shape.** The *identical* CDF binary
search is correct in `upt04_light_tile_presample.slang` (~131k/frame) and wrong
in `upt04_initial_light_selection.slang` (~16M/frame). Always ask "how many
times per frame," never "is this a loop."

---

## 7. Captured shader facts (2026-08-09/10)

| | Direct D0 | Indirect D0 | Resolve |
|---|---|---|---|
| SASS instructions | 10,344 | 15,621 | 432 |
| RayQuery sites | 1 | 2 | 0 |
| Live regs max / weighted | 91 / 63.6 | **108 / 80.6** | 17 / 12 |
| % samples > 64 regs | 51% | **91%** | 0% |
| No Instructions | 8.0% | **40.5%** | — |
| Long Scoreboard | 33.8% | 20.6% | 61.3% |

- `OpBranchConditional` = **44–46% of SASS**; `OpSelect` = 0.8–1.0%. (§6.2.1 target.)
- Indirect cold bloat: 2,197 SASS (**14.1%**) with <200 samples each.
- Entering traversal at **76–83 live registers**; traversal adds ~20.

Hot instructions:

| Instruction | wl (cyc) | atpw | dep | mix |
|---|---|---|---|---|
| `OpLoad Upt04UnifiedLightRecord` (direct) | 7,206 | 32 | 24.2% | 54 |
| `OpRayQueryProceedKHR` (direct) | 4,207 | 15 | 43.4% | 266 |
| `OpRayQueryProceedKHR` ×2 (indirect) | 9,137 / 7,088 | 14 / 10 | 24.1% / 29.6% | 323 / 306 |
| `OpLoad Upt03PackedReservoir` (resolve) | 9,137 | 32 | **63%** | 38 |
| `OpLoad EmissiveLookupEntry` (indirect) | 56 | **1** | 0.03% | **256** |

**⚠ T0 was never captured.** All three exports are D0/resolve. Every conclusion
drawn from them was scoped to non-bottleneck passes — which is why the emissive
cost was invisible (0.1% samples, 170–208 cyc latency). **Capture every pass.**

**Lean primary hits 90% RT throughput, ~100% coherence, no significant stalls.**
Control experiment: proves BVH, data feed, buffers, and hardware are all fine.
It is fast because camera rays are coherent and there is no SM work wrapped
around the trace — not because of anything reproducible in secondary passes.
Realistic ceilings: primary 80–90%, shadow 40–60%, BSDF/reconnection 20–40%.

---

## 8. Reference implementations

### ReSTIR PT Enhanced (Lin/Kettunen/Wyman 2026) — Table 1, 1080p, RTX 5880 Ada

| Row | Total | Initial | Temporal | Spatial | DI & others |
|---|---|---|---|---|---|
| +Russian roulette | 16.52 | 5.21 | 2.24 | 3.83 | 5.24 |
| **+Unify DI & GI (§6.1)** | **13.04** | 6.47 | **2.14** | **3.43** | **1.00** |

- "ReSTIR DI and others" = the *entire separate ReSTIR DI pipeline*. Unification
  deletes it (5.24 → 1.00) while temporal/spatial **get slightly cheaper**.
- **§6.1 unifies the SHIFT, not just the reservoir.** A DI sample is a path whose
  reconnection vertex is at vertex 1, so the reconnection shift degenerates to the
  cheap DI shift. Same code path. We unified *storage* only — one reservoir, two
  shift mechanisms with 10× different cost.
- **§3 paired/reciprocal reuse is SPATIAL ONLY.** Does not apply to temporal
  (no reciprocal partner across frames). Do not chase it for T0.
- **§6.2.3 forced NEE reconnection** removes light sampling from random replay.
  Table 1: temporal 4.16→3.40, spatial 13.06→9.73.
- §6.2.4 RR: initial 12.56→5.21 — biggest single lever, but requires long paths.
- Their post-optimization occupancy: 34.9%, atpw 20.6, warp latency 347k→82k.
- Secondary NEE uses **inverse-square decayed candidate counts** by bounce index.
  `Upt04InverseSquareNeeTrialCount` exists; D0's secondary NEE does not call it.

### Cyberpunk 2077 (DXR capture)

| | CP2077 | Ours |
|---|---|---|
| Spatiotemporal kernel | **2,080 SASS**, max 70 regs, 1 `traceRay` | — |
| Light sampling in reuse pass | **none** | CDF search + hash probe |
| DI live regs max / weighted / >64 | 121 / **58.5** / **26%** | 108 / 80.6 / **91%** |
| Stalls | Long Scoreboard **57.8%**, No Instr 11% | No Instr 40.5% |
| `TraceRay Live State Bytes` | max 128 B, most 12–16 B | 76–83 regs ≈ 304–332 B |
| `TraceRay Spill Sites` | **0 everywhere** | — |

Higher *peak* registers than us but far leaner *average* — the distribution
matters, not the peak. Explains why forcing 108→96 backfired.

### RTXDI SDK

- `Rtxdi/PT/TemporalResampling.hlsli` — 435 lines library, 88-line pass shader.
  **No `RAB_TraceRay` anywhere in it.** Only `RAB_GetGBufferSurface`,
  `RAB_GetSurfaceWorldPos/Normal/LinearDepth`, `RAB_GetMaterial`.
- `RTXDI_PTReservoir` stores `TranslatedWorldPosition`, `WorldNormal`,
  `Radiance` — **decoded geometry, directly usable.** Jacobian is pure
  arithmetic. Ours stores identity + seed, requiring re-resolution to use.
  Same 64-byte budget, opposite choice. **This is why our reconnection attempt
  regressed: right algorithm, wrong data representation.**
- `getGeometryFromHit` (donut): 4 dependent levels — *same depth as ours*.
  The differences are: no route dispatch, **skinning baked into the vertex
  buffer before the BLAS build** (so animated meshes are byte-identical to
  static at trace time), `ObjectToWorld3x4()` from hardware instead of a loaded
  matrix, and attribute masking (fetch only what the caller asked for).

### RTX Remix

- `surfaces[uint(surfaceIndex)]` everywhere — flat array, **never searches in a
  shader**. `surfaceIndex = instanceCustomIndex(base) + geometryIndex`.
- `surfaceMapping[lastFrameSurfaceID]` — cross-frame identity is a **CPU-built
  dense array**, one indexed load. We have `gUpt04PreviousToCurrentLights`
  (same idea) but temporal indirect hash-probes instead of using it.

---

## 9. Codebase specifics worth remembering

- `Upt04ResolveTriangle` — 5-way route dispatch (staticBucket / id==0 / id==1 /
  skinned / rigid). Static = 1 memory level; **rigid = 4 dependent levels.**
- `hit.geometryId != 0` rejection is **intentional** — the backend canonicalizes
  `GeometryIndex*256 + PrimitiveIndex` in `upt04_backend_rayquery.slang` and sets
  geometryId=0. `RT_SMOKE_BLAS_GEOMETRY_TRIANGLE_CHUNK = 256` must match.
- Opacity partition works: cvar defaults 1, flips `opaque`/`programmable` counts
  correctly (verified in an opaque-only scene: programmable=0). Coverage in a
  real level is **still unmeasured** — read `geometry(v/i/t/cpuSurfaceRecords/
  descs/opaque/programmable/invalidRanges)` from the build log.
- Alpha clip is a **compile-time bool** per call site in
  `upt04_backend_rayquery.slang` (lines ~98, ~117) plus an ungated copy at
  `upt07_temporal_indirect.slang:245`. No cvar. `r_pathTracingDisableAnyHitAlpha`
  does **not** reach the UPT path.
- `r_pathTracingUnifiedPtFamily` defaults to **1 (direct-only)**;
  `TemporalIndirect` and `TemporalPairwise` default to **0**. Check before
  interpreting any measurement.
- Parity 0 **ignores** `configuredEmissiveTrials` (hardcodes 4+4). Any
  candidate-count A/B at parity 0 measures nothing.
- Parity 1 analytic trials = `min(analyticCount, 32)` — **scene-determined**, so
  candidate counts are not comparable across scenes.
- Resolve pass: `[numthreads(8,8,1)]` with `pixel.y*width+pixel.x` → a warp
  straddles 4 rows = 4 disjoint runs = 4× cache lines. Likely the cause of the
  9,137-cycle reservoir load at atpw=32. Same block shape is used across most
  UPT kernels.

---

## 10. Methodology lessons

1. **Ablation localizes; optimization doesn't.** Config ablations (frozen scene,
   family, orbs out of view) produced 30–60% deltas. Ten-plus code optimizations
   produced zero. Remove categories of work to find cost; only then make it cheaper.
2. **Dependency-attributed samples rank *within* a kernel, not *across* kernels.**
   Use pass timings to choose the kernel; use dependency attribution to choose the
   instruction inside it. Reversing this cost weeks on the smaller half of the frame.
3. **Capture every pass**, not the ones under suspicion. The evidence base was
   narrower than the problem for the entire investigation.
4. **atpw discriminates the two failure modes**, and they need opposite fixes.
   Latency alone sends you the wrong way.
5. **Peak registers is an output, not a target.** Forcing it down relocates state
   to local memory. Optimize the working set; registers follow.
6. **Code reading generated ~12 confident, specific, plausible hypotheses. Nearly
   all were falsified by measurement.** Weight source-inspection conclusions
   accordingly and let instrumentation choose targets.

---

## 11. Open questions

- **FrozenScene 2's 4–6 ms**: upload/barrier cache contention, or scene stability
  raising shift success rates? Test: stand still in mode 0, settled, vs mode 2.
  Also `TemporalRouteDiagnostics 1` mode 0 vs 2 to compare shift failure rates.
- **Mode 3's 8 ms**: emissive vs TLAS content vs route dispatch.
  Test: `r_pathTracingEmissiveInventoryMaxTriangles 1` in mode 2.
- **6.488 vs 3.556 ms** reciprocal discrepancy — unattributed 2.9 ms.
- **1.955 ms** production feedback difference — unattributed.
- Reconnection's −30%: real, or first-batch warm-up artifact? Re-run settled.
- Opacity coverage in a real level — never measured.

---

## 12. Standing recommendations

- Build **per-pass budget counters**: rays, searches, dependent loads, candidate
  returns — per pixel, declared vs measured. Every finding here would have been
  obviously wrong the day it landed.
- Keep the **stage ablation stubs** permanent, not one-off.
- Run `scatter.py` on every capture; add the cold-bloat filter.
- Before any optimization, answer: *is this scatter, divergence, or footprint?*
