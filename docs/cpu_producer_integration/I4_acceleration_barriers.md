# I4: skinned update synchronization and AS attribution

Follow-up: the user confirmed I4 fixed the AS regression on 2026-09-07. The remaining approximately 0.2 ms post-TLAS copy work is tracked separately in I5_light_upload_reuse.md. The original validation record below is retained.

2026-09-07. Parent candidate: I3, e205cb937. User reports hundreds of consecutive vkCmdBuildAccelerationStructuresKHR calls in every settled frame, costing about 4 ms, versus two in the old executable. The supplied screenshot selects a 0.78 ms individual call. The reported aggregate cost has not been independently measured.

## Source finding and bounded change

The rewrite updates one BLAS per joined skinned mesh. Its warm update loop previously left automatic barriers enabled. The current Vulkan NVRHI backend prepares each UPDATE source in AccelStructBuildBlas state, commits barriers, then prepares its combined AccelStructBuildBlas | AccelStructWrite state and commits again. Repeating that sequence inside the loop introduces AS-stage synchronization between independent mesh updates.

The existing legacy skinned comparison path in PathTraceSmokeSceneBuild.cpp already prepares update dependencies together before issuing its mesh updates with automatic barriers temporarily disabled. I4 applies that approach to the rewrite's warm skinned updates. It transitions every private index buffer to build input, prepares all prior BLAS source reads, commits, prepares all combined read/write destinations, and commits. An RAII guard restores automatic barriers before later scene/TLAS submission, including exception unwinding.

Each package mesh owns a distinct BLAS. Initial allocation creates one per row; replacement allocation reuse marks each previous row used once. The backend suballocates distinct scratch ranges in the command-list recording version. The shared skinned output remains read-only during AS updates. The automatic TLAS path still transitions all referenced BLAS outputs before the TLAS build. Geometry, joints, mesh membership, update flags, refresh cadence, slot selection, retirement, and history are unchanged.

This reduces the warm loop's per-mesh barrier preparation to two package-wide batches. It does not merge the BLASes or reduce the API build-call count to two. Full replacement and periodic refresh builds retain their previous automatic-barrier path. Neither rigid-cache churn nor unexpected skinned replacement frequency has been ruled out by runtime counters yet.

Vulkan requires synchronization of AS source/destination accesses and scratch use; independent work must not alias writable allocations. See the [Vulkan build command reference](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdBuildAccelerationStructuresKHR.html). The change preserves those dependencies while moving their preparation before the independent update loop.

## Attribution

With r_pathTracingNsightGpuMarkers=1, the candidate labels CPURewrite.Skinned Skinning; CPURewrite.Skinned BLAS Update, Refresh, or Replace; and CPURewrite.Rigid BLAS Cache Miss. The skinned AS label includes the batched synchronization. Rewrite static/TLAS submission now enables the existing acceleration helper labels. Automatic Optick captures include TAGS, exposing existing skinnedFullBuilds, skinnedUpdates, rigidColdBlasCount, layout mismatch and commit counters, plus skinnedUpdateBarrierBatches.

## Validation and deployment

- Vulkan-only Release build passes; no shader or NVRHI submodule changes.
- Complete CTest run: 35/36 passed. The CPU producer harness reported four timing-sensitive latest-complete mailbox assertions while the diagnostic applications were running. Its isolated rerun passed in 0.88 seconds. The rewrite harness passed in the complete run. No assertions were changed.
- Protected Discovery SHA256 remains EC769D35F156DD21184CBBA4A7E149FEF46E3FB7628DD53F69806DD82806C404.
- Two automatic hidden-window native probes reached initialization but did not execute the scene script. Both task-created game processes were stopped and saved settings/log restored. These probes provide no scene correctness or GPU performance evidence.
- Separate launcher: E:/prog/rbdoom-3-BFG-prebuilt_cpu_integration/launch-rewrite-I4-AS.cmd. Existing I3 launchers remain available and unchanged. Fail-on-recovery remains enabled in I4.
- EXE: RBDoom3BFG.cpu-integration-I4-11FCA5E6-20260907.exe, SHA256 11FCA5E6D3598D45D8FE411D8C9E7F20F1CD4B2F476730115912BBF3CE51AAC6. Matching MAP SHA256 8F1B611DE17B4C3479B10B64C87FC48C74C6C282C5DA0B4A2C12DD675247431B.
- Evidence: E:/prog/cpu-producer-integration-20260907/I4-as-attribution, including build/test logs and receipt.json.

Acceptance remains pending: capture the same settled Mars City view with I3 and I4, matching effective renderer settings, and compare the complete AS span. Record whether the hundreds of calls belong to Update, Replace, Refresh, or rigid cache misses. The screenshot also contains a split indirect UPT dispatch; ensure both captures use the same effective UPT family and passes. No GPU saving or complete resolution of the reported regression is claimed yet.
