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
