Geometry modernization - validated plan and worker packet
==========================================================
This directory now contains two layers:

  00-04  Preliminary audit/research prepared before runtime validation.
         These remain useful background, but proposals in them are not
         automatically implementation requirements.

  05-08  Validated runtime evidence, decisions, worker rules, and the
         ordered execution plan. These documents control the work.

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
  tasks/README.txt                - ordered self-contained worker tickets
  artifacts/README.txt            - durable baselines, capture commands, field
                                    ownership, and completed-ticket handoffs

Start implementation with tasks/GEO-00_runtime_baseline.txt. The existing
runtime captures satisfy much of GEO-00, but the ticket must still establish
durable artifacts and the exact comm1 reproduction checkpoint before any
behavioral patch.

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
  GEO-13 has accepted checked dynamic/static cap removal, canonical rigid and
  static-bucket production defaults, clean-GI resident-bucket decoding, and a
  warm identical-content route comparison. The latest CPU checkpoint removes
  a duplicate whole-world material registration and archives the remaining
  disclosed route-one overhead. The final production-route matrix is archived.
  GEO-13's authorized rigid/static/cap/cleanup work is complete, but packet-wide
  Phase-E completion remains blocked by the revoked per-instance skinned
  update-BLAS route. Retained monolithic static, merged dynamic, and legacy
  rigid owners are intentional live fallbacks, not dead code.

  GEO-14 has now isolated the first skinned reset owner to a Vulkan BLAS UPDATE
  state-contract mismatch: the prior BLAS source read was absent from NVRHI's
  automatic barrier state. The fixed diagnostic route survives scripted
  movement with capture split off and on, while the full-BUILD control is
  rejected at about 4.45 ms. Batched source-read/read-write transitions reduce
  the correct UPDATE median from about 0.635 ms to 0.079-0.084 ms while
  preserving route and motion admission. Extended portal/view-set, two-map,
  spawn/despawn/respawn, replacement-retirement, and soak coverage also passes
  without a device/Vulkan/fatal error. A diagnostic one-shot armed after CPU
  capture omission cannot reconstruct its independent legacy shadow and must
  force one legacy-capture frame before the final audit matrix. Defaults remain
  off pending that correction and human visual acceptance.
