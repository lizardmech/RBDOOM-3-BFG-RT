Geometry modernization - worker ticket index
=============================================
Status: implementation complete; final acceptance reopened, 2026-07-30.

REQUIRED READING
----------------
Before taking any ticket, read:

  ../README.txt
  ../05_validated_runtime_findings.txt
  ../06_decision_register.txt
  ../07_worker_protocol.txt
  ../08_execution_plan.txt
  the assigned GEO-XX ticket

The older 00-04 documents are proposal/research inputs. They are useful context,
but they do not override the validated findings, decision register, protocol,
or active ticket.

EXECUTION ORDER
---------------
Phase A - correctness and measurement

  GEO-00_runtime_baseline.txt
  GEO-01_comm1_static_contract_instrumentation.txt
  GEO-02_comm1_narrow_static_fix.txt
  GEO-03_gpu_skinning_observability_and_parity.txt

Phase B - identity, ownership, and storage

  GEO-04_canonical_mesh_instance_contract.txt
  GEO-05_per_world_lifecycle_shadow_registry.txt
  GEO-06_pooled_mesh_ingestion.txt

Phase C - true-deforming geometry

  GEO-07_authoritative_jointcache_gpu_skinning.txt
  GEO-08_per_instance_skinned_blas.txt
  GEO-09_skinned_temporal_material_emissive_promotion.txt

Phase D - static world and scheduling

  GEO-10_portal_area_static_buckets.txt
  GEO-11_as_budget_lifetime_compaction.txt
  GEO-12_attribute_layout_measurement.txt

Phase E - cutover

  GEO-13_cutover_cap_removal_cleanup.txt

Phase E recovery - completed skinned cutover

  GEO-14_skinned_vulkan_update_recovery.txt

Current Phase-E status (2026-07-30):
  The production-default acceptance is reopened by D192. The diagnostic
  clean-GI warmup cap was left at zero after the complete 16/16 lane had
  already passed, making normal-launch GI effectively off. The default is now
  restored to 16 and autonomous dispatch proof passes. Interactive
  normal-launch GI, movement, portal, and glass validation remains required.

  GEO-13's authorized rigid/static/cap/cleanup/performance work is complete.
  GEO-14's batched in-place UPDATE route has passed its automated lifecycle,
  live/shadow consumer, hit/material/motion, and emissive publication matrix.
  A supplemental same-camera screenshot A/B also finds no candidate-only
  structural or material regression. The final human pass reports stable
  traversal; the remaining older emissive artifact is unchanged by
  capture-only and full-route rollback. The per-instance route and capture
  split are production defaults, with merged dynamic retained as the exact
  zero/zero rollback. See
  ../artifacts/GEO-14_final_consumer_handoff.txt.

CLAIMING A TICKET
-----------------
1. Verify every predecessor's DONE WHEN section and checkpoint commit.
2. Record the current branch, head, and dirty files.
3. State the ticket ID in the working notes and keep edits inside its scope.
4. If a prerequisite is missing, stop; do not silently absorb its work.
5. Use the ticket's named gate for every new live route.

COMMON ACCEPTANCE RULES
-----------------------
- Vulkan only. Do not build, validate, copy, or modify DX12/DXIL output.
- Use `cmake --build --preset win64-pt-dev-release` from `neo/`.
- New routes stay default off until their promotion ticket.
- A screenshot can supplement, but not replace, numeric contract evidence.
- Same-content comparison is mandatory for performance claims.
- No ticket may hide missing geometry behind a fallback or raised cap.
- Preserve unrelated worktree changes and use coherent checkpoint commits.

HANDOFF
-------
The handoff must state: outcome, changed files, gate/default, build and SPIR-V
proof, exact runtime tuple, rollback, remaining blockers, and checkpoint commit.
Use the evidence schema in ../07_worker_protocol.txt.
