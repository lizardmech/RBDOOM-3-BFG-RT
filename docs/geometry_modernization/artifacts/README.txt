Geometry modernization - durable artifact index
================================================

PURPOSE
-------
This directory contains bounded, machine-diffable evidence produced by the GEO
tickets. Do not place complete console logs, screenshots, shader binaries, or
executables here.

GEO-00 FILES
------------
  baseline_2026-07-23.csv
    Accepted numeric baseline extracted from geometry_plan.log. `NA` means the
    original capture did not contain that field; it is not equivalent to zero.

  diagnostic_source_map.csv
    Maps every baseline field group to its current log prefix, source owner, and
    console trigger.

  capture_checklist.txt
    Exact bounded console sequence and scene requirements for refreshing the
    four baseline scenarios.

  GEO-00_handoff.txt
    Completion record, limitations, build status, and rollback.

GEO-01 FILES
------------
  GEO-01_instrumentation_handoff.txt
    Current-code contract audit, the discarded active-prefix hypothesis,
    one-shot capture command, output interpretation, build/deploy identity,
    and the runtime work still required to localize the earliest mismatch.

  GEO-01_runtime_capture_2026-07-23.txt
    Paired comm1 runtime tuple values, the discarded mistaken block, both
    shader samples, the localized opaque brush receiver, and the final retained
    cache versus static-BLAS traversal A/B.

  MONSTER_FIRE_composite_ab_2026-07-23.txt
    Frame-window A/B showing the stalls require the screen-space composite but
    not CPU sort or particle lighting, the aligned trace, and the descriptor-
    cache A/B which rules out per-frame binding creation as a required cause.

GEO-03 FILES
------------
  GEO-03_gpu_skinning_runtime_handoff.txt
    Vulkan binding fix, numeric parity coverage, invalid-history interpretation,
    geometry_geo18 identity evidence, and the logical-view ownership finding.

GEO-04 FILES
------------
  GEO-04_identity_contract_audit.txt
    Current producer/consumer audit; accepted WorldKey, MeshKey, InstanceKey,
    PrimitiveKey and HistoryOwnerKey tuples; record/lifetime/offset/emissive
    rules; shadow disagreement schema; and blockers delegated to GEO-05/06.

  GEO-04_handoff.txt
    Passive helper/harness coverage, build and deploy identity, unchanged live
    route/default status, rollback, and exact ownership blockers for GEO-05/06.

GEO-05 FILES
------------
  GEO-05_lifecycle_owner_audit.txt
    RenderWorld hook/teardown audit, initial per-world shadow owner and record
    contract, bounded dump schema, geometry_geo19/20 evidence, SMP snapshot
    restoration, and the callback-MD5 discovery prerequisite correction.

GEO-06 FILES
------------
  GEO-06_entry_audit.txt
    Current rigid storage/BLAS addressing audit, accepted first pooled-storage
    ABI, focused MD5 predecessor proof, ordered implementation slices, and the
    checked uint64 pool-planner plus immutable CPU source-registry harness
    handoffs. Records the correction that resolved material binding is
    instance/route state rather than immutable mesh-source content.

GEO-07 FILES
------------
  GEO-07_entry_audit.txt
    Renderer jointCache production/format/lifetime audit, rejection of the
    global last-view temporal bridge, staged compact GPU-joint decision,
    authoritative job/fallback contract, ordered implementation slices,
    geometry_geo39 entry acceptance, geometry_geo40 compact-plan acceptance,
    and the default-off authoritative current-joint GPU-copy comparison
    contract.

GEO-10 FILES
------------
  GEO-10_entry_audit.txt
    Historical implementation and runtime evidence for portal-area static
    buckets, including the rejected parallel route-table/SBT design.

  GEO-10_architecture_pivot_2026-07-27.txt
    Active architecture override. Keeps bucket assignment/BLAS/TLAS work but
    routes the resident pool through existing static shader slots, encodes the
    triangle base in InstanceID, removes contribution 4/5 and compact hit
    libraries, and moves resident-pack caching before live acceptance.

GEO-11 FILES
------------
  GEO-11_entry_audit.txt
    Validated graphics-submission and NVRHI lifetime contract, concrete
    static-bucket premature-release gap, result-versus-scratch budget
    capability, current Vulkan compaction rejection, and ordered bounded
    implementation slices.

GEO-12 FILES
------------
  GEO-12_entry_audit.txt
    Live attribute producer/consumer and ABI audit, canonical-registry coverage
    limit, first observation-only paged survey contract, rejected assumptions,
    and ordered completion/encoding slices.

  GEO-12_rendered_capture_survey.txt
    Final rendered static/dynamic ownership and identity contract, both-UV-pair
    paged survey, prepared-save and representative coverage evidence, class
    census, and the measurements that close candidate selection.

  GEO-12_first_encoding_selection.txt
    First single-candidate decision, exact color-UNORM8 eligibility/fallback
    contract, CPU-only storage boundary, unchanged transport/GPU ABI, pure
    acceptance, and ordered runtime A/B.

GEO-13 FILES
------------
  GEO-13_entry_audit.txt
    Checked cap-removal boundary, route/default prerequisites, and ordered
    cutover slices.

  GEO-13_slice6_transitive_cleanup_audit.txt
    Transitive live-consumer proof, retained rollback/fallback owners, clean-GI
    resident-bucket decode repair, Vulkan-only shader validation, and runtime
    acceptance.

  GEO-13_final_performance_2026-07-30.csv
    Machine-diffable historical baseline, identical-content route-zero control,
    pre-fix route-one candidate, and optimized route-one timing/memory rows.

  GEO-13_final_performance_handoff.txt
    Exact warm A/B method, disclosed remaining overhead, memory accounting,
    post-fix route/emissive audit, raw-log provenance, and build/deploy identity.

  GEO-13_final_correctness_matrix_2026-07-30.csv
    Final production-route matrix for static, skinned fallback, comm1,
    lifecycle, transitions, GPU fallback, UV disposition, and old-cap stress.

  GEO-13_final_correctness_handoff.txt
    Current comm1 exact-route/screenshot evidence, route-zero shutdown control,
    final defaults and retained fallbacks, and the remaining packet blocker.

GEO-14 FILES
------------
  GEO-14_update_barrier_2026-07-30.csv
    Machine-diffable full-BUILD control, historical UPDATE baseline, and
    post-fix barriered UPDATE timing/correctness rows.

  GEO-14_update_barrier_handoff.txt
    Vulkan UPDATE state-contract root cause, full-BUILD rejection, fixed-route
    movement/capture-split evidence, unchanged defaults, raw-log provenance,
    and the remaining promotion matrix.

  GEO-14_batched_update_2026-07-30.csv
    Machine-diffable correct per-BLAS versus batched-barrier UPDATE timing and
    capture-split correctness rows.

  GEO-14_batched_update_handoff.txt
    Two-phase validated job-list design, two-batch barrier contract, movement
    evidence, performance recovery, unchanged defaults, and next matrix gates.

  GEO-14_extended_matrix_2026-07-30.csv
    Machine-diffable portal/view-set, map-transition, spawn/despawn/respawn,
    route/motion/capture-split, retirement, and error results.

  GEO-14_extended_matrix_handoff.txt
    Extended matrix interpretation, raw-log provenance, the post-admission
    consumer-audit timing limitation, unchanged defaults, and final promotion
    gates.

NAMING
------
Use:

  GEO-XX_<scenario>_<YYYY-MM-DD>.<ext>

Future CSV files must preserve existing column meanings. Add a new column rather
than silently changing the interpretation of an old one. Use decimal integers,
plain decimal milliseconds, `NA` for absent data, and `0` only for a measured
zero.

LARGE RAW LOGS
--------------
Keep raw logs outside git and record their absolute path, byte size, timestamp,
executable identity, and the labels used to derive the bounded artifacts.

GEO-00 source:

  C:/Users/lizard/Saved Games/id Software/RBDOOM 3 BFG/base/geometry_plan.log
  bytes: 2778132
  modified: 2026-07-23 01:06:53 Australia/Sydney
  executable: RBDOOM 3 BFG 1.6.0.1403 win-x64 Jul 22 2026 00:29:49
