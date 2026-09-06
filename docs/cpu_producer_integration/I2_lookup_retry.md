# I2: bounded emissive lookup allocation retry

The I1 native mars_city1 rewrite produces 4096 emissive records with unique (instanceId, primitiveIndex) identities. The current CPU lookup builder retries only 2x and 4x table capacities. The captured records need 29 and 20 probes respectively, exceeding the unchanged shader limit of 16. At 8x capacity the same identities require at most 16 probes. The worker correctly marks its 2-entry failure lookup inexact, causing current main UPT to reject the scene.

This resolves the previously unknown semantic boundary: no duplicate identity or invalid resource is being accepted. I2 changes only bounded CPU allocation retries; shader hashing, 16-probe search, exactness, PDF/identity validation and publication stay intact.

Allowed paths: neo/renderer/NVRHI/PathTraceUnifiedLight.cpp; neo/tests/pathtrace_remix_light_manager_harness.cpp; a compact identity fixture under neo/tests/fixtures; this integration documentation. The temporary diagnostics in UnifiedPt, UnifiedLight and SmokeSceneBuild are restored before the final build.

1. Add a compact fixture preserving all 4096 native identity pairs in dense order. Exercise the actual production builder through the existing RemixLightManager harness and simulate the existing shader lookup to verify every identity, dense index and conditional PDF within 16 probes. Retain negative checks for duplicate hit identities and malformed CDFs.
2. Demonstrate that the regression fails before the fix. Add two bounded capacity retries at 8x and 16x to the existing 2x/4x attempts. Keep failure behavior when no table fits.
3. Run the affected light-manager harness, build Vulkan Release, and repeat the same native rewrite scene with UPT settings intact. Check a short legacy route run and preserve source/runtime receipts. No performance claim follows from these tests.
4. Keep the integration fix separate from structural cleanup. Obtain user image/event acceptance before main promotion.

Evidence: lookup-light-identities.csv, lookup-insertion-details.txt and lookup-collision-analysis.json in E:/prog/cpu-producer-integration-20260907. The first two failures are dense 2658 at (1077,267), capacity8192; and dense2797 at (1081,22), capacity16384. Neither is a duplicate. The shader accepts a power-of-two logical capacity <= physical count and obtains its mask from that capacity (slang_upt04/upt04_emissive_lookup.slang); allocation size does not change the shader ABI.