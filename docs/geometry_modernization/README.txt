Geometry modernization - validated plan and worker packet
==========================================================
This directory now contains two layers:

  00-04  Preliminary audit/research prepared before runtime validation.
         These remain useful background, but proposals in them are not
         automatically implementation requirements.

  05-09  Validated runtime evidence, decisions, worker rules, the ordered
         execution plan, and the post-acceptance capture/cache architecture
         assessment. These documents control the work.

  tasks/ Self-contained implementation tickets. Execute them in the order
         listed by tasks/README.txt. Do not skip a phase gate because a
         later ticket looks independently implementable.

Authority order when documents disagree:

  1. A concrete current-code contract or a reproduced runtime result.
  2. The active GEO-* worker ticket.
  3. 05_validated_runtime_findings.txt and 06_decision_register.txt.
  4. 07_worker_protocol.txt and 08_execution_plan.txt.
  5. Preliminary ideas in 00-04.

The key correction to the preliminary diagnosis is load-bearing: the
reproduced comm1 level-geometry disappearance occurs with no geometry-cap
overflow and only in PT. Hard-cap removal is still required, but it is not
accepted as the cause of that failure without a traced contract mismatch.

Reading order:
  00_current_state_findings.txt   - audit of the three coexisting routes,
                                    hard caps, per-frame costs, and the
                                    mechanisms behind each observed symptom
  01_target_architecture_ideas.txt- mesh/instance registry, pooled
                                    compressed geometry, per-mesh BLAS +
                                    refit, async builds, OMMs; what NOT to
                                    build
  02_gpu_skinning_findings.txt    - why the old GPU skinning attempt
                                    T-posed (volatile joint source + silent
                                    gates), and the jointCache-based fix
  03_mega_geometry_forward_compat.txt - CLAS/RTXMG later phase; the seams
                                    the conventional system must keep clean
  04_open_questions.txt           - decisions to settle by spike or
                                    measurement before ticketing; risks
  05_validated_runtime_findings.txt - exact in-game evidence gathered on
                                      2026-07-23, including the PT-only
                                      comm1 failure, CPU skin cost, crowd,
                                      GPU-mode, and lifecycle results
  06_decision_register.txt        - accepted decisions, deferred decisions,
                                    and the disposition of Q1-Q18
  07_worker_protocol.txt          - branch, scope, build/deploy, shader,
                                    evidence, checkpoint, and rollback rules
  08_execution_plan.txt           - phase order, dependencies, and phase gates
  09_capture_cache_architecture_divergence.txt
                                  - measured cases where frame-local capture,
                                    validation, copying, and route-specific
                                    caches diverge from persistent
                                    mesh/surface/instance reference designs;
                                    target invariants for follow-up work
  tasks/README.txt                - ordered self-contained worker tickets
  artifacts/README.txt            - durable baselines, capture commands, field
                                    ownership, and completed-ticket handoffs

The historical implementation entry point was
tasks/GEO-00_runtime_baseline.txt. The ordered packet has since completed
through GEO-14; do not restart a completed ticket unless its recorded reopen
conditions are met. Use the current execution status below and the decision
register for follow-up work.

Related prior work:
  docs/geometry_classifier/            (classifier + remix pivot history)
  docs/geometry_classifier/async_bvh/  (async + residency-first research)
  docs/geometry_classifier/entitydef_feed/ (entityDef producer design)

External reference:
  E:\prog\references\DoomRTX-main - working idTech4 DXR path tracer with
  entity-lifecycle BLAS/instance residency (push-model hooks), per-world
  double-buffered TLAS, and non-blocking same-queue BLAS builds. Cited
  with file/line pointers in 01 (reference section) and 04 (Q10/Q12/Q18).
  Reference for lifecycle/scheduling shapes only - its skinning, light
  transport, and storage are behind this plan.

Platform boundary:
  - Vulkan is the only runtime target for this packet.
  - Do not compile, validate, or deploy DX12 shaders.
  - Deploy rebuilt shader blobs from base/renderprogs2/spirv only.

Current execution status (2026-07-30):
  FINAL ACCEPTANCE REOPENED. The completed implementation packet left
  `r_pathTracingCleanRestirGiPipelineWarmupLimit` at the diagnostic value zero,
  which prevents every GI ray pipeline from being built even though GI enable
  and resolve default to one. The accepted production default is restored to
  16. Build/deploy and autonomous default-dispatch proof pass; ordinary
  interactive launch, visible GI, movement, portal, and glass validation are
  still required before the packet may be called complete again. See D192 and
  artifacts/GEO-GI_default_restore_handoff_2026-07-30.txt.

  GEO-13 has accepted checked dynamic/static cap removal, canonical rigid and
  static-bucket production defaults, clean-GI resident-bucket decoding, and a
  warm identical-content route comparison. The latest CPU checkpoint removes
  a duplicate whole-world material registration and archives the remaining
  disclosed route-one overhead. The final production-route matrix is archived.
  At the GEO-13 checkpoint, packet-wide Phase-E completion was still blocked
  by the revoked per-instance skinned update-BLAS route. GEO-14, summarized
  immediately below, superseded that blocker and completed the cutover.
  Retained monolithic static, merged dynamic, and legacy rigid owners are
  intentional rollback/fallback paths, not dead code.

  GEO-14 has now isolated the first skinned reset owner to a Vulkan BLAS UPDATE
  state-contract mismatch: the prior BLAS source read was absent from NVRHI's
  automatic barrier state. The fixed diagnostic route survives scripted
  movement with capture split off and on, while the full-BUILD control is
  rejected at about 4.45 ms. Batched source-read/read-write transitions reduce
  the correct UPDATE median from about 0.635 ms to 0.079-0.084 ms while
  preserving route and motion admission. Extended portal/view-set, two-map,
  spawn/despawn/respawn, replacement-retirement, and soak coverage also passes
  without a device/Vulkan/fatal error. Consumer/hit one-shots now force one
  legacy-capture frame, so a delayed post-admission audit also passes 12 live
  and 12 independent shadow routes covering 15,962 triangles with every
  failure bucket zero. The final automated consumer matrix also passes live
  and shadow consumers, 318 comparable hit/material/motion pairs, 772/772
  current and previous emissive records, and 24/24 current and previous
  publication records with every mismatch bucket zero.
  a supplemental same-camera screenshot A/B also shows no candidate-only
  structural or material regression. The final human pass reports stable
  traversal, and the pre-existing emissive artifact is unchanged by both
  capture-only and full-route rollback. Its separate follow-up identifies
  stochastic any-hit coverage as the owner and accepts deterministic
  additive-emissive receiver blend-through as the production default. The
  synchronized skinned route and capture split are now production defaults
  with zero/zero rollback retained.

  A post-acceptance Optick performance pass then removed two repeated CPU
  geometry paths without changing those defaults. The skinned route no longer
  rebuilds identical source-only hit metadata after submission when its full
  live contract remains exact. Stable rigid meshes no longer reconvert their
  local payload before an exact-BLAS check or invalidate content on transient
  `srfTriangles_t*` wrapper churn. `PT Build Scene` improves from 40.995 to
  37.502 ms/frame in the character scene and from 81.517 to 29.691 ms/frame in
  `testmaps/test_gltf2_models`. Final human visual confirmation is pending.
  See D193 and
  artifacts/GEO-14_cpu_geometry_performance_2026-07-30.txt.

  A post-acceptance soak found one narrower static-route exception: live clean
  GI plus refracted glass PSR can device-remove under route-one portal churn,
  while route zero, straight transmission, and reflection-only controls
  survive. Production now fails closed to retained monolithic static traversal
  only for that intersection. Route one remains the default and remains
  admitted for the separately accepted consumers. See D184 and
  artifacts/GEO-13_gi_refracted_route_guard_2026-07-30.txt.

  The first post-packet open storage question is also closed. A numeric survey
  rejects deriving posed skinned normal/tangent data from bind-triangle deltas
  in hit shaders: articulated surfaces show double-digit RMS error while
  single-bone controls remain exact. The production skinning compute continues
  to write the full posed basis once. See D185 and
  artifacts/GEO-12_q2_write_vs_derive_2026-07-30.txt.

  Q3 is closed as well. A controlled paired Vulkan timing run found no
  measurable BLAS-build penalty from a non-zero index-buffer offset after
  alternating build order. The shared uint32 index pool remains the accepted
  layout. See D186 and
  artifacts/GEO-06_q3_offset_blas_timing_2026-07-30.txt.

  Q4's required consumer audit found no live geometry scene-origin anchor to
  migrate. Legacy capture is world-space, canonical geometry is local-space
  with TLAS transforms, and shaders receive no `sceneOrigin`. The capture/order
  anchor is renamed and the dead always-zero CPU signature field is removed.
  See D187 and
  artifacts/GEO-04_q4_scene_origin_consumer_audit_2026-07-30.txt.

  Q7 is closed without adding a special single-bone production route. The
  classifier is conservative and remains available as metadata, while
  single-bone surfaces continue through the accepted GPU-skinning and
  per-instance BLAS path. They represent 2.98 percent of surveyed skinned
  vertices, and no profile isolates a payoff for another transform/history/AS
  ownership branch. See D188 and
  artifacts/GEO-07_q7_single_bone_route_audit_2026-07-30.txt.

  Q18 is closed without main-TLAS double-buffering. The fixed TLAS rebuild and
  all consumers are ordered on the graphics queue, NVRHI supplies the required
  read/write transitions and command-versioned instance uploads, and actual
  handle replacement remains event-query retired with its scene package.
  Nested BLAS retirement remains application-owned. See D189 and
  artifacts/GEO-11_q18_tlas_inflight_lifetime_audit_2026-07-30.txt.

  GEO-12's accepted lossless canonical `color` UNORM8 storage is now the
  production default. The current-head prepared-save census again packs
  988/988 records, saves 779,832 retained CPU bytes, and reports zero fallback
  records while transport and GPU attributes remain full float. CVar zero plus
  map reload is the rollback. See D190 and
  artifacts/GEO-12_color_unorm8_default_promotion_2026-07-30.txt.

  Q15 is now fully closed. The original spawn capture proved that the
  Fatzombie's first-seen material IDs entered the universe; a current-head
  audit now finds the Fatzombie in the live table and reports current safe
  handles for all 402 entries that expose diffuse images. The only five
  missing rows have no diffuse image by their sky/black/effect/glass
  contracts. See D191 and
  artifacts/GEO-Q15_material_universe_closure_2026-07-30.txt.
