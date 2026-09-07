# I5: skip unchanged rewrite lighting uploads

2026-09-07. Parent: I4 (4ef597cb7). The user confirmed that I4 fixed the AS regression. The remaining reported difference is about 0.2 ms of copy work immediately after the TLAS, with approximately 30 GB/s PCIe burst throughput. The I4 screenshots are runtime evidence from the user; I5 savings have not been measured.

## Finding

FinishRewriteLighting unconditionally called writeBuffer for 19 current/previous lighting, remap, emissive geometry, distribution and lookup buffers after TLAS submission. Buffer allocations were reused per idle slot, but uploaded contents were not compared. This is a source-confirmed omission; the screenshot alone does not identify every copy in that interval. Rigid route instance and other changing uploads can also remain there.

## Change

The lighting slot now retains exact bytes, following the existing material-slot comparison pattern. Upload reuse requires the same GPU buffer handle, identical byte length, and memcmp equality. Each of the three idle slots warms independently. New buffers, changed values and changed sizes upload normally. Empty inputs retain the existing one-record zero upload contract.

Cache receipts are limited to 8 MiB per slot (24 MiB retained across three slots, plus up to 8 MiB of candidate receipts during an attempt). Buffers beyond the per-attempt cache budget continue uploading normally. Warm unchanged frames compare bytes without copying them into new receipt vectors. This adds bounded CPU memory and byte-comparison work; the total CPU/GPU tradeoff still needs runtime verification.

Before recording a changed upload, allocate its candidate receipt, then invalidate and release the old slot receipt. If a later upload or scene validation rejects the attempt, the invalidated receipt cannot authorize reuse of stale bytes. Only successful scene commit publishes candidate receipts. Unchanged receipts move to the committed package without copying. Existing route/lifecycle reset clears the slot buffers and receipts together. The 19 buffers are shader inputs; their shader-resource bindings and CPU upload ownership are unchanged.

Nsight marker: CPURewrite.Lighting Upload. Optick counters: lightUploadBytes, lightUploadReusedBytes, lightUploadReusedBuffers, lightUploadCachedBytes. This addresses redundant lighting writes only; it does not change lighting values, temporal history/remap generation, GPU shaders, BLAS work, or the accepted I4 synchronization change.

## Validation and deployment

- Vulkan-only Release build passes. Existing code-page warnings remain.
- All 36 CTests pass in 22.54 seconds. No tests or assertions were changed. These checks do not substitute for a native lighting/temporal comparison.
- Reviewed handle/size/byte mismatch, first-use and zero-record cases, candidate-allocation failure, post-recording rejection, and lifecycle reset against the actual upload and publication order.
- Protected Discovery SHA256: EC769D35F156DD21184CBBA4A7E149FEF46E3FB7628DD53F69806DD82806C404.
- Separate launcher: E:/prog/rbdoom-3-BFG-prebuilt_cpu_integration/launch-rewrite-I5-uploads.cmd. I4 remains available. Fail-on-recovery and GPU labels remain enabled.
- EXE SHA256: EF03F967B9E3D8DBEC082A043725D8EDC584AFC567A06523E39753BB6FC4AB5E. Matching MAP: 558BA277560741ABA34B1854C6E60A407BEAFC694DBC41A646D1B83ADE9371D1.
- Evidence: E:/prog/cpu-producer-integration-20260907/I5-light-uploads/build.log, tests.log and receipt.json.

Runtime acceptance remains pending. Compare the same settled view after all three slots warm, then walk/change visible lights to confirm updates still appear normally. The PCIe burst rate is not a per-frame byte count; use the uploaded/reused byte counters and the complete GPU upload interval to judge the change. No recovery of the entire reported 0.2 ms is claimed.
