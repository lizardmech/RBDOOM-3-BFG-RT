# I3: recovery failure diagnostics

User mergebug.opt (5.494 seconds, 74 CPU frames) records routeBoundaryBefore/After=0 and rewriteRecoveryReason=4 throughout. Embedded source paths identify the integration checkout. MainThread PT Build Scene median=66.153 ms, Capture Doom Surfaces=40.604 ms; GPU DrawView_3D median=13.467 ms. The capture begins after the failure. Source reason4 is the existing 120-consecutive-rejected-scene threshold; it does not identify which rejection site caused it. The user was walking around Mars City.

The user explicitly requests that emergency recovery crash/stop the game instead of switching to serial. I3 adds an opt-in fail-on-recovery diagnostic, enables it in the candidate launchers, and records the rejecting source line at the existing threshold. Fatal handling occurs at the main-thread idle boundary before ApplyRouteAtFrameBoundary can drain/promote legacy. Normal temporary keep-last handling and the 120-frame threshold remain unchanged. Source default of the new diagnostic is off; the candidate launcher selects it on. The original legacy/recovery implementation remains available when the diagnostic is off.

Allowed edits: PathTraceCVars.cpp/.h; PathTraceCpuProducerRewrite.cpp/.h (non-consuming pending reason inspection and idle-boundary fatal); PathTraceSmokeSceneBuild.cpp (rejection site telemetry and threshold warning); the existing rewrite harness recovery case; these docs and candidate-only launcher/deployment files. No geometry, lighting, shader, ownership, retirement or scheduling algorithm changes.

1. Add the diagnostic switch and inspect pending/latched recovery without consuming it. Report reason at the fatal boundary, and report source line/root/family at persistent rejection.
2. Preserve the first pending reason and existing off/on/map-reset recovery behavior in focused tests. Build Vulkan Release; use the complete test suite after the shared header/source rebuild.
3. Keep I2 EXE/MAP and launcher snapshots as rollback. Deploy a distinct I3 EXE/MAP with matched shaders, same launchUTP renderer flags, fail-on-recovery=1 and immediate logfile flushing. Verify deployment and flags.
4. Use the next Mars City failure's console log/rejection site to diagnose the underlying bug. Do not label the bug fixed merely because fallback becomes fatal. Do not disable safeguards or auto-retry forever.
## Built candidate

Source checkpoint aea3f841c78362613f2d622f379c1388781d5b88. Vulkan Release build passes. The rebuilt rewrite harness passes in 19.62 seconds, including non-consuming inspection/first-reason preservation. The other 35 registered tests pass; all 36 are green. The protected Discovery hash remains unchanged. The pending/latched reason check precedes the call that starts the legacy drain.

Deployed under E:/prog/rbdoom-3-BFG-prebuilt_cpu_integration:

- RBDoom3BFG.cpu-integration-I3-E310E461-20260907.exe, SHA256 E310E4616B1F2679DD2A823F641BB278D52E224CF2723674116DDE334C5A0919.
- Matching MAP, SHA256 5981FAEDC09C972602DDA353A1C5109DC137B6B7753DB4A3DB72ABEA40FCBDD0.
- launch-rewrite.cmd selects rewrite=1, fail-on-recovery=1 and logFile=2. launch-legacy.cmd selects explicit rewrite=0 for intentional legacy use. Both retain the corrected original launchUTP renderer flags. launch-rewrite-I2-rollback.cmd and I2 EXE/MAP are retained.

On the 120th consecutive rejection, the backend reports PathTraceSmokeSceneBuild.cpp:line, scene root, product root, keep-last family and skinned reason, then requests recovery4. The next idle boundary prints the fatal reason and calls the engine FatalError handler before normal recovery can switch to legacy. The logfile at base/qconsole.log is flushed after every print. Other capacity recovery reasons also stop at that boundary when the diagnostic is enabled. This is a controlled game fatal error, not an intentional invalid memory access.

The complete triggering walk is not reproduced by the existing capture, which starts after fallback. The underlying rejection remains unresolved. The next matching Mars City failure must supply the rejection-site log; do not interpret fatal-on-recovery as a correctness fix.

Capture analysis and I3 build/test/deployment receipts: E:/prog/cpu-producer-integration-20260907/mergebug.
Startup probe confirms rewrite=1, fail-on-recovery=1, UPT family=1 and logFile=2, then exits normally. The fatal path has not yet been exercised by the user's triggering walk. Each launcher copies the previous session log to base/qconsole.previous.log before starting. I3-startup-qconsole.log preserves the probe evidence.
